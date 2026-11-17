/* NullGame.c: a small test game exercising each front-end feature.
 *
 * - Screen drawing: a rotating box.
 * - Attributes: the box's ink cycles through a scrolling rainbow.
 * - Border: steps through all eight colours, one per second.
 * - Keyboard: a map of the 40 keys, each lit while held.
 * - Beeper: a held key sounds a tone, pitch rising along the keyboard.
 * - AY: a looping arpeggio on channel A.
 */
#include <stdlib.h>
#include <string.h>

#include "ZXGame/Game.h"
#include "ZXSpectrum/slopay-chip.h"

#define FRAME_48K       (69888) /* T-states per 50Hz frame */
#define FRAME_128K      (70908)
#define BEEP_HEADROOM   (1000)  /* T-states of each frame left to other OUTs */

#define BOX_CX          (128)   /* box centre, pixels */
#define BOX_CY          (72)
#define BOX_RADIUS      (60)    /* centre to corner, pixels */
#define FIX_SHIFT       (8)     /* corner fixed point fraction bits */
#define ROT_SHIFT       (5)     /* rotation step is about 1/32 radian */

#define BOX_ATTR_ROWS   (18)    /* attribute rows the box area covers */
#define KEYS_ROW        (19)    /* attribute row of the top keyboard row */

#define FRAMES_PER_NOTE (8)

typedef struct
{
  zxspectrum_t *zx;
  int           frame_tstates;
  unsigned int  frame;
  int           corner_x;   /* one corner, relative to centre, fixed point */
  int           corner_y;
  int           ear;        /* current speaker level */
}
nullgame_t;

/* The two half-row ports of each keyboard row, top to bottom. A right-hand
 * half-row numbers its bits from the outside in, so they run 4..0 across. */
static const uint16_t key_port[4][2] =
{
  { port_KEYBOARD_12345,     port_KEYBOARD_09876           },
  { port_KEYBOARD_QWERT,     port_KEYBOARD_POIUY           },
  { port_KEYBOARD_ASDFG,     port_KEYBOARD_ENTERLKJH       },
  { port_KEYBOARD_SHIFTZXCV, port_KEYBOARD_SPACESYMSHFTMNB }
};

/* Half-periods in T-states (3,500,000 / 2f) of C4..B4. Each octave up
 * halves them. */
static const int semitone_half[12] =
{
  6690, 6313, 5959, 5625, 5309, 5011, 4730, 4464, 4214, 3977, 3754, 3543
};

/* AY tone periods (1,773,400 / 16f) of C5, E5, G5 and C6. */
static const int arpeggio[4] = { 212, 168, 141, 106 };

static void *null_create(zxspectrum_t *zx)
{
  nullgame_t *game;

  game = calloc(1, sizeof(*game));
  if (game == NULL)
    return NULL;

  game->zx = zx;
  return game;
}

static void null_destroy(void *game)
{
  free(game);
}

/* Set the pixel at (x, y), (0, 0) being top left. */
static void plot(zxscreen_t *screen, int x, int y)
{
  int offset;

  if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
    return;

  /* Spectrum screen addressing: y bits 76 543 210 are stored as 76 210 543. */
  offset = ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | (x >> 3);
  screen->pixels[offset] |= 0x80 >> (x & 7);
}

static void line(zxscreen_t *screen, int x0, int y0, int x1, int y1)
{
  int dx;
  int dy;
  int sx;
  int sy;
  int err;
  int e2;

  dx  = abs(x1 - x0);
  dy  = -abs(y1 - y0);
  sx  = x0 < x1 ? 1 : -1;
  sy  = y0 < y1 ? 1 : -1;
  err = dx + dy;

  for (;;)
  {
    plot(screen, x0, y0);
    if (x0 == x1 && y0 == y1)
      break;
    e2 = 2 * err;
    if (e2 >= dy)
    {
      err += dy;
      x0  += sx;
    }
    if (e2 <= dx)
    {
      err += dx;
      y0  += sy;
    }
  }
}

static void draw_box(nullgame_t *game)
{
  zxscreen_t *screen;
  int         x;
  int         y;
  int         cx[4];
  int         cy[4];
  int         i;

  screen = &game->zx->screen;

  /* Minsky's circle algorithm: rotates without trigonometry and its orbit
   * stays closed, so the box neither grows nor shrinks. */
  game->corner_x -= game->corner_y >> ROT_SHIFT;
  game->corner_y += game->corner_x >> ROT_SHIFT;

  x = game->corner_x >> FIX_SHIFT;
  y = game->corner_y >> FIX_SHIFT;

  /* The other corners are the first turned by 90, 180 and 270 degrees. */
  cx[0] = BOX_CX + x; cy[0] = BOX_CY + y;
  cx[1] = BOX_CX - y; cy[1] = BOX_CY + x;
  cx[2] = BOX_CX - x; cy[2] = BOX_CY - y;
  cx[3] = BOX_CX + y; cy[3] = BOX_CY - x;

  memset(screen->pixels, 0, sizeof(screen->pixels));
  for (i = 0; i < 4; i++)
    line(screen, cx[i], cy[i], cx[(i + 1) & 3], cy[(i + 1) & 3]);
}

