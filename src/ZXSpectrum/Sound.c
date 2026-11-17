/* Sound.c
 *
 * Renders the virtual Spectrum's speaker and AY output to PCM.
 *
 * Moved from the SDL3 front-end so that every front-end can share it.
 *
 * Copyright (c) David Thomas, 2013-2026. <dave@davespace.co.uk>
 */

#include <stdlib.h>

#include "ZXSpectrum/Macros.h"
#include "ZXSpectrum/slopay-chip.h"

#include "ZXSpectrum/Sound.h"

/* ----------------------------------------------------------------------- */

#define AY_CLOCK_FREQ       (1773400) /* ZX Spectrum 128K AY-3-8912 clock rate */
#define AY_VOLUME_PCT       (20)      /* 0..AY_MASTER_VOLUME_MAX */

#define BEEPER_VOLUME_PCT   (20)      /* 48K beeper level, percent of full scale */
#define BEEPER_AMPLITUDE    (32767 * BEEPER_VOLUME_PCT / 100)

/* The beeper is a hard on/off level, so summing it straight into the AY signal
 * biases the waveform upward whenever it is high. AY gain headroom (9/10)
 * keeps that sum inside int16 range, and a one-pole DC-block highpass removes
 * the bias first. The filter's pole, R = 0.995, is held as 1 - R in 1/32768
 * units, and its state in 1/256 units, so it needs no floating point. */
#define AY_GAIN_NUM         (9)
#define AY_GAIN_DEN         (10)
#define DC_BLOCK_ONE_MINUS_R (164)    /* (1 - 0.995) * 32768 */
#define DC_BLOCK_STATE_ONE  (256)

/* Sampled speech drives the AY DAC with three register writes per nibble
 * (channels A, B and C). The longest Chase H.Q. sample needs about 32,000
 * slots; this leaves headroom above that. */
#define QUEUE_CAPACITY      (65536)

/* ----------------------------------------------------------------------- */

enum
{
  EVENT_AY,     /* AY register write (128K music and speech) */
  EVENT_SPEAKER /* speaker level change (48K bit-banged effects) */
};

typedef struct
{
  uint32_t time;  /* T-state timestamp, modular */
  uint8_t  type;  /* EVENT_* */
  uint8_t  reg;   /* AY events only */
  uint8_t  value; /* AY: register value; speaker: level 0/1 */
}
event_t;

struct zxsound
{
  int               sample_rate;
  int               channels;
  uint32_t          tstates_per_sec;
  int               cushion_ms;

  slopay_chip_t    *ay;
  slopay_chip_reg_t ay_latched_reg; /* selected by the last port_AY_REGISTER write */

  event_t          *queue;
  int               queue_head;
  int               queue_tail;

  int               speaker_last_level; /* last queued level */
  int               speaker_level;      /* current replay level */
  int               speaker_muted;
  int32_t           dc_prev_in;         /* DC-block state, DC_BLOCK_STATE_ONE units */
  int32_t           dc_prev_out;

  /* Replay cursor: the T-state the next output sample starts at. It advances
   * by step_int + step_rem / sample_rate T-states per sample, the remainder
   * carried in step_frac, so it never drifts from the sample count. */
  uint32_t          step_int;
  uint32_t          step_rem;
  uint32_t          step_frac;
  uint32_t          cushion;  /* T-states */
  uint32_t          cursor;
  int               anchored; /* cleared when the queue runs dry */
};

/* ----------------------------------------------------------------------- */

zxsound_t *zxsound_create(int      sample_rate,
                          uint32_t tstates_per_sec,
                          int      cushion_ms,
                          int      channels)
{
  zxsound_t *sound;

  sound = calloc(1, sizeof(*sound));
  if (sound == NULL)
    return NULL;

  sound->sample_rate     = sample_rate;
  sound->channels        = (channels == 2) ? 2 : 1;
  sound->tstates_per_sec = tstates_per_sec;
  sound->cushion_ms      = cushion_ms;

  sound->ay = slopay_chip_create(AY_CLOCK_FREQ, sample_rate);
  if (sound->ay == NULL)
    goto failure;
  slopay_chip_set_stereo_mode(sound->ay, SLOPAY_CHIP_STEREO_MODE_MONO);
  slopay_chip_set_volume(sound->ay, AY_VOLUME_PCT);

  sound->queue = malloc(QUEUE_CAPACITY * sizeof(*sound->queue));
  if (sound->queue == NULL)
    goto failure;

  zxsound_set_speed(sound, 100);

  return sound;


failure:
  zxsound_destroy(sound);
  return NULL;
}

void zxsound_destroy(zxsound_t *sound)
{
  if (sound == NULL)
    return;

  free(sound->queue);
  if (sound->ay)
    slopay_chip_destroy(sound->ay);
  free(sound);
}

void zxsound_set_speed(zxsound_t *sound, int percent)
{
  uint32_t per_sec; /* T-states replayed per second of output */

  /* Every Spectrum clock is a whole multiple of 100Hz, so this is exact and
   * stays inside 32 bits up to 1000%. */
  per_sec = sound->tstates_per_sec / 100 * (uint32_t) percent;

  sound->step_int  = per_sec / (uint32_t) sound->sample_rate;
  sound->step_rem  = per_sec % (uint32_t) sound->sample_rate;
  sound->step_frac = 0;
  sound->cushion   = per_sec / 1000 * (uint32_t) sound->cushion_ms;
}

void zxsound_mute_speaker(zxsound_t *sound, int mute)
{
  sound->speaker_muted = mute;
}

