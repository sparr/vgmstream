#include "vgmstream.h"
#include "mixing.h"
#include "plugins.h"
#include <math.h>


/**
 * Mixing lets vgmstream modify the resulting sample buffer before final output.
 * This can be implemented in a number of ways but it's done like it is considering
 * overall simplicity in coding, usage and performance (main complexity is allowing
 * down/upmixing). Code is mostly independent with some hooks in the main vgmstream
 * code.
 *
 * It works using two buffers:
 * - outbuf: plugin's pcm16 buffer, at least input_channels*sample_count
 * - mixbuf: internal's pcmfloat buffer, at least mixing_channels*sample_count
 * outbuf starts with decoded samples of vgmstream->channel size. This unsures that
 * if no mixing is done (most common case) we can skip copying samples between buffers.
 * Resulting outbuf after mixing has samples for ->output_channels (plus garbage).
 * - output_channels is the resulting total channels (that may be less/more/equal)
 * - input_channels is normally ->channels or ->output_channels when it's higher
 *
 * First, a meta (ex. TXTP) or plugin may add mixing commands through the API,
 * validated so non-sensical mixes are ignored (to ensure mixing code doesn't
 * have to recheck every time). Then, before starting to decode mixing must be
 * manually activated, because plugins need to be ready for possibly different
 * input/output channels. API could be improved but this way we can avoid having
 * to update all plugins, while allowing internal setup and layer/segment mixing
 * (may change in the future for simpler usage).
 *
 * Then after decoding normally, vgmstream applies mixing internally:
 * - detect if mixing is active and needs to be done at this point (some effects
 *   like fades only apply after certain time) and skip otherwise.
 * - copy outbuf to mixbuf, as using a float buffer to increase accuracy (most ops
 *   apply float volumes) and slightly improve performance (avoids doing
 *   int16-to-float casts per mix, as it's not free)
 * - apply all mixes on mixbuf
 * - copy mixbuf to outbuf
 * segmented/layered layouts handle mixing on their own.
 *
 * Mixing is tuned for most common case (no mix except fade-out at the end) and is
 * fast enough but not super-optimized yet, there is some penalty the more effects
 * are applied. Maybe could add extra sub-ops to avoid ifs and dumb values (volume=0.0
 * could simply use a clear op), only use mixbuf if necessary (swap can be done without
 * mixbuf if it goes first) or add function pointer indexes but isn't too important.
 * Operations are applied once per "step" with 1 sample from all channels to simplify code
 * (and maybe improve memory cache?), though maybe it should call one function per operation.
 */

#define VGMSTREAM_MAX_MIXING 128


/* mixing info */
typedef enum {
    MIX_SWAP,
    MIX_ADD,
    MIX_VOLUME,
    MIX_LIMIT,
    MIX_UPMIX,
    MIX_DOWNMIX,
    MIX_KILLMIX,
    MIX_FADE
} mix_command_t;

typedef struct {
    mix_command_t command;
    /* common */
    int ch_dst;
    int ch_src;
    float vol;

    /* fade envelope */
    float vol_start;    /* volume from pre to start */
    float vol_end;      /* volume from end to post */
    char shape;         /* curve type */
    int32_t time_pre;   /* position before time_start where vol_start applies (-1 = beginning) */
    int32_t time_start; /* fade start position where vol changes from vol_start to vol_end */
    int32_t time_end;   /* fade end position where vol changes from vol_start to vol_end */
    int32_t time_post;  /* position after time_end where vol_end applies (-1 = end) */
} mix_command_data;

typedef struct {
    int mixing_channels;    /* max channels needed to mix */
    int output_channels;    /* resulting channels after mixing */
    int mixing_on;          /* mixing allowed */
    int mixing_count;       /* mixing number */
    size_t mixing_size;     /* mixing max */
    mix_command_data mixing_chain[VGMSTREAM_MAX_MIXING]; /* effects to apply (could be alloc'ed but to simplify...) */
    float* mixbuf;          /* internal mixing buffer */
} mixing_data;


/* ******************************************************************* */

static int is_active(mixing_data *data, int32_t current_start, int32_t current_end) {
    int i;
    int32_t fade_start, fade_end;

    for (i = 0; i < data->mixing_count; i++) {
        mix_command_data mix = data->mixing_chain[i];

        if (mix.command != MIX_FADE)
            return 1; /* has non-fades = active */

        /* check is current range falls within a fade
         * (assuming fades were already optimized on add) */
        fade_start = mix.time_pre < 0 ? 0 : mix.time_pre;
        fade_end = mix.time_post < 0 ? INT_MAX : mix.time_post;

        if (current_start < fade_end && current_end > fade_start)
            return 1;
    }

    return 0;
}