/* Colour the box area in bright rainbow bands that scroll upwards. */
static void colour_box_area(nullgame_t *game)
{
  attribute_t *attrs;
  int          row;
  int          ink;

  attrs = game->zx->screen.attributes;
  for (row = 0; row < BOX_ATTR_ROWS; row++)
  {
    ink = 1 + (row + game->frame / 4) % 7; /* blue..white, never black */
    memset(&attrs[row * SCREEN_ATTRIBUTES_WIDTH],
           MKATTR(0, 1, ATTR_BLACK, ink),
           SCREEN_ATTRIBUTES_WIDTH);
  }
}

/* Light each held key in the keyboard map, two cells wide with a one cell
 * gap. Returns the index of the last held key (0..39), or -1 if none. */
static int scan_keyboard(nullgame_t *game)
{
  zxspectrum_t *zx;
  attribute_t  *attrs;
  int           held;
  int           row;
  int           half;
  int           bits;
  int           i;
  int           key;
  int           bit;
  attribute_t   attr;

  zx    = game->zx;
  attrs = zx->screen.attributes;
  held  = -1;

  for (row = 0; row < 4; row++)
  {
    for (half = 0; half < 2; half++)
    {
      bits = zx->in(zx, key_port[row][half]); /* active low */
      for (i = 0; i < 5; i++)
      {
        key  = row * 10 + half * 5 + i;
        bit  = half ? 4 - i : i;
        attr = MKATTR(0, 0, ATTR_BLUE, ATTR_WHITE);
        if ((bits & (1 << bit)) == 0)
        {
          attr = MKATTR(0, 1, ATTR_GREEN, ATTR_WHITE);
          held = key;
        }
        memset(&attrs[(KEYS_ROW + row) * SCREEN_ATTRIBUTES_WIDTH +
                      1 + (key % 10) * 3],
               attr, 2);
      }
    }
  }

  return held;
}

/* Fill the frame with a square wave for the held key, pitch rising one
 * semitone per key. Every OUT also sets the border. */
static void beep(nullgame_t *game, int key, int border)
{
  zxspectrum_t *zx;
  int           half;
  int           n;

  zx = game->zx;

  if (key < 0)
  {
    game->ear = 0;
    zx->out(zx, port_BORDER_EAR_MIC, (uint8_t) border);
    return;
  }

  half = semitone_half[key % 12] >> (key / 12);
  for (n = (game->frame_tstates - BEEP_HEADROOM) / half; n > 0; n--)
  {
    game->ear = !game->ear;
    zx->out(zx, port_BORDER_EAR_MIC,
            (uint8_t) (border | (game->ear ? port_MASK_EAR : 0)));
    zx->logtime(zx, half - 11); /* the OUT bills its own 11 */
  }
}

static void ay_write(zxspectrum_t *zx, int reg, int value)
{
  zx->out(zx, port_AY_REGISTER, (uint8_t) reg);
  zx->out(zx, port_AY_DATA, (uint8_t) value);
}

/* A plucked arpeggio: each note starts loud and decays. */
static void play_ay(nullgame_t *game)
{
  zxspectrum_t *zx;
  int           step;
  int           period;

  zx   = game->zx;
  step = game->frame % FRAMES_PER_NOTE;

  if (step == 0)
  {
    period = arpeggio[(game->frame / FRAMES_PER_NOTE) % 4];
    ay_write(zx, AY_REG_CHANNEL_A_FINE_PITCH, period & 0xFF);
    ay_write(zx, AY_REG_CHANNEL_A_COARSE_PITCH, period >> 8);
  }
  ay_write(zx, AY_REG_CHANNEL_A_VOLUME, 13 - step);
}

static void null_run(void *opaque, const zxgame_launch_t *launch)
{
  nullgame_t   *game;
  zxspectrum_t *zx;
  int           border;
  int           key;

  game = opaque;
  zx   = game->zx;

  game->frame_tstates = launch->model == ZXGAME_MODEL_128K ? FRAME_128K
                                                           : FRAME_48K;
  game->corner_x      = BOX_RADIUS << FIX_SHIFT;
  game->corner_y      = 0;

  ay_write(zx, AY_REG_MIXER, AY_MIXER_ALL_OFF & ~AY_MIXER_NO_TONE_A);

  while (!zxspectrum_quit_requested(zx))
  {
    zx->stamp(zx);

    border = (game->frame / 50) % 8;

    draw_box(game);
    colour_box_area(game);
    key = scan_keyboard(game);
    zx->draw(zx, NULL);

    play_ay(game);
    beep(game, key, border);

    game->frame++;
    if (zx->sleep(zx, game->frame_tstates))
      break;
  }

  ay_write(zx, AY_REG_CHANNEL_A_VOLUME, 0);
}

static const zxgame_ops_t null_ops =
{
  "Null Game",
  ZXGAME_MODEL_48K | ZXGAME_MODEL_128K,
  ZXGAME_MODEL_128K,
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
