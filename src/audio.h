#ifndef ESP_AUDIO_H
#define ESP_AUDIO_H

#include "esp8266.h"

typedef struct {
    int    rate;
    double pos;             /* the tick the next sample starts at */
    float  level;           /* the speaker pin's level at 'pos' */
    float  dc;
    float  volume;
    bool   speaker;         /* the speaker's sound (speaker_filter.c), or the bare pin */
    float  fir[256];        /* the last samples, for the speaker filter */
    int    fir_pos;
} EspAudio;

/* the speaker's response at 48 kHz (speaker_filter.c) */
extern const int   esp_speaker_taps;
extern const float esp_speaker_fir[];

void esp_audio_init(EspAudio *a, int rate, uint64_t now);
/* samples for the time run since the last call, at most max */
int  esp_audio_render(EspAudio *a, Esp *s, float *out, int max);
/* drops what was logged: the time since goes unheard */
void esp_audio_skip(EspAudio *a, Esp *s);

#endif