static int32_t get_current_pos(VGMSTREAM* vgmstream) {
    int32_t current_pos;

    if (vgmstream->loop_flag && vgmstream->current_sample > vgmstream->loop_start_sample) {
        int loop_pre = vgmstream->loop_start_sample;
        int loop_into = vgmstream->current_sample - vgmstream->loop_start_sample;
        int loop_samples = vgmstream->loop_end_sample - vgmstream->loop_start_sample;
        current_pos = loop_pre + loop_into + loop_samples*vgmstream->loop_count;
    }
    else {
        current_pos = vgmstream->current_sample;
    }

    return current_pos;
}

static int get_fade_gain(mix_command_data *mix, float *out_cur_vol, int32_t current_subpos) {
    //todo optimizations: interleave calcs, maybe use cosf, powf, etc?
    float cur_vol = 0.0f;

    if ((current_subpos >= mix->time_pre || mix->time_pre < 0) && current_subpos < mix->time_start) {
        cur_vol = mix->vol_start; /* before */
    }
    else if (current_subpos >= mix->time_end && (current_subpos < mix->time_post || mix->time_post < 0)) {
        cur_vol = mix->vol_end; /* after */
    }
    else if (current_subpos >= mix->time_start && current_subpos < mix->time_end) {
        /* in between */
        float range_vol, range_dur, range_idx, index, gain;

        if (mix->vol_start < mix->vol_end) { /* fade in */
            range_vol = mix->vol_end - mix->vol_start;
            range_dur = mix->time_end - mix->time_start;
            range_idx = current_subpos - mix->time_start;
            index = range_idx / range_dur;
        } else { /* fade out */
            range_vol = mix->vol_end - mix->vol_start;
            range_dur = mix->time_end - mix->time_start;
            range_idx = mix->time_end - current_subpos;
            index = range_idx / range_dur;
        }

        /* Fading is done like this:
         * - find current position within fade duration
         * - get linear % (or rather, index from 0.0 .. 1.0) of duration
         * - apply shape to % (from linear fade to curved fade)
         * - get final volume for that point
         *
         * Roughly speaking some curve shapes are better for fades (decay rate is more natural
         * sounding in that highest to mid/low happens faster but low to lowest takes more time,
         * kinda like a gunshot or bell), and others for crossfades (decay of fade-in + fade-out
         * is adjusted so that added volume level stays constant-ish).
         *
         * As curves can fade in two ways ('normal' and curving 'the other way'), they are adjusted
         * to get 'normal' shape on both fades (by reversing index and making 1 - gain), thus some
         * curves are complementary (exponential fade-in ~= logarithmic fade-out); the following
         * are described taking fade-in = normal.
         */

        /* (curve math mostly from SoX/FFmpeg) */
        switch(mix->shape) {
            /* 2.5f in L/E 'pow' is the attenuation factor, where 5.0 (100db) is common but a bit fast
             * (alt calculations with 'exp' from FFmpeg use (factor)*ln(0.1) = -NN.N...  */

            case 'E': /* exponential (for fade-outs, closer to natural decay of sound) */
                //gain = pow(0.1f, (1.0f - index) * 2.5f);
                gain = exp(-5.75646273248511f * (1.0f - index));
                break;
            case 'L': /* logarithmic (inverse of the above, maybe for crossfades) */
                //gain = 1 - pow(0.1f, (index) * 2.5f);
                gain = 1 - exp(-5.75646273248511f * (index));
                break;

            case 'H': /* raised sine wave or cosine wave (for more musical crossfades) */
                gain = (1.0f - cos(index * M_PI )) / 2.0f;
                break;

            case 'Q': /* quarter of sine wave (for musical fades) */
                gain = sin(index * M_PI / 2.0f);
                break;

            case 'p': /* parabola (maybe for crossfades) */
                gain =  1.0f - sqrt(1.0f - index);
                break;
            case 'P': /* inverted parabola (maybe for fades) */
                gain = (1.0f - (1.0f - index) * (1.0f - index));
                break;

            case 'T': /* triangular/linear (simpler/sharper fades) */
            default:
                gain = index;
                break;
        }

        if (mix->vol_start < mix->vol_end) {  /* fade in */
            cur_vol = mix->vol_start + range_vol * gain;
        } else { /* fade out */
            cur_vol = mix->vol_end - range_vol * gain; //mix->vol_start - range_vol * (1 - gain);
        }
    }
    else {
        goto fail;
    }

    *out_cur_vol = cur_vol;
    return 1;
fail:
    return 0;
}

