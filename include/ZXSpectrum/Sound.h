/* Sound.h
 *
 * Renders the virtual Spectrum's speaker and AY output to PCM.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 */

#ifndef ZXSPECTRUM_SOUND_H
#define ZXSPECTRUM_SOUND_H

#include <stdint.h>

#include "ZXSpectrum/Spectrum.h"

/**
 * A sound renderer. The game side queues speaker level changes and AY
 * register writes, each timestamped with the virtual T-state clock. The output
 * side renders signed 16-bit PCM, replaying each event at the sample its
 * timestamp falls in.
 *
 * Events are timed in T-states, not wall-clock time, and kept in 32 bits
 * (modular), as Norcroft C has no 64-bit integer type. The replay cursor
 * re-anchors to the next event, less a cushion, whenever the queue runs dry,
 * so the game and output clocks never need relating directly.
 *
 * Not thread safe: a host with separate game and audio threads must serialise
 * every call with one lock.
 */
typedef struct zxsound zxsound_t;

/**
 * Create a sound renderer.
 *
 * \param[in] sample_rate     Output sample rate, Hz.
 * \param[in] tstates_per_sec Z80 clock (Z80_CLOCK_48K or Z80_CLOCK_128K).
 * \param[in] cushion_ms      Output latency added at each re-anchor, to absorb
 *                            jitter in the game thread.
 * \param[in] channels        Output channels per frame: 1 or 2. Output is
 *                            mono; with 2 it is duplicated into both.
 *
 * \return New renderer, or NULL if out of memory.
 */
zxsound_t *zxsound_create(int      sample_rate,
                          uint32_t tstates_per_sec,
                          int      cushion_ms,
                          int      channels);

void zxsound_destroy(zxsound_t *sound);

/** Set the game speed, percent. Higher speeds replay T-states faster. */
void zxsound_set_speed(zxsound_t *sound, int percent);

/** Silence the speaker (beeper) contribution. */
void zxsound_mute_speaker(zxsound_t *sound, int mute);

/** Enable or disable AY channel 0..2 (A..C). */
void zxsound_enable_ay_channel(zxsound_t *sound, int channel, int enable);

/** Queue a speaker level change. Fits zxconfig_t's speaker callback. */
void zxsound_speaker(zxsound_t *sound, int on_off, zxclock_t tstates);

/** Queue an AY register select or data write. Fits zxconfig_t's ay_out. */
void zxsound_ay_out(zxsound_t *sound,
                    uint16_t   port,
                    uint8_t    byte,
                    zxclock_t  tstates);

/** Render 'frames' frames of signed 16-bit PCM into 'buf'. */
void zxsound_render(zxsound_t *sound, int16_t *buf, int frames);

#endif /* ZXSPECTRUM_SOUND_H */
