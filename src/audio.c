/*
 * The speaker, turned into samples.
 *
 * periph.c logs every change of the speaker pin (GPIO0) with the tick it
 * happened on: a level of 0 or 1, or a fraction when the pin carries the
 * sigma-delta modulator. Each output sample is the average level over
 * exactly the ticks it covers, so a tone() square wave, the core's waveform
 * PWM (analogWrite) used as a DAC and anything bit-banged all come out
 * right. A DC blocker then does what the speaker's coupling does, and,
 * when switched on (F7, --speaker), a filter with a small speaker's response
 * (speaker_filter.c) what the speaker makes of the pin. It is off by default:
 * the bare pin signal.
 */
#include <string.h>
#include "audio.h"

void esp_audio_init(EspAudio *a, int rate, uint64_t now)
{
    const float vol = a->volume > 0 ? a->volume : 1.0f;
    const bool speaker = a->speaker;
    memset(a, 0, sizeof(*a));
    a->rate = rate;
    a->pos = (double)now;
    a->volume = vol;
    a->speaker = speaker;
}

int esp_audio_render(EspAudio *a, Esp *s, float *out, int max)
{
    const double tps = (double)ESP_TICK_HZ / a->rate;
    const double end = (double)esp_now(s);
    int n = 0;
    while (n < max && a->pos + tps <= end) {
        const double t0 = a->pos, t1 = t0 + tps;
        double high = 0.0, t = t0;
        while (s->spk_tail != s->spk_head && (double)s->spk_log[s->spk_tail].tick < t1) {
            const SpkEvent *e = &s->spk_log[s->spk_tail];
            const double at = (double)e->tick > t ? (double)e->tick : t;
            high += a->level * (at - t);
            t = at;
            a->level = e->level;
            s->spk_tail = (s->spk_tail + 1) % ESP_SPK_LOG;
        }
        high += a->level * (t1 - t);
        const float level = (float)(high / tps);
        /* AC coupling: a first order high pass at about 20 Hz */
        a->dc += (level - a->dc) * (float)(2.0 * 3.14159265 * 20.0 / a->rate);
        float v = level - a->dc;
        /* what the speaker makes of it (the filter is for 48 kHz) */
        a->fir[a->fir_pos] = v;
        if (a->speaker && a->rate == 48000) {
            float acc = 0;
            int k = a->fir_pos;
            for (int i = 0; i < esp_speaker_taps; i++) {
                acc += esp_speaker_fir[i] * a->fir[k];
                k = k ? k - 1 : esp_speaker_taps - 1;
            }
            v = acc;
        }
        a->fir_pos = a->fir_pos + 1 < esp_speaker_taps ? a->fir_pos + 1 : 0;
        v *= a->volume;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[n++] = v;
        a->pos = t1;
    }
    return n;
}

void esp_audio_skip(EspAudio *a, Esp *s)
{
    while (s->spk_tail != s->spk_head) {
        a->level = s->spk_log[s->spk_tail].level;
        s->spk_tail = (s->spk_tail + 1) % ESP_SPK_LOG;
    }
    a->pos = (double)esp_now(s);
}