void mix_vgmstream(sample_t *outbuf, int32_t sample_count, VGMSTREAM* vgmstream) {
    mixing_data *data = vgmstream->mixing_data;
    int ch, s, m, ok;

    int32_t current_pos, current_subpos;
    float temp_f, temp_min, temp_max, cur_vol;
    float *temp_mixbuf;
    sample_t *temp_outbuf;

    const float limiter_max = 32767.0f;
    const float limiter_min = -32768.0f;


    /* no support or not need to apply */
    if (!data || !data->mixing_on || data->mixing_count == 0)
        return;

    /* try to skip if no ops apply (for example if fade set but does nothing yet) */
    current_pos = get_current_pos(vgmstream);
    if (!is_active(data, current_pos, current_pos + sample_count))
        return;


    /* use advancing buffer pointers to simplify logic */
    temp_mixbuf = data->mixbuf;
    temp_outbuf = outbuf;

    /* apply mixes in order per channel */
    for (s = 0; s < sample_count; s++) {
        /* reset after new sample 'step'*/
        float *stpbuf = temp_mixbuf;
        int step_channels = vgmstream->channels;

        for (ch = 0; ch < step_channels; ch++) {
            stpbuf[ch] = temp_outbuf[ch]; /* copy current 'lane' */
        }

        for (m = 0; m < data->mixing_count; m++) {
            mix_command_data mix = data->mixing_chain[m];

            /* mixing ops are designed to apply in order, all channels per 1 sample 'step'. Since some ops change
             * total channels, channel number meaning varies as ops move them around, ex:
             * - 4ch w/ "1-2,2+3" = ch1<>ch3, ch2(old ch1)+ch3 = 4ch: ch2 ch1+ch3 ch3 ch4
             * - 4ch w/ "2+3,1-2" = ch2+ch3, ch1<>ch2(modified) = 4ch: ch2+ch3 ch1 ch3 ch4
             * - 2ch w/ "1+2,1u" = ch1+ch2, ch1(add and push rest) = 3ch: ch1' ch1+ch2 ch2
             * - 2ch w/ "1u,1+2" = ch1(add and push rest) = 3ch: ch1'+ch1 ch1 ch2
             * - 2ch w/ "1-2,1d" = ch1<>ch2, ch1(drop and move ch2(old ch1) to ch1) = ch1
             * - 2ch w/ "1d,1-2" = ch1(drop and pull rest), ch1(do nothing, ch2 doesn't exist now) = ch2
             */
            switch(mix.command) {

                case MIX_SWAP:
                    temp_f = stpbuf[mix.ch_dst];
                    stpbuf[mix.ch_dst] = stpbuf[mix.ch_src];
                    stpbuf[mix.ch_src] = temp_f;
                    break;

                case MIX_ADD:
                    stpbuf[mix.ch_dst] = stpbuf[mix.ch_dst] + stpbuf[mix.ch_src] * mix.vol;
                    break;

                case MIX_VOLUME:
                    if (mix.ch_dst < 0) {
                        for (ch = 0; ch < step_channels; ch++) {
                            stpbuf[ch] = stpbuf[ch] * mix.vol;
                        }
                    }
                    else {
                        stpbuf[mix.ch_dst] = stpbuf[mix.ch_dst] * mix.vol;
                    }
                    break;

                case MIX_LIMIT:
                    temp_max = limiter_max * mix.vol;
                    temp_min = limiter_min * mix.vol;

                    if (mix.ch_dst < 0) {
                        for (ch = 0; ch < step_channels; ch++) {
                            if (stpbuf[ch] > temp_max)
                                stpbuf[ch] = temp_max;
                            else if (stpbuf[ch] < temp_min)
                                stpbuf[ch] = temp_min;
                        }
                    }
                    else {
                        if (stpbuf[mix.ch_dst] > temp_max)
                            stpbuf[mix.ch_dst] = temp_max;
                        else if (stpbuf[mix.ch_dst] < temp_min)
                            stpbuf[mix.ch_dst] = temp_min;
                    }
                    break;

                case MIX_UPMIX:
                    step_channels += 1;
                    for (ch = step_channels - 1; ch > mix.ch_dst; ch--) {
                        stpbuf[ch] = stpbuf[ch-1]; /* 'push' channels forward (or pull backwards) */
                    }
                    stpbuf[mix.ch_dst] = 0; /* inserted as silent */
                    break;

                case MIX_DOWNMIX:
                    step_channels -= 1;
                    for (ch = mix.ch_dst; ch < step_channels; ch++) {
                        stpbuf[ch] = stpbuf[ch+1]; /* 'pull' channels back */
                    }
                    break;

                case MIX_KILLMIX:
                    step_channels = mix.ch_dst; /* clamp channels */
                    break;

                case MIX_FADE:
                    current_subpos = current_pos + s;

                    ok = get_fade_gain(&mix, &cur_vol, current_subpos);
                    if (!ok) {
                        break;
                    }

                    if (mix.ch_dst < 0) {
                        for (ch = 0; ch < step_channels; ch++) {
                            stpbuf[ch] = stpbuf[ch] * cur_vol;
                        }
                    }
                    else {
                        stpbuf[mix.ch_dst] = stpbuf[mix.ch_dst] * cur_vol;
                    }
                    break;

                default:
                    break;
            }
        }

        temp_mixbuf += step_channels;
        temp_outbuf += vgmstream->channels;
    }

    /* copy resulting mix to output */
    for (s = 0; s < sample_count * data->output_channels; s++) {
        /* when casting float to int, value is simply truncated:
         * - (int)1.7 = 1, (int)-1.7 = -1
         * alts for more accurate rounding could be:
         * - (int)floor(f)
         * - (int)(f < 0 ? f - 0.5f : f + 0.5f)
         * - (((int) (f1 + 32768.5)) - 32768)
         * - etc
         * but since +-1 isn't really audible we'll just cast as it's the fastest
         */
        outbuf[s] = clamp16( (int32_t)data->mixbuf[s] );
    }
}

