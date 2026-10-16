/* HeadlessHost.c
 *
 * Headless front-end for ZXGame ports.
 *
 * Runs the game as fast as possible for a fixed number of frames (timed
 * segments) with no display or sound output. Useful for smoke tests.
 *
 * Derived from the Great Escape in C headless front-end. (c) David Thomas,
 * 2017-2026.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ZXSpectrum/Keyboard.h"
#include "ZXSpectrum/Spectrum.h"

#include "ZXGame/Game.h"

/* -------------------------------------------------------------------------- */

#define DEFAULT_FRAMES (100000)

typedef struct headless
{
  const zxgame_ops_t *ops;
  unsigned            frames;     /* sleep() calls so far */
  unsigned            max_frames;
  unsigned            polls;      /* key port reads so far */
}
headless_t;

static void draw_handler(const zxbox_t *dirty, void *opaque)
{
  (void) dirty;
  (void) opaque;
}

static void stamp_handler(void *opaque)
{
  (void) opaque;
}

static int sleep_handler(int duration, void *opaque)
{
  headless_t *h = opaque;

  (void) duration;

  /* Return immediately: run the game as fast as possible. */
  return ++h->frames >= h->max_frames;
}

static int key_handler(uint16_t port, void *opaque)
{
  headless_t *h = opaque;

  h->polls++;

  if (h->ops->scripted_key != NULL)
    return h->ops->scripted_key(port, h->polls);

  return port == port_KEMPSTON_JOYSTICK ? 0 : 0x1F;
}

static void border_handler(int colour, void *opaque)
{
  (void) colour;
  (void) opaque;
}

static void speaker_handler(int on_off, zxclock_t tstates, void *opaque)
{
  (void) on_off;
  (void) tstates;
  (void) opaque;
}

/* -------------------------------------------------------------------------- */

int zxgame_headless_main(int argc, char *argv[], const zxgame_ops_t *ops)
{
  headless_t      h;
  zxconfig_t      zxconfig;
  zxspectrum_t   *zx;
  void           *game;
  zxgame_launch_t launch;
  clock_t         start, end;
  double          secs;

  h.ops        = ops;
  h.frames     = 0;
  h.max_frames = DEFAULT_FRAMES;
  h.polls      = 0;

  if (argc > 1)
  {
    if (strcmp(argv[1], "-frames") == 0 && argc > 2)
    {
      h.max_frames = (unsigned) strtoul(argv[2], NULL, 10);
    }
    else
    {
      fprintf(stderr, "Usage: %s [-frames count]\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  printf("%s\n", ops->name);
  printf("Initialising...\n");

  memset(&zxconfig, 0, sizeof(zxconfig));
  zxconfig.width        = SCREEN_WIDTH / 8;
  zxconfig.height       = SCREEN_HEIGHT / 8;
  zxconfig.opaque       = &h;
  zxconfig.draw         = &draw_handler;
  zxconfig.stamp        = &stamp_handler;
  zxconfig.sleep        = &sleep_handler;
  zxconfig.key          = &key_handler;
  zxconfig.border       = &border_handler;
  zxconfig.speaker      = &speaker_handler;
  zxconfig.pixel_format = ZX_PIXEL_ARGB8888;

  zx = zxspectrum_create(&zxconfig);
  if (zx == NULL)
    return EXIT_FAILURE;

  game = ops->create(zx);
  if (game == NULL)
  {
    zxspectrum_destroy(zx);
    return EXIT_FAILURE;
  }

  launch.model     = ops->default_model;
  launch.load_file = NULL;

  printf("Running...\n");
  start = clock();
  if (ops->run != NULL)
  {
    ops->run(game, &launch);
  }
  else
  {
    while (!ops->step(game, &launch))
      ;
  }
  end = clock();

  secs = (double) (end - start) / CLOCKS_PER_SEC;
  printf("%u frames in %.0fms = %.2f frames/sec\n",
         h.frames,
         secs * 1000.0,
         secs > 0.0 ? (double) h.frames / secs : 0.0);

  ops->destroy(game);
  zxspectrum_destroy(zx);

  printf("(quit)\n");

  return EXIT_SUCCESS;
}

/* vim: ts=8 sts=2 sw=2 et */
