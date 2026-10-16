/* NullGame.c: smallest possible game, used to smoke-test the front-ends. */
#include <stdlib.h>

#include "ZXGame/Game.h"

static void *null_create(zxspectrum_t *zx) { return zx; }
static void null_destroy(void *game) { (void) game; }

static void null_run(void *game, const zxgame_launch_t *launch)
{
  zxspectrum_t *zx = game;

  (void) launch;
  while (!zxspectrum_quit_requested(zx))
  {
    zx->stamp(zx);
    zx->sleep(zx, 69888);
  }
}

static const zxgame_ops_t null_ops =
{
  "Null Game", ZXGAME_MODEL_48K, ZXGAME_MODEL_48K,
  null_create, null_destroy, null_run
};

int main(int argc, char *argv[])
{
#ifdef NULLGAME_SDL
  return zxgame_sdl3_main(argc, argv, &null_ops);
#else
  return zxgame_headless_main(argc, argv, &null_ops);
#endif
}