/* ******************************************************************* */

void mixing_init(VGMSTREAM* vgmstream) {
    mixing_data *data = calloc(1, sizeof(mixing_data));
    if (!data) goto fail;

    data->mixing_size = VGMSTREAM_MAX_MIXING; /* fixed array for now */
    data->mixing_channels = vgmstream->channels;
    data->output_channels = vgmstream->channels;

    vgmstream->mixing_data = data;
    return;

fail:
    free(data);
    return;
}

void mixing_close(VGMSTREAM* vgmstream) {
    mixing_data *data = NULL;
    if (!vgmstream) return;

    data = vgmstream->mixing_data;
    if (!data) return;

    free(data->mixbuf);
    free(data);
}

void mixing_update_channel(VGMSTREAM* vgmstream) {
    mixing_data *data = vgmstream->mixing_data;
    if (!data) return;

    /* lame hack for dual stereo, but dual stereo is pretty hack-ish to begin with */
    data->mixing_channels++;
    data->output_channels++;
}

/* ******************************************************************* */

static int add_mixing(VGMSTREAM* vgmstream, mix_command_data *mix) {
    mixing_data *data = vgmstream->mixing_data;
    if (!data) return 0;


    if (data->mixing_on) {
        VGM_LOG("MIX: ignoring new mixes when mixing active\n");
        return 0; /* to avoid down/upmixing after activation */
    }

    if (data->mixing_count + 1 > data->mixing_size) {
        VGM_LOG("MIX: too many mixes\n");
        return 0;
    }

    data->mixing_chain[data->mixing_count] = *mix; /* memcpy */
    data->mixing_count++;

    //;VGM_LOG("MIX: total %i\n", data->mixing_count);
    return 1;
}


void mixing_push_swap(VGMSTREAM* vgmstream, int ch_dst, int ch_src) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};

    if (ch_dst < 0 || ch_src < 0 || ch_dst == ch_src) return;
    if (!data || ch_dst >= data->output_channels || ch_src >= data->output_channels) return;
    mix.command = MIX_SWAP;
    mix.ch_dst = ch_dst;
    mix.ch_src = ch_src;

    add_mixing(vgmstream, &mix);
}

void mixing_push_add(VGMSTREAM* vgmstream, int ch_dst, int ch_src, double volume) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};
    if (!data) return;

    //if (volume < 0.0) return; /* negative volume inverts the waveform */
    if (volume == 0.0) return; /* ch_src becomes silent and nothing is added */
    if (ch_dst < 0 || ch_src < 0) return;
    if (!data || ch_dst >= data->output_channels || ch_src >= data->output_channels) return;

    mix.command = MIX_ADD; //if (volume == 1.0) MIX_ADD_COPY /* could simplify */
    mix.ch_dst = ch_dst;
    mix.ch_src = ch_src;
    mix.vol = volume;

    //;VGM_LOG("MIX: add %i+%i*%f\n", ch_dst,ch_src,volume);
    add_mixing(vgmstream, &mix);
}