void zxsound_enable_ay_channel(zxsound_t *sound, int channel, int enable)
{
  slopay_chip_enable_channel(sound->ay, channel, enable);
}

static void queue_push(zxsound_t *sound,
                       uint32_t   time,
                       int        type,
                       uint8_t    reg,
                       uint8_t    value)
{
  int      next_tail;
  event_t *ev;

  next_tail = (sound->queue_tail + 1) % QUEUE_CAPACITY;
  if (next_tail == sound->queue_head)
    return; /* full: drop the event */

  ev        = &sound->queue[sound->queue_tail];
  ev->time  = time;
  ev->type  = (uint8_t) type;
  ev->reg   = reg;
  ev->value = value;

  sound->queue_tail = next_tail;
}

void zxsound_speaker(zxsound_t *sound, int on_off, zxclock_t tstates)
{
  if (on_off == sound->speaker_last_level)
    return; /* level unchanged: no edge to reproduce */

  sound->speaker_last_level = on_off;

  queue_push(sound, (uint32_t) tstates, EVENT_SPEAKER, 0, (uint8_t) on_off);
}

void zxsound_ay_out(zxsound_t *sound,
                    uint16_t   port,
                    uint8_t    byte,
                    zxclock_t  tstates)
{
  if (port == port_AY_REGISTER)
  {
    /* Register select: always followed by its data write, which carries the
     * register with it, so it needs no queuing. */
    sound->ay_latched_reg = byte;
    return;
  }

  queue_push(sound, (uint32_t) tstates, EVENT_AY, sound->ay_latched_reg, byte);
}

/* Applies the queued events due within the next output sample's period and
 * returns the beeper contribution for that sample.
 *
 * Speaker level changes can come faster than the sample rate (the 48K engine
 * tone toggles every ~60 T-states, under one 44.1kHz sample), so sampling the
 * instantaneous level would alias or silence them. Instead the level is
 * integrated over the sample period (a box filter): the result is
 * BEEPER_AMPLITUDE scaled by the fraction of the period spent high. */
static int apply_due_events(zxsound_t *sound)
{
  uint32_t start;   /* sample period start, T-states */
  uint32_t end;     /* sample period end */
  uint32_t level_t; /* start of the current speaker level within the period */
  uint32_t high;    /* T-states spent high within the period */
  event_t *ev;
  uint32_t edge;

  if (sound->queue_head == sound->queue_tail)
  {
    sound->anchored = 0;

    /* No pending edges: the speaker holds its level for the whole period. */
    return sound->speaker_level ? BEEPER_AMPLITUDE : 0;
  }

  if (!sound->anchored)
  {
    sound->cursor    = sound->queue[sound->queue_head].time - sound->cushion;
    sound->step_frac = 0;
    sound->anchored  = 1;
  }

  start            = sound->cursor;
  end              = start + sound->step_int;
  sound->step_frac += sound->step_rem;
  if (sound->step_frac >= (uint32_t) sound->sample_rate)
  {
    sound->step_frac -= (uint32_t) sound->sample_rate;
    end++;
  }
  sound->cursor = end;

  level_t = start;
  high    = 0;

  while (sound->queue_head != sound->queue_tail)
  {
    ev = &sound->queue[sound->queue_head];

    if ((int32_t) (ev->time - end) > 0)
      break; /* not yet due */

    if (ev->type == EVENT_AY)
    {
      slopay_chip_write_register(sound->ay, ev->reg, ev->value);
    }
    else
    {
      edge = ev->time;
      if ((int32_t) (edge - start) < 0)
        edge = start; /* overdue edge: takes effect at period start */
      if (sound->speaker_level)
        high += edge - level_t;
      level_t              = edge;
      sound->speaker_level = ev->value;
    }

    sound->queue_head = (sound->queue_head + 1) % QUEUE_CAPACITY;
  }

  if (sound->speaker_level)
    high += end - level_t;

  if (end == start) /* only at very low speeds */
    return sound->speaker_level ? BEEPER_AMPLITUDE : 0;

  return (int) ((uint32_t) BEEPER_AMPLITUDE * high / (end - start));
}

/* One-pole highpass removing the DC bias of the box-filtered beeper level. */
static int dc_block(zxsound_t *sound, int beeper)
{
  int32_t in;
  int32_t out;

  in  = (int32_t) beeper * DC_BLOCK_STATE_ONE;
  out = (in - sound->dc_prev_in) + sound->dc_prev_out -
        sound->dc_prev_out * DC_BLOCK_ONE_MINUS_R / 32768;

  sound->dc_prev_in  = in;
  sound->dc_prev_out = out;

  return (int) (out / DC_BLOCK_STATE_ONE);
}

void zxsound_render(zxsound_t *sound, int16_t *buf, int frames)
{
  int i;
  int beeper;
  int ay;
  int mix;

  for (i = 0; i < frames; i++)
  {
    beeper = apply_due_events(sound);
    beeper = dc_block(sound, sound->speaker_muted ? 0 : beeper);

    /* Mono, so the low half is the whole signal */
    ay  = (int16_t) (slopay_chip_get_sample(sound->ay) & 0xFFFF);
    mix = ay * AY_GAIN_NUM / AY_GAIN_DEN + beeper;
    mix = CLAMP(mix, INT16_MIN, INT16_MAX);

    *buf++ = (int16_t) mix;
    if (sound->channels == 2)
      *buf++ = (int16_t) mix;
  }
}
