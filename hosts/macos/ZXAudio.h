/* ZXAudio.h -- Spectrum speaker output through Core Audio */

#ifndef ZXAUDIO_H
#define ZXAUDIO_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct zxaudio zxaudio_t;

/** Create the output and start playing. \return NULL on failure. */
zxaudio_t *zxaudio_create(void);

void zxaudio_destroy(zxaudio_t *audio);

/** Turn output on or off. */
void zxaudio_toggle(zxaudio_t *audio);

/** Game speed in percent. Scales how many speaker bits make one sample. */
void zxaudio_set_speed(zxaudio_t *audio, int percent);

/** Queue one speaker bit. Called from the game thread. */
void zxaudio_speaker(zxaudio_t *audio, int on);

#ifdef __cplusplus
}
#endif

#endif /* ZXAUDIO_H */