void mixing_push_volume(VGMSTREAM* vgmstream, int ch_dst, double volume) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};

    //if (ch_dst < 0) return; /* means all channels */
    //if (volume < 0.0) return; /* negative volume inverts the waveform */
    if (volume == 1.0) return; /* no change */
    if (!data || ch_dst >= data->output_channels) return;

    mix.command = MIX_VOLUME; //if (volume == 0.0) MIX_VOLUME0 /* could simplify */
    mix.ch_dst = ch_dst;
    mix.vol = volume;

    //;VGM_LOG("MIX: volume %i*%f\n", ch_dst,volume);
    add_mixing(vgmstream, &mix);
}

void mixing_push_limit(VGMSTREAM* vgmstream, int ch_dst, double volume) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};

    //if (ch_dst < 0) return; /* means all channels */
    if (volume < 0.0) return;
    if (volume == 1.0) return; /* no actual difference */
    if (!data || ch_dst >= data->output_channels) return;
    //if (volume == 0.0) return; /* dumb but whatevs */

    mix.command = MIX_LIMIT;
    mix.ch_dst = ch_dst;
    mix.vol = volume;

    add_mixing(vgmstream, &mix);
}

void mixing_push_upmix(VGMSTREAM* vgmstream, int ch_dst) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};
    int ok;

    if (ch_dst < 0) return;
    if (!data || ch_dst > data->output_channels || data->output_channels +1 > VGMSTREAM_MAX_CHANNELS) return;
    /* dst can be == output_channels here, since we are inserting */

    mix.command = MIX_UPMIX;
    mix.ch_dst = ch_dst;

    ok = add_mixing(vgmstream, &mix);
    if (ok) {
        data->output_channels += 1;
        if (data->mixing_channels < data->output_channels)
            data->mixing_channels = data->output_channels;
    }
}

void mixing_push_downmix(VGMSTREAM* vgmstream, int ch_dst) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};
    int ok;

    if (ch_dst < 0) return;
    if (!data || ch_dst >= data->output_channels || data->output_channels - 1 < 1) return;

    mix.command = MIX_DOWNMIX;
    mix.ch_dst = ch_dst;

    ok = add_mixing(vgmstream, &mix);
    if (ok) {
        data->output_channels -= 1;
    }
}

void mixing_push_killmix(VGMSTREAM* vgmstream, int ch_dst) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};
    int ok;

    if (ch_dst <= 0) return; /* can't kill from first channel */
    if (!data || ch_dst >= data->output_channels) return;

    mix.command = MIX_KILLMIX;
    mix.ch_dst = ch_dst;

    //;VGM_LOG("MIX: killmix %i\n", ch_dst);
    ok = add_mixing(vgmstream, &mix);
    if (ok) {
        data->output_channels = ch_dst; /* clamp channels */
    }
}


static mix_command_data* get_last_fade(mixing_data *data, int target_channel) {
    int i;
    for (i = data->mixing_count; i > 0; i--) {
        mix_command_data *mix = &data->mixing_chain[i-1];
        if (mix->command != MIX_FADE)
            continue;
        if (mix->ch_dst == target_channel)
            return mix;
    }

    return NULL;
}


