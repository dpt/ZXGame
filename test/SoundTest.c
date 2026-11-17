/* SoundTest.c: renders known speaker patterns through zxsound_t. */

#undef NDEBUG /* the checks are the test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "ZXSpectrum/Sound.h"
#include "ZXSpectrum/slopay-chip.h"

/* 3,500,000 / 50,000 gives exactly 70 T-states per output sample. */
#define RATE    (50000)
#define PERIOD  (70)
#define FRAMES  (3000)

static int16_t baseline_buf[FRAMES];
static int16_t buf[FRAMES];

/* Queue speaker edges every PERIOD T-states, starting at 'first', and render
 * FRAMES frames. */
static void render_square(int16_t *out, int cushion_ms, uint32_t first)
{
  zxsound_t *s;
  int        i;

  s = zxsound_create(RATE, Z80_CLOCK_48K, cushion_ms, 1);
  assert(s);
  /* A harmless AY write at T-state 0 anchors the replay cursor there, not at
   * the first edge. */
  zxsound_ay_out(s, port_AY_REGISTER, AY_REG_MIXER, 0);
  zxsound_ay_out(s, port_AY_DATA, AY_MIXER_ALL_OFF, 0);
  for (i = 0; i < FRAMES * 2; i++)
    zxsound_speaker(s, !(i & 1), first + (uint32_t) i * PERIOD);
  zxsound_render(s, out, FRAMES);
  zxsound_destroy(s);
}

int main(void)
{
  zxsound_t *s;
  int        i;
  int        d0;
  int        d1;

  /* Silence: no events, so output is constant (the idle AY level). */
  s = zxsound_create(RATE, Z80_CLOCK_48K, 0, 1);
  assert(s);
  zxsound_render(s, baseline_buf, FRAMES);
  zxsound_destroy(s);
  for (i = 1; i < FRAMES; i++)
    assert(baseline_buf[i] == baseline_buf[0]);

  /* Edges on sample boundaries: whole samples high then low, so after the
   * DC-block settles the output alternates either side of the baseline. */
  render_square(buf, 0, 0);
  for (i = 1000; i < FRAMES; i++)
  {
    d0 = buf[i] - baseline_buf[0];
    d1 = buf[i - 1] - baseline_buf[0];
    assert(d0 * d1 < 0);
    assert(abs(d0) > 32767 / 5 / 4);
  }

  /* Edges mid-sample: every sample is half high, a constant level the
   * DC-block removes, leaving the baseline. */
  render_square(buf, 0, PERIOD / 2);
  for (i = FRAMES - 100; i < FRAMES; i++)
    assert(abs(buf[i] - baseline_buf[0]) <= 2);

  /* A 1ms cushion (50 samples) delays the first edge by that much. */
  render_square(buf, 1, 0);
  for (i = 0; i < 50; i++)
    assert(buf[i] == baseline_buf[0]);
  assert(buf[50] != baseline_buf[0]);

  printf("SoundTest: ok\n");
  return 0;
}