void mixing_push_fade(VGMSTREAM* vgmstream, int ch_dst, double vol_start, double vol_end, char shape,
        int32_t time_pre, int32_t time_start, int32_t time_end, int32_t time_post) {
    mixing_data *data = vgmstream->mixing_data;
    mix_command_data mix = {0};
    mix_command_data *mix_prev;


    //if (ch_dst < 0) return; /* means all channels */
    if (!data || ch_dst >= data->output_channels) return;
    if (time_pre > time_start || time_start > time_end || (time_post >= 0 && time_end > time_post)) return;
    if (time_start < 0 || time_end < 0) return;
    //if (time_pre < 0 || time_post < 0) return; /* special meaning of file start/end */
    //if (vol_start == vol_end) /* weird but let in case of being used to cancel others fades... maybe? */

    if (shape == '{' || shape == '}')
        shape = 'E';
    if (shape == '(' || shape == ')')
        shape = 'H';

    mix.command = MIX_FADE;
    mix.ch_dst = ch_dst;
    mix.vol_start = vol_start;
    mix.vol_end = vol_end;
    mix.shape = shape;
    mix.time_pre = time_pre;
    mix.time_start = time_start;
    mix.time_end = time_end;
    mix.time_post = time_post;


    /* cancel fades and optimize a bit when using negative pre/post:
     * - fades work like this:
     *   <----------|----------|---------->
     *   pre1       start1  end1      post1
     * - when pre and post are set nothing is done (fade is exact and multiple fades may overlap)
     * - when previous fade's post or current fade's pre are negative (meaning file end/start)
     *   they should cancel each other (to allow chaning fade-in + fade-out + fade-in + etc):
     *   <----------|----------|----------| |----------|----------|---------->
     *   pre1       start1  end1      post1 pre2       start2  end2      post2
     * - other cases (previous fade is actually after/in-between current fade) are ignored
     *   as they're uncommon and hard to optimize
     * fades cancel fades of the same channel, and 'all channel' (-1) fades also cancel 'all channels'
     */
    mix_prev = get_last_fade(data, mix.ch_dst);
    if (mix_prev == NULL) {
        if (vol_start == 1.0 && time_pre < 0)
            time_pre = time_start; /* fade-out helds default volume before fade start can be clamped */
        if (vol_end == 1.0 && time_post < 0)
            time_post = time_end; /* fade-in helds default volume after fade end can be clamped */
    }
    else if (mix_prev->time_post < 0 || mix.time_pre < 0) {
        int is_prev = 1;
        if ((mix_prev->time_end > mix.time_start) ||
            (mix_prev->time_post >= 0 && mix_prev->time_post > mix.time_start) ||
            (mix.time_pre >= 0 && mix.time_pre < mix_prev->time_end))
            is_prev = 0;

        if (is_prev) {
            /* change negative values to actual points */
            if (mix_prev->time_post < 0 && mix_prev->time_post < 0) {
                mix_prev->time_post = mix_prev->time_end;
                mix.time_pre = mix_prev->time_post;
            }
            if (mix_prev->time_post >= 0 && mix.time_pre < 0) {

                mix.time_pre = mix_prev->time_post;
            }
            else if (mix_prev->time_post < 0 && mix.time_pre >= 0) {
                mix_prev->time_post = mix.time_pre;
            }
            /* else: both define start/ends, do nothing */
        }
        /* should only modify prev if add_mixing but meh */
    }

    //;VGM_LOG("MIX: fade %i^%f~%f=%c@%i~%i~%i~%i\n", ch_dst, vol_start, vol_end, shape, time_pre, time_start, time_end, time_post);
    add_mixing(vgmstream, &mix);
}

/* ******************************************************************* */

void mixing_macro_volume(VGMSTREAM* vgmstream, double volume, uint32_t mask) {
    mixing_data *data = vgmstream->mixing_data;
    int ch;

    if (!data)
        return;

    if (mask == 0) {
        mixing_push_volume(vgmstream, -1, volume);
        return;
    }

    for (ch = 0; ch < data->output_channels; ch++) {
        if (!((mask >> ch) & 1))
            continue;
        mixing_push_volume(vgmstream, ch, volume);
    }
}

void mixing_macro_track(VGMSTREAM* vgmstream, uint32_t mask) {
    mixing_data *data = vgmstream->mixing_data;
    int ch;

    if (!data)
        return;

    if (mask == 0) {
        return;
    }

    /* reverse remove all channels (easier this way as when removing channels numbers change) */
    for (ch = data->output_channels - 1; ch >= 0; ch--) {
        if ((mask >> ch) & 1)
            continue;
        mixing_push_downmix(vgmstream, ch);
    }
}

void mixing_macro_layer(VGMSTREAM* vgmstream, int max, uint32_t mask, char mode) {
    mixing_data *data = vgmstream->mixing_data;
    int current, ch, output_channels, selected_channels;

    if (!data)
        return;
    if (max <= 0 || data->output_channels <= max)
        return;

    /* set all channels (non-existant channels will be ignored) */
    if (mask == 0) {
        mask = ~mask;
    }

    /* save before adding fake channels */
    output_channels = data->output_channels;

    /* count possibly set channels */
    selected_channels = 0;
    for (ch = 0; ch < output_channels; ch++) {
        selected_channels += (mask >> ch) & 1;
    }

    /* make N fake channels at the beginning for easier calcs */
    for (ch = 0; ch < max; ch++) {
        mixing_push_upmix(vgmstream, 0);
    }

    /* add all layers in this order: ch0: 0, 0+N, 0+N*2 ... / ch1: 1, 1+N ... */
    current = 0;
    for (ch = 0; ch < output_channels; ch++) {
        double volume = 1.0;

        if (!((mask >> ch) & 1))
            continue;

        /* mode 'v': same volume for all layers (for layered vocals) */
        /* mode 'b': volume adjusted depending on layers (for layered bgm) */
        /* mode 'e': volume adjusted equally for all layers (for generic downmixing) */
        if (mode == 'b' && ch < max) {
            /* reduce a bit main channels (see below) */
            int channel_mixes = selected_channels / max;
            if (current < selected_channels % (channel_mixes * max)) /* may be simplified? */
                channel_mixes += 1;
            channel_mixes -= 1; /* better formula? */
            if (channel_mixes <= 0) /* ??? */
                channel_mixes = 1;

            volume = 1 / sqrt(channel_mixes);
        }
        if ((mode == 'b' && ch >= max) || (mode == 'e')) {
            /* find how many will be mixed in current channel (earlier channels receive more
             * mixes than later ones, ex: selected 8ch + max 3ch: ch0=0+3+6, ch1=1+4+7, ch2=2+5) */
            int channel_mixes = selected_channels / max;
            if (channel_mixes <= 0) /* ??? */
                channel_mixes = 1;
            if (current < selected_channels % (channel_mixes * max)) /* may be simplified? */
                channel_mixes += 1;

            volume = 1 / sqrt(channel_mixes); /* "power" add */
        }
        //;VGM_LOG("MIX: layer ch=%i, cur=%i, v=%f\n", ch, current, volume);

        mixing_push_add(vgmstream, current, max + ch, volume); /* ch adjusted considering upmixed channels */
        current++;
        if (current >= max)
            current = 0;
    }

    /* remove all mixed channels */
    mixing_push_killmix(vgmstream, max);
}

void mixing_macro_crosstrack(VGMSTREAM* vgmstream, int max) {
    mixing_data *data = vgmstream->mixing_data;
    int current, ch, track, track_ch, track_num, output_channels;
    int32_t change_pos, change_next, change_time;

    if (!data)
        return;
    if (max <= 0 || data->output_channels <= max)
        return;
    if (!vgmstream->loop_flag) /* maybe force loop? */
        return;

    /* this probably only makes sense for even channels so upmix before if needed) */
    output_channels = data->output_channels;
    if (output_channels % 2) {
        mixing_push_upmix(vgmstream, output_channels);
        output_channels += 1;
    }

    /* set loops to hear all track changes */
    track_num = output_channels / max;
    if (vgmstream->config_loop_count < track_num)
        vgmstream->config_loop_count = track_num;

    ch = 0;
    for (track = 0; track < track_num; track++) {
        double volume = 1.0; /* won't play at the same time, no volume change needed */

        int loop_pre = vgmstream->loop_start_sample;
        int loop_samples = vgmstream->loop_end_sample - vgmstream->loop_start_sample;
        change_pos = loop_pre + loop_samples * track;
        change_next = loop_pre + loop_samples * (track + 1);
        change_time = 15.0 * vgmstream->sample_rate; /* in secs */

        for (track_ch = 0; track_ch < max; track_ch++) {
            if (track > 0) { /* fade-in when prev track fades-out */
                mixing_push_fade(vgmstream, ch + track_ch, 0.0, volume, '(', -1, change_pos, change_pos + change_time, -1);
            }

            if (track + 1 < track_num) { /* fade-out when next track fades-in */
                mixing_push_fade(vgmstream, ch + track_ch, volume, 0.0, ')', -1, change_next, change_next + change_time, -1);
            }
        }

        ch += max;
    }

    /* mix all tracks into first */
    current = 0;
    for (ch = max; ch < output_channels; ch++) {
        mixing_push_add(vgmstream, current, ch, 1.0); /* won't play at the same time, no volume change needed */

        current++;
        if (current >= max)
            current = 0;
    }

    /* remove unneeded channels */
    mixing_push_killmix(vgmstream, max);
}

void mixing_macro_crosslayer(VGMSTREAM* vgmstream, int max, char mode) {
    mixing_data *data = vgmstream->mixing_data;
    int current, ch, layer, layer_ch, layer_num, loop, output_channels;
    int32_t change_pos, change_time;

    if (!data)
        return;
    if (max <= 0 || data->output_channels <= max)
        return;
    if (!vgmstream->loop_flag) /* maybe force loop? */
        return;

    /* this probably only makes sense for even channels so upmix before if needed) */
    output_channels = data->output_channels;
    if (output_channels % 2) {
        mixing_push_upmix(vgmstream, output_channels);
        output_channels += 1;
    }

    /* set loops to hear all track changes */
    layer_num = output_channels / max;
    if (vgmstream->config_loop_count < layer_num)
        vgmstream->config_loop_count = layer_num;

    /* mode 'v': constant volume
     * mode 'e': sets fades to successively lower/equalize volume per loop for each layer
     * (to keep final volume constant-ish), ex. 3 layers/loops, 2 max:
     * - layer0 (ch0+1): loop0 --[1.0]--, loop1 )=1.0..0.7, loop2 )=0.7..0.5, loop3 --[0.5/end]--
     * - layer1 (ch2+3): loop0 --[0.0]--, loop1 (=0.0..0.7, loop2 )=0.7..0.5, loop3 --[0.5/end]--
     * - layer2 (ch4+5): loop0 --[0.0]--, loop1 ---[0.0]--, loop2 (=0.0..0.5, loop3 --[0.5/end]--
     * mode 'b': similar but 1st layer (main) has higher/delayed volume:
     * - layer0 (ch0+1): loop0 --[1.0]--, loop1 )=1.0..1.0, loop2 )=1.0..0.7, loop3 --[0.7/end]--
     */
    for (loop = 1; loop < layer_num; loop++) {
        double volume1 = 1.0;
        double volume2 = 1.0;

        int loop_pre = vgmstream->loop_start_sample;
        int loop_samples = vgmstream->loop_end_sample - vgmstream->loop_start_sample;
        change_pos = loop_pre + loop_samples * loop;
        change_time = 10.0 * vgmstream->sample_rate; /* in secs */

        if (mode == 'e') {
            volume1 = 1 / sqrt(loop + 0);
            volume2 = 1 / sqrt(loop + 1);
        }

        ch = 0;
        for (layer = 0; layer < layer_num; layer++) {
            char type;

            if (mode == 'b') {
                if (layer == 0) {
                    volume1 = 1 / sqrt(loop - 1 <= 0 ? 1 : loop - 1);
                    volume2 = 1 / sqrt(loop + 0);
                }
                else {
                    volume1 = 1 / sqrt(loop + 0);
                    volume2 = 1 / sqrt(loop + 1);
                }
            }

            if (layer > loop) { /* not playing yet (volume is implicitly 0.0 from first fade in) */
                continue;
            } else if (layer == loop) { /* fades in for the first time */
                volume1 = 0.0;
                type = '(';
            } else { /* otherwise fades out to match other layers's volume */
                type = ')';
            }

            //;VGM_LOG("MIX: loop=%i, layer %i, vol1=%f, vol2=%f\n", loop, layer, volume1, volume2);

            for (layer_ch = 0; layer_ch < max; layer_ch++) {
                mixing_push_fade(vgmstream, ch + layer_ch, volume1, volume2, type, -1, change_pos, change_pos + change_time, -1);
            }

            ch += max;
        }
    }

    /* mix all tracks into first */
    current = 0;
    for (ch = max; ch < output_channels; ch++) {
        mixing_push_add(vgmstream, current, ch, 1.0);

        current++;
        if (current >= max)
            current = 0;
    }

    /* remove unneeded channels */
    mixing_push_killmix(vgmstream, max);
}

/* ******************************************************************* */

void mixing_setup(VGMSTREAM * vgmstream, int32_t max_sample_count) {
    mixing_data *data = vgmstream->mixing_data;
    float *mixbuf_re = NULL;

    if (!data) goto fail;

    /* a bit wonky but eh... */
    if (vgmstream->channel_layout && vgmstream->channels != data->output_channels) {
        vgmstream->channel_layout = 0;
        ((VGMSTREAM*)vgmstream->start_vgmstream)->channel_layout = 0;
    }

    /* special value to not actually enable anything (used to query values) */
    if (max_sample_count <= 0)
        goto fail;

    /* create or alter internal buffer */
    mixbuf_re = realloc(data->mixbuf, max_sample_count*data->mixing_channels*sizeof(float));
    if (!mixbuf_re) goto fail;

    data->mixbuf = mixbuf_re;
    data->mixing_on = 1;

    /* since data exists on its own memory and pointer is already set
     * there is no need to propagate to start_vgmstream */

    /* segments/layers are independant from external buffers and may always mix */

    return;
fail:
    return;
}

void mixing_info(VGMSTREAM * vgmstream, int *out_input_channels, int *out_output_channels) {
    mixing_data *data = vgmstream->mixing_data;
    int input_channels, output_channels;

    if (!data) goto fail;

    output_channels = data->output_channels;
    if (data->output_channels > vgmstream->channels)
        input_channels = data->output_channels;
    else
        input_channels = vgmstream->channels;

    if (out_input_channels)  *out_input_channels = input_channels;
    if (out_output_channels) *out_output_channels = output_channels;

    //;VGM_LOG("MIX: channels %i, in=%i, out=%i, mix=%i\n", vgmstream->channels, input_channels, output_channels, data->mixing_channels);
    return;
fail:
    return;
}
