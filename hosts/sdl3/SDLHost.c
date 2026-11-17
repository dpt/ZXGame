/**
 * SDLHost.c
 *
 * SDL3 front-end for ZXGame ports of ZX Spectrum games.
 *
 * Derived from the Chase H.Q. in C SDL front-end. Copyright (c) 2013-2026
 * David Thomas. Free software provided without warranty in the interests of
 * education and software preservation.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include "ZXSpectrum/Macros.h"
#include "ZXSpectrum/Spectrum.h"
#include "ZXSpectrum/Keyboard.h"
#include "ZXSpectrum/Kempston.h"
#include "ZXSpectrum/Sound.h"

#include "ZXGame/Game.h"

#include "CRTShader.h"


// -----------------------------------------------------------------------------

/* Configuration
 *
 */
#define BORDER              (16)

#define SCALE_DEFAULT        (4)
#define SCALE_MIN            (1)
#define SCALE_MAX            (4)

#define SPEED_DEFAULT      (100) // percent
#define SPEED_MIN            (1)
#define SPEED_MAX         (1000)
#define SPEED_STEP           (5)

#define VOLUME_DEFAULT     (100) // percent
#define VOLUME_MIN           (0)
#define VOLUME_MAX         (100)
#define VOLUME_STEP         (10)

/* Mouse steering: relative motion accumulates into a virtual wheel position
 * (pixels), clamped to +-MOUSE_WHEEL_RANGE. Past MOUSE_WHEEL_DEADZONE either
 * side, kempston LEFT/RIGHT is asserted -- Kempston has no analogue axis, so
 * the wheel position only ever decides a digital direction.
 */
#define MOUSE_WHEEL_RANGE      (150)
#define MOUSE_WHEEL_DEADZONE    (15)

/* Whether the CRT post-effect starts switched on. Both display backends are
 * always built: the plain SDL_Renderer blit (any GPU backend,
 * nearest-neighbour scaling) and DPTLib's CRT post-effect. F4 switches between
 * them at runtime, so this only chooses which one comes up first. On when
 * DPTLib has a shader backend for the target platform; override with
 * -DZXH_CRT_SHADER=1/0.
 *
 * They cannot both be live at once -- SDL_CreateRenderer and
 * SDL_ClaimWindowForGPUDevice each want the window's swapchain -- so
 * switching tears one down and builds the other. If the CRT backend will not
 * start, zxh_set_crt_enabled falls back to the plain renderer.
 */
#ifndef ZXH_CRT_SHADER
#ifdef DPTLIB_CRT_AVAILABLE
#define ZXH_CRT_SHADER       (1)
#else
#define ZXH_CRT_SHADER       (0)
#endif
#endif

#define MAXDIRTYRECTS        (8) // max dirty rects captured per frame before we coalesce to full-screen

#define AY_SAMPLE_RATE   (44100) // preferred output rate; see audio.sample_rate

/* Replay cursor anchor cushion: the output latency zxsound_t adds each time
 * its queue runs dry and it re-anchors (see Sound.h). Emscripten's game
 * thread falls behind wall-clock more often (pthread scheduling jitter, no
 * realtime priority), so it needs more
 * lead time before playback catches up to avoid repeated re-anchors -- each
 * one is an audible stutter, and it happens often enough during dense speech
 * playback to sound like scratchiness.
 *
 * This cuts both ways: the cushion is also how long the AY and beeper hold
 * their last state after the queue runs dry, so an over-large value turns
 * every starve into a long low chug rather than a short click. It was 120ms
 * while the browser build starved constantly (a 512-frame main-thread
 * ScriptProcessorNode, plus a per-sample mutex on that same thread); with
 * both of those fixed the starves should be rare enough that a cushion only
 * modestly above desktop's is enough.
 */
#ifdef __EMSCRIPTEN__
#define SOUND_CUSHION_MS (50)
#else
#define SOUND_CUSHION_MS (30)
#endif

/* How far the game thread may fall behind its schedule before the backlog is
 * written off rather than caught up. Roughly the four 20ms frames the pacing
 * loop was originally built around; see zxh_sleep_handler. The RISC OS host
 * caps the same backlog with ZXH_MAX_LAG_FRAMES, counted in centisecond ticks
 * rather than wall-clock seconds.
 */
#define ZXH_MAX_LAG_SECS (0.08)

/* Waits shorter than this are skipped and left on the deadline for the next
 * call to honour. Nothing on this host can time a sub-millisecond wait: the
 * sleep rounds up to Atomics.wait's ~1ms floor, and a busy-wait is worse
 * still, because the clock it polls only moves in coarse steps and so
 * overshoots by however long the step is. Deferring costs nothing and the
 * debt is never lost, so the long-run rate stays right.
 */
#define ZXH_MIN_WAIT_SECS (0.002)

// -----------------------------------------------------------------------------

// All effects disabled: 0.0f knobs off, 1.0f leaves brightness/contrast/
// saturation unmodified. Used by Ctrl-R to zero out the shader's look.
static const zxh_CRT_params_t crt_default_params = {
  0.0f,
  1.0f,
  0.0f,
  1.0f,
  1.0f,
  1.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f
};

// Defaults tuned by eye against this game's screen.
static const zxh_CRT_params_t crt_tuned_params = {
  0.025f,
  0.5f,
  0.025f,
  1.1f,
  1.1f,
  1.0f,
  0.75f,
  0.3f,
  0.6f,
  0.0f
};

// -----------------------------------------------------------------------------

static int zxh_window_width(int scale)
{
  return (SCREEN_WIDTH + BORDER * 2) * scale;
}

static int zxh_window_height(int scale)
{
  return (SCREEN_HEIGHT + BORDER * 2) * scale;
}

// -----------------------------------------------------------------------------

// zxh_sdl_state_t.flags bits
#define ZXH_FLAG_QUIT      (1u << 0)
#define ZXH_FLAG_PAUSED    (1u << 1)
#define ZXH_FLAG_MODE_128K (1u << 2) // 0 selects the 48K entry point
#define ZXH_FLAG_CLOSED    (1u << 3) // instance already torn down (two-up mode)

#define ZXH_FLAG_TEST(state, flag)   (((state)->flags & (flag)) != 0)
#define ZXH_FLAG_SET(state, flag)    ((state)->flags |= (flag))
#define ZXH_FLAG_CLEAR(state, flag)  ((state)->flags &= ~(flag))
#define ZXH_FLAG_ASSIGN(state, flag, on) \
  ((on) ? ZXH_FLAG_SET(state, flag) : ZXH_FLAG_CLEAR(state, flag))

typedef struct zxh_sdl_state
{
  zxspectrum_t     *zx;
  const zxgame_ops_t *ops;
  zxgame_launch_t     launch;
  void       *game;

  zxkeyset_t        keys;
  zxkempston_t      kempston;
  int               mouse_wheel; // relative-motion accumulator driving steering, +-MOUSE_WHEEL_RANGE
  int               mouse_steering; // bool; toggled with Ctrl-G, off by default

  int               instance;   // 0-based index; only used to label the window title in n-up mode
  int               instance_count; // total instances launched; >1 shows the instance number in the title

  unsigned int      flags;      // ZXH_FLAG_* bits: quit, paused, mode_128k

  int               speed;      // game speed, percent, SPEED_MIN..SPEED_MAX

  /* Absolute monotonic-clock deadline for the next sleep, advanced by each
   * call's nominal duration rather than re-anchored from "now" -- see
   * zxh_sleep_handler for why. */
  double            next_deadline;  // seconds, SDL_GetTicksNS()-epoch
  int               deadline_valid; // bool

  struct
  {
    int             volume; // output volume, percent, VOLUME_MIN..VOLUME_MAX

    /* Rate the AY is emulated at and the replay cursor counts in. Taken from
     * the opened device rather than fixed at AY_SAMPLE_RATE so SDL has no
     * resampling to do: SDL3's Emscripten backend forces the device to the
     * browser's audioContext.sampleRate (48kHz on most Windows machines) and
     * that resample would otherwise run on the same main thread as the
     * ScriptProcessorNode callback and the render frame.
     */
    int             sample_rate;

    /* Speaker and AY renderer. The game thread queues events, the audio
     * thread renders them; lock serialises every call into it. */
    zxsound_t      *sound;
    SDL_Mutex      *lock;

    int             muted;                // bool; mute audio if true
    int             ay_channel_muted[3];  // bool per AY channel A/B/C; toggled with F7/F8/F9
    int             speaker_muted;        // bool; beeper sfx muted; toggled with F10

    SDL_AudioStream *stream;
  }
  audio;

  struct
  {
    int             scale;      // window/render scale, SCALE_MIN..SCALE_MAX
    int             fullscreen; // bool; toggled with F11

    /* Dirty-rect overlay: the game (thread) reports each screen region it
     * refreshes via zxh_draw_handler; we stash them here and outline them
     * over the rendered frame so refreshed regions are visible on screen.
     * Cleared once drawn. dirty_mutex guards all fields in this group.
     */
    SDL_Mutex      *dirty_mutex;
    zxbox_t         dirty_rects[MAXDIRTYRECTS];
    int             dirty_count;
    int             dirty_full_screen; // bool; a NULL dirty box was reported (whole screen)
    int             show_dirty_overlay; // bool; toggled with F3, off by default
    int             monochrome; // bool; toggled with Ctrl-M, off by default
    int             mellow;     // bool; toggled with Ctrl-N, off by default
    int             show_backbuffer; // bool; toggled with F12, off by default
    int             border;     // border colour 0..7, set by the game thread

    SDL_Window     *window;

    /* Display backend. Exactly one of these is live at a time, selected by
     * crt_enabled and swapped by zxh_set_crt_enabled: crt when enabled,
     * renderer/texture when not. The other's handles are NULL.
     */
    int              crt_enabled; // bool; toggled with F4
    zxh_CRT_shader_t crt;
    zxh_CRT_params_t crt_params;
    int              crt_param_index; // which crt_params field +/- adjusts
    SDL_Renderer   *renderer;
    SDL_Texture    *texture;

    /* CRT-style config-change OSD: brief bright-green status text at the
     * bottom of the picture, e.g. "VOLUME 80 PC". osd_shown_at_ms == 0 means
     * inactive; set by zxh_osd_show, read by zxh_draw_osd.
     */
    char            osd_text[32];
    Uint64          osd_shown_at_ms;
    u8              osd_mask[SCREEN_WIDTH * SCREEN_HEIGHT]; // scratch buffer for zxh_render_osd_mask
  }
  video;

  SDL_Thread       *game_thread;
}
zxh_sdl_state_t;

/* ----------------------------------------------------------------------- */

/* The title bar doubles as the status display for the F-key controls: the
 * game itself has nowhere to show them.
 */
static void zxh_update_window_title(const zxh_sdl_state_t *state)
{
  char title[128];
  char label[48];

  if (state->instance_count > 1)
    SDL_snprintf(label, sizeof(label), "%s #%d", state->ops->name, state->instance + 1);
  else
    SDL_snprintf(label, sizeof(label), "%s", state->ops->name);

  SDL_snprintf(title, sizeof(title),
               "%s - Speed: %d%%%s - Volume: %d%%%s",
               label,
               state->speed,
               ZXH_FLAG_TEST(state, ZXH_FLAG_PAUSED) ? " (Paused)" : "",
               state->audio.volume,
               state->audio.muted ? " (Muted)" : "");
  SDL_SetWindowTitle(state->video.window, title);
}

static void zxh_draw_handler(const zxbox_t *dirty,
                             void          *opaque)
{
  zxh_sdl_state_t *state = opaque;

  /* SDL_UpdateTexture must be called from the main thread (Metal requirement).
   * The main loop picks up changes via zxspectrum_claim_screen. Here we only
   * stash the dirty region (game thread) for the main loop to outline once it
   * renders the frame.
   */
  SDL_LockMutex(state->video.dirty_mutex);
  if (dirty == NULL)
    state->video.dirty_full_screen = 1;
  else if (state->video.dirty_count < MAXDIRTYRECTS)
    state->video.dirty_rects[state->video.dirty_count++] = *dirty;
  SDL_UnlockMutex(state->video.dirty_mutex);
}

static void zxh_stamp_handler(void *opaque)
{
  // Nothing to do: sleeps run to a fixed deadline schedule, not from stamps
  (void) opaque;
}

static int zxh_sleep_handler(int durationTStates, void *opaque)
{
  zxh_sdl_state_t *state = opaque;
  int              paused;

  // Quit straight away if signalled
  if (ZXH_FLAG_TEST(state, ZXH_FLAG_QUIT))
    return 1;

  paused = ZXH_FLAG_TEST(state, ZXH_FLAG_PAUSED);
  if (paused)
  {
    // If paused, sit in this loop, checking twice per second for unpausing or
    // quitting
    for (;;)
    {
      paused = ZXH_FLAG_TEST(state, ZXH_FLAG_PAUSED);
      if (!paused)
        break;

      if (ZXH_FLAG_TEST(state, ZXH_FLAG_QUIT))
        return 1;

      SDL_Delay(500); // 0.5s
    }
  }
  else
  {
    /* A Spectrum 48K's Z80 runs at 3.5MHz (3,500,000 T-states per second).
     * A 128K runs its Z80 faster, at 3.5469MHz (3,546,900 T-states per
     * second) -- pacing every mode at the 48K rate throttles wall-clock time
     * per T-state on a 128K, which slows the beeper's real-time toggle rate
     * (timed by how fast the game thread actually runs) without affecting
     * the AY chip's own tone pitch (computed from its own fixed clock,
     * independent of this loop) -- the two drift ~1.3% apart.
     */
    const double   tstatesPerSec = ZXH_FLAG_TEST(state, ZXH_FLAG_MODE_128K)
                                     ? (double) Z80_CLOCK_128K
                                     : (double) Z80_CLOCK_48K;

    Uint64         nowNs;
    double         nowSecs;
    double         duration; // seconds

    nowNs   = SDL_GetTicksNS(); // get time now before anything else
    nowSecs = nowNs / 1e9;

    /* 'duration' tells us how long the operation should take. Turn T-state
     * duration into seconds. */
    duration = durationTStates / tstatesPerSec;
    // Adjust the game speed: higher speed -> shorter sleep
    duration = duration * 100 / state->speed;

    /* Pace off an absolute deadline that advances by 'duration' every call,
     * rather than re-anchoring from 'now' each time. The delay call on this
     * host routinely overshoots its requested delay by a few ms (OS scheduler
     * granularity); re-anchoring from 'now' every call bakes that overshoot
     * into every single frame with nothing to claw it back, producing a
     * steady-state frame rate well under 50Hz (measured ~42fps) even though
     * each call's own math looks correct in isolation. Advancing a
     * fixed schedule instead means an overshoot simply shortens (or zeroes)
     * the next call's sleep, so the long-run average rate is right.
     */
    if (!state->deadline_valid)
    {
      state->next_deadline  = nowSecs + duration;
      state->deadline_valid = 1;
    }
    else
    {
      state->next_deadline += duration;

      /* Behind by a long way (paused, breakpoint, host stall) -- don't try to
       * fast-forward through the whole backlog.
       *
       * The cap is an absolute span, not a multiple of 'duration'. It used to
       * be four times whatever the caller asked for, which reads sensibly for
       * the 20ms frame sleeps it was written against (80ms of catch-up) but
       * scales with the request: play_speech_128k asks for
       * SPEECH_NIBBLE_TSTATES, 126us, which shrank the budget to 504us. Any
       * stall longer than that -- and under emscripten the host clock's own
       * granularity is longer than that -- moved the deadline forward and
       * discarded the debt, so the game clock quietly lost time it could
       * never claw back. Sampled sound played back around twelve times slow.
       */
      if (state->next_deadline < nowSecs - ZXH_MAX_LAG_SECS)
        state->next_deadline = nowSecs - ZXH_MAX_LAG_SECS;
    }

    if (state->next_deadline > nowSecs)
    {
      double delay = state->next_deadline - nowSecs; // seconds

      /* Anything this short is left on the deadline rather than waited for.
       * play_speech_128k asks for SPEECH_NIBBLE_TSTATES (447 T-states, 126us)
       * between nibbles, far below what the host can time; trying to honour
       * each one individually overshoots every time. Skipping lets the
       * nibbles run back to back and the accumulated debt trips a wait that
       * is long enough to be honoured accurately.
       */
      if (delay >= ZXH_MIN_WAIT_SECS)
        SDL_DelayNS((Uint64) (delay * 1e9));
    }
  }

  return 0;
}

static int zxh_key_handler(uint16_t port, void *opaque)
{
  zxh_sdl_state_t *state = opaque;

  if (port == port_KEMPSTON_JOYSTICK)
    return state->kempston;
  else
    return zxkeyset_for_port(port, &state->keys);
}

static void zxh_border_handler(int colour, void *opaque)
{
  zxh_sdl_state_t *state = opaque;

  state->video.border = colour;
}

static void zxh_speaker_handler(int on_off, zxclock_t tstates, void *opaque)
{
  zxh_sdl_state_t *state = opaque;

  SDL_LockMutex(state->audio.lock);
  zxsound_speaker(state->audio.sound, on_off, tstates);
  SDL_UnlockMutex(state->audio.lock);
}

static void zxh_ay_out_handler(uint16_t  port,
                               uint8_t   byte,
                               zxclock_t tstates,
                               void     *opaque)
{
  zxh_sdl_state_t *state = opaque;

  SDL_LockMutex(state->audio.lock);
  zxsound_ay_out(state->audio.sound, port, byte, tstates);
  SDL_UnlockMutex(state->audio.lock);
}

// Runs on SDL's audio thread. Called whenever SDL wants more data queued.
//
// The lock is taken once per chunk rather than once per sample because
// SDL3's Emscripten backend runs this on the main browser thread, which
// cannot Atomics.wait -- a contended lock spin-waits there, and the game
// Worker contends heavily during speech playback (three AY writes per
// nibble). 44kHz of lock traffic on the thread that also runs the render
// frame is what starves the ScriptProcessorNode.
static void zxh_audio_callback(void            *opaque,
                               SDL_AudioStream *stream,
                               int              additional_amount,
                               int              total_amount)
{
  zxh_sdl_state_t *state = opaque;
  int16_t          buf[512]; // 256 stereo pairs per chunk
  int              npairs;
  int              framebytes; // bytes per stereo frame (L + R, 16-bit)
  int              chunkbytes;
  int              volume;
  int              i;

  NOT_USED(total_amount);

  // The device is opened (and, on Emscripten, its ScriptProcessorNode
  // connected) before the renderer is created at the device's own rate.
  if (state->audio.sound == NULL)
    return;

  framebytes = 2 * (int) sizeof(*buf);
  volume     = state->audio.muted ? 0 : state->audio.volume;

  while (additional_amount > 0)
  {
    npairs = additional_amount / framebytes;
    if (npairs > (int) (NELEMS(buf) / 2))
      npairs = NELEMS(buf) / 2;
    if (npairs <= 0)
      break;

    SDL_LockMutex(state->audio.lock);
    zxsound_render(state->audio.sound, buf, npairs);
    SDL_UnlockMutex(state->audio.lock);

    if (volume != 100)
      for (i = 0; i < npairs * 2; i++)
        buf[i] = (int16_t) (buf[i] * volume / 100);

    chunkbytes = npairs * framebytes;
    SDL_PutAudioStreamData(stream, buf, chunkbytes);
    additional_amount -= chunkbytes;
  }
}

static int zxh_game_thread(void *opaque)
{
  zxh_sdl_state_t *state = opaque;

  if (state->ops->run != NULL)
  {
    state->ops->run(state->game, &state->launch);
  }
  else
  {
    while (!state->ops->step(state->game, &state->launch))
      ;
  }
  ZXH_FLAG_SET(state, ZXH_FLAG_QUIT);
  return 0;
}

/* CRT shader tuning knobs, cycled with TAB (Shift-TAB steps backwards) and
 * adjusted with PAGEUP/PAGEDOWN (see zxh_sdl_key_pressed).
 *
 * [offset] indexes into zxh_CRT_params_t so one table drives all the controls
 * instead of one keybinding per field.
 */
typedef struct zxh_crt_param_desc
{
  const char *name;
  size_t      offset;
  float       step;
  float       min, max;
}
zxh_crt_param_desc_t;

static const zxh_crt_param_desc_t zxh_crt_param_descs[] =
{
  { "curvature",          offsetof(zxh_CRT_params_t, curvature),          0.005f, 0.0f, 0.2f },
  { "bloom threshold",    offsetof(zxh_CRT_params_t, bloom_threshold),    0.05f,  0.0f, 1.0f },
  { "bloom intensity",    offsetof(zxh_CRT_params_t, bloom_intensity),    0.005f, 0.0f, 0.2f },
  { "brightness",         offsetof(zxh_CRT_params_t, brightness),         0.05f,  0.5f, 2.0f },
  { "contrast",           offsetof(zxh_CRT_params_t, contrast),           0.05f,  0.5f, 2.0f },
  { "saturation",         offsetof(zxh_CRT_params_t, saturation),         0.05f,  0.0f, 2.0f },
  { "scanline intensity", offsetof(zxh_CRT_params_t, scanline_intensity), 0.05f,  0.0f, 1.0f },
  { "vignette strength",  offsetof(zxh_CRT_params_t, vignette_strength),  0.05f,  0.0f, 1.0f },
  { "chroma bleed",       offsetof(zxh_CRT_params_t, chroma_bleed),       0.05f,  0.0f, 1.0f },
  { "glitch",             offsetof(zxh_CRT_params_t, glitch),             0.05f,  0.0f, 1.0f },
};

#define ZXH_CRT_PARAM_COUNT (int) NELEMS(zxh_crt_param_descs)

static float *zxh_crt_param_field(zxh_CRT_params_t           *params,
                                  const zxh_crt_param_desc_t *desc)
{
  return (float *) ((char *) params + desc->offset);
}

/* ----------------------------------------------------------------------- */

/* Display backend setup. Only one backend may hold the window at a time, so
 * each of these tears its own down completely before the other is built.
 */

static void zxh_renderer_destroy(zxh_sdl_state_t *state)
{
  if (state->video.texture != NULL)
  {
    SDL_DestroyTexture(state->video.texture);
    state->video.texture = NULL;
  }
  if (state->video.renderer != NULL)
  {
    SDL_DestroyRenderer(state->video.renderer);
    state->video.renderer = NULL;
  }
}

static int zxh_renderer_create(zxh_sdl_state_t *state)
{
  /* Every failure below tears down what it got so far, so the caller can
   * read "renderer == NULL" as "no plain renderer" without having to know
   * how far this got.
   */
  state->video.renderer = SDL_CreateRenderer(state->video.window, NULL);
  if (state->video.renderer == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateRenderer: %s\n", SDL_GetError());
    return 0;
  }

  SDL_SetRenderVSync(state->video.renderer, 1);

  /* The screen buffer is always converted with R in the lowest memory byte
   * (zxconfig.pixel_format below), because that is what the CRT backend's
   * R8G8B8A8 GPU texture requires and the backend can change at any time.
   * SDL_PIXELFORMAT_ABGR8888 is the same order for this texture; SDL
   * converts on upload if the renderer would rather have something else.
   */
  state->video.texture = SDL_CreateTexture(state->video.renderer,
                                           SDL_PIXELFORMAT_ABGR8888,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           SCREEN_WIDTH, SCREEN_HEIGHT);
  if (state->video.texture == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateTexture: %s\n", SDL_GetError());
    zxh_renderer_destroy(state);
    return 0;
  }

  if (!SDL_SetTextureBlendMode(state->video.texture, SDL_BLENDMODE_NONE))
  {
    fprintf(stderr, "Error: SDL_SetTextureBlendMode: %s\n", SDL_GetError());
    zxh_renderer_destroy(state);
    return 0;
  }

  /* Conv: nearest-neighbour keeps ZX Spectrum pixels crisp when the window
   * is scaled up; SDL3's default is linear, which blurs them.
   */
  SDL_SetTextureScaleMode(state->video.texture, SDL_SCALEMODE_NEAREST);

  return 1;
}

/* Conv: on Linux (Wayland/X11), claiming a GPU swapchain on a window that
 * already had a swapchain of any kind (SDL_Renderer's or a previous GPU
 * claim) torn down on it presents every frame without error but the
 * compositor never shows the new frames -- it sticks on the backend that
 * was live before the teardown. Destroying and recreating the SDL_Window
 * itself gives the new GPU claim a surface the compositor has never seen
 * before, which does update normally. Metal/macOS doesn't hit this, but the
 * recreation is harmless there too -- it isn't gated to Linux only.
 */
#if !defined(__EMSCRIPTEN__)
static int zxh_video_recreate_window(zxh_sdl_state_t *state)
{
  char            title[128];
  int             x;
  int             y;
  int             w;
  int             h;
  SDL_WindowFlags flags;
  SDL_Window *window;

  SDL_strlcpy(title, SDL_GetWindowTitle(state->video.window), sizeof(title));
  SDL_GetWindowPosition(state->video.window, &x, &y);
  SDL_GetWindowSize(state->video.window, &w, &h);
  flags = SDL_GetWindowFlags(state->video.window);

  window = SDL_CreateWindow(title, w, h, flags);
  if (window == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateWindow (recreate): %s\n", SDL_GetError());
    return 0;
  }
  SDL_SetWindowPosition(window, x, y);

  SDL_DestroyWindow(state->video.window);
  state->video.window = window;
  return 1;
}
#endif /* !__EMSCRIPTEN__ */

/* Switches the display backend. Returns 1 if the requested backend is now
 * live, 0 if it could not be created and the other one was restored instead
 * (the CRT backend needs Metal on macOS or Vulkan/SPIR-V on Linux, so this
 * is the normal result on a platform without a shader backend).
 */
static int zxh_set_crt_enabled(zxh_sdl_state_t *state, int enable)
{
  if (enable == state->video.crt_enabled &&
      (state->video.crt_enabled || state->video.renderer != NULL))
    return 1; // already in the requested state

  if (enable)
  {
#if defined(__EMSCRIPTEN__)
    /* Conv: Emscripten's SDL video driver backs every SDL_Window with the
     * same single <canvas> element -- SDL_CreateWindow does not hand back a
     * fresh compositor surface the way it does on Linux/macOS, so the
     * recreate-window workaround below (needed to dodge the stuck-swapchain
     * bug on those platforms) instead leaves the canvas with no live GL
     * context: zxh_CRT_shader_create "succeeds" but nothing ever draws.
     * Reusing the existing window and just creating a new GL context on it
     * is both correct and simpler here.
     */
    zxh_renderer_destroy(state);
    if (zxh_CRT_shader_create(&state->video.crt, state->video.window,
                              SCREEN_WIDTH, SCREEN_HEIGHT))
    {
      state->video.crt_enabled = 1;
      return 1;
    }

    fprintf(stderr, "CRT shader unavailable; using the plain renderer\n");
#else
    /* Only the very first claim (nothing live yet at startup) gets a window
     * the compositor has never presented anything on; every later re-enable
     * needs a fresh window to avoid the stuck-frame bug above. Must be
     * checked before zxh_renderer_destroy clears video.renderer below.
     */
    int window_already_used = state->video.renderer != NULL ||
                              state->video.crt.crt != NULL;

    zxh_renderer_destroy(state);

    if (window_already_used && !zxh_video_recreate_window(state))
      goto crt_unavailable;

    if (zxh_CRT_shader_create(&state->video.crt, state->video.window,
                              SCREEN_WIDTH, SCREEN_HEIGHT))
    {
      state->video.crt_enabled = 1;
      return 1;
    }

crt_unavailable:
    fprintf(stderr, "CRT shader unavailable; using the plain renderer\n");
#endif
  }
  else if (state->video.crt_enabled)
  {
    zxh_CRT_shader_destroy(&state->video.crt, state->video.window);
    memset(&state->video.crt, 0, sizeof(state->video.crt));
  }

  state->video.crt_enabled = 0;

  if (!zxh_renderer_create(state))
    return 0; // neither backend is up; the caller has to give up

  return !enable; // the plain renderer is only a success if it was asked for
}

/* ----------------------------------------------------------------------- */

#define ZXH_OSD_DURATION_MS (2500)
#define ZXH_OSD_GLYPH_W        (8)
#define ZXH_OSD_GLYPH_H        (7)
#define ZXH_OSD_GLYPH_GAP      (1)

/* Show text as a bottom-of-screen CRT-style OSD for ZXH_OSD_DURATION_MS,
 * mirroring a real CRT TV's on-screen settings display (see reference
 * screenshot in the feature request).
 */
static void zxh_osd_show(zxh_sdl_state_t *state, const char *text)
{
  SDL_strlcpy(state->video.osd_text, text, sizeof(state->video.osd_text));
  state->video.osd_shown_at_ms = SDL_GetTicks();
}

static const u8 *zxh_osd_glyph(const zxh_sdl_state_t *state, char c)
{
  if (c == ' ' || state->ops->osd_glyph == NULL)
    return NULL;

  return state->ops->osd_glyph(c);
}

#define ZXH_OSD_MARGIN (4) // game pixels

/* Visits every lit pixel of the active OSD text, in unscaled game-pixel
 * space (256x192, top-down), calling plot(ctx, gx + dx, gy + dy) for each
 * one. (dx, dy) lets the caller re-run the same glyph layout offset by a
 * pixel, to stamp a black outline before the text itself -- see
 * zxh_osd_draw_outlined. Glyph height is doubled (each font[] row plotted
 * twice) to read clearly at the game's native resolution. Returns 0 (and
 * calls plot for nothing) once ZXH_OSD_DURATION_MS has elapsed since
 * zxh_osd_show, or if there is no active text -- shared by both the
 * plain-renderer and CRT-shader draw paths so the glyph layout only lives
 * in one place.
 */
static int zxh_osd_visit(zxh_sdl_state_t *state, int dx, int dy,
                         void (*plot)(void *ctx, int gx, int gy),
                         void *ctx)
{
  int         col;
  const char *p;

  if (state->video.osd_shown_at_ms == 0)
    return 0;

  if (SDL_GetTicks() - state->video.osd_shown_at_ms > ZXH_OSD_DURATION_MS)
  {
    state->video.osd_shown_at_ms = 0;
    return 0;
  }

  col = 0;
  for (p = state->video.osd_text; *p != '\0'; p++, col++)
  {
    const u8 *glyph;
    int       row, bit;

    glyph = zxh_osd_glyph(state, *p);
    if (glyph == NULL)
      continue;

    for (row = 0; row < ZXH_OSD_GLYPH_H; row++)
    {
      u8 rowbits = glyph[row];

      for (bit = 0; bit < ZXH_OSD_GLYPH_W; bit++)
      {
        int gx, gy;

        if (!(rowbits & (0x80 >> bit)))
          continue;

        gx = ZXH_OSD_MARGIN + col * (ZXH_OSD_GLYPH_W + ZXH_OSD_GLYPH_GAP) + bit;
        gy = SCREEN_HEIGHT - ZXH_OSD_MARGIN - ZXH_OSD_GLYPH_H * 2 + row * 2;
        plot(ctx, gx + dx, gy + dy);
        plot(ctx, gx + dx, gy + 1 + dy);
      }
    }
  }

  return 1;
}

/* Runs zxh_osd_visit for the 8 neighbouring pixel offsets then dead centre,
 * so callers get a solid outline drawn in one colour followed by the text
 * itself in another -- e.g. a black outline behind bright green text so it
 * reads over any background. outline_plot/text_plot may be the same
 * function if the caller distinguishes by colour some other way (they
 * don't have to be separate contexts either).
 */
static int zxh_osd_draw_outlined(zxh_sdl_state_t *state,
                                 void (*outline_plot)(void *ctx, int gx, int gy),
                                 void             *outline_ctx,
                                 void (*text_plot)(void *ctx, int gx, int gy),
                                 void             *text_ctx)
{
  static const int offsets[8][2] =
  {
    {-1,-1}, {0,-1}, {1,-1},
    {-1, 0},         {1, 0},
    {-1, 1}, {0, 1}, {1, 1}
  };
  int i;
  int active;

  active = 0;
  for (i = 0; i < 8; i++)
    active |= zxh_osd_visit(state, offsets[i][0], offsets[i][1],
                            outline_plot, outline_ctx);

  active |= zxh_osd_visit(state, 0, 0, text_plot, text_ctx);

  return active;
}

typedef struct
{
  zxh_sdl_state_t *state;
  int               x, y; // game view origin, window pixel space
} zxh_osd_render_ctx_t;

static void zxh_osd_plot_renderer_outline(void *vctx, int gx, int gy)
{
  zxh_osd_render_ctx_t *ctx   = vctx;
  int                    scale = ctx->state->video.scale;
  SDL_FRect              px;

  px.x = (float) (ctx->x + gx * scale);
  px.y = (float) (ctx->y + gy * scale);
  px.w = (float) scale;
  px.h = (float) scale;
  SDL_SetRenderDrawColor(ctx->state->video.renderer, 0x00, 0x00, 0x00, 0xFF);
  SDL_RenderFillRect(ctx->state->video.renderer, &px);
}

static void zxh_osd_plot_renderer_fill(void *vctx, int gx, int gy)
{
  zxh_osd_render_ctx_t *ctx   = vctx;
  int                    scale = ctx->state->video.scale;
  SDL_FRect              px;

  px.x = (float) (ctx->x + gx * scale);
  px.y = (float) (ctx->y + gy * scale);
  px.w = (float) scale;
  px.h = (float) scale;
  SDL_SetRenderDrawColor(ctx->state->video.renderer, 0x40, 0xFF, 0x40, 0xFF);
  SDL_RenderFillRect(ctx->state->video.renderer, &px);
}

/* Draws the active OSD text (if any) bottom-left of the game picture, in
 * window pixel space -- same (x, y, scale) convention as
 * zxh_draw_dirty_overlay. A black outline is stamped first so the green
 * text reads over any background. No-op if there is no renderer (CRT
 * shader mode composites the OSD itself, via zxh_render_osd_mask below).
 */
static void zxh_draw_osd(zxh_sdl_state_t *state, int x, int y)
{
  zxh_osd_render_ctx_t ctx;

  if (state->video.renderer == NULL)
    return;

  ctx.state = state;
  ctx.x     = x;
  ctx.y     = y;

  zxh_osd_draw_outlined(state, zxh_osd_plot_renderer_outline, &ctx,
                                zxh_osd_plot_renderer_fill, &ctx);
}

static void zxh_osd_plot_mask_outline(void *vctx, int gx, int gy)
{
  u8 *mask = vctx;

  if (gx >= 0 && gx < SCREEN_WIDTH && gy >= 0 && gy < SCREEN_HEIGHT && mask[gy * SCREEN_WIDTH + gx] == 0)
    mask[gy * SCREEN_WIDTH + gx] = 2;
}

static void zxh_osd_plot_mask_fill(void *vctx, int gx, int gy)
{
  u8 *mask = vctx;

  if (gx >= 0 && gx < SCREEN_WIDTH && gy >= 0 && gy < SCREEN_HEIGHT)
    mask[gy * SCREEN_WIDTH + gx] = 1;
}

/* Fills state->video.osd_mask, a SCREEN_WIDTH*SCREEN_HEIGHT byte mask (0 = empty,
 * 1 = lit OSD pixel, 2 = black outline pixel, top-down, same layout as the
 * converted screen buffer zxh_CRT_shader_render uploads) for the CRT
 * shader path to composite directly into its staging buffer, since that
 * path has no SDL_Renderer to draw rects with. The mask lives in
 * zxh_sdl_state_t rather than as a per-call stack buffer -- at
 * SCREEN_WIDTH*SCREEN_HEIGHT bytes (~49KB) that's substantial to put on the
 * stack of zxh_sdl_main_loop every frame. Outline is stamped before the
 * fill so fill pixels always win where the two overlap. Returns 0 if the
 * OSD isn't active (mask left untouched).
 */
static int zxh_render_osd_mask(zxh_sdl_state_t *state)
{
  u8 *mask = state->video.osd_mask;

  memset(mask, 0, sizeof(state->video.osd_mask));

  return zxh_osd_draw_outlined(state, zxh_osd_plot_mask_outline, mask,
                                      zxh_osd_plot_mask_fill, mask);
}

/* Builds a SCREEN_WIDTH*SCREEN_HEIGHT ABGR8888 view of the game's debug
 * buffer (see zxgame_ops_t::debug_buffer) for the F12 toggle.
 *
 * ponytail: debug view only, tears against the game thread; good enough to
 * eyeball the buffer.
 */
static const uint32_t *zxh_build_backbuffer_pixels(zxh_sdl_state_t *state)
{
  static uint32_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];
  const u8       *buf;
  int             y, x;

  buf = state->ops->debug_buffer(state->game);
  for (y = 0; y < SCREEN_HEIGHT; y++)
    for (x = 0; x < SCREEN_WIDTH; x++)
      pixels[y * SCREEN_WIDTH + x] =
          (buf[y * (SCREEN_WIDTH / 8) + x / 8] & (0x80 >> (x % 8))) ?
          0xFF000000u : 0xFFFFFFFFu;

  return pixels;
}

static void zxh_action_toggle_pause(zxh_sdl_state_t *state)
{
  ZXH_FLAG_ASSIGN(state, ZXH_FLAG_PAUSED, !ZXH_FLAG_TEST(state, ZXH_FLAG_PAUSED));
  zxh_update_window_title(state);
  zxh_osd_show(state, ZXH_FLAG_TEST(state, ZXH_FLAG_PAUSED) ? "PAUSED" : "RESUMED");
}

static void zxh_action_toggle_mute(zxh_sdl_state_t *state)
{
  state->audio.muted = !state->audio.muted;
  zxh_update_window_title(state);
  zxh_osd_show(state, state->audio.muted ? "MUTE ON" : "MUTE OFF");
}

static void zxh_action_toggle_dirty_overlay(zxh_sdl_state_t *state)
{
  state->video.show_dirty_overlay = !state->video.show_dirty_overlay;
  zxh_osd_show(state, state->video.show_dirty_overlay ? "DIRTY RECTS ON" : "DIRTY RECTS OFF");
}

static void zxh_action_toggle_crt(zxh_sdl_state_t *state)
{
  zxh_set_crt_enabled(state, !state->video.crt_enabled);
  zxh_osd_show(state, state->video.crt_enabled ? "CRT ON" : "CRT OFF");
}

static void zxh_action_toggle_monochrome(zxh_sdl_state_t *state)
{
  state->video.monochrome = !state->video.monochrome;
  zxspectrum_set_monochrome(state->zx, state->video.monochrome);
  zxh_osd_show(state, state->video.monochrome ? "MONOCHROME ON" : "MONOCHROME OFF");
}

static void zxh_action_toggle_mellow(zxh_sdl_state_t *state)
{
  state->video.mellow = !state->video.mellow;
  zxspectrum_set_mellow(state->zx, state->video.mellow);
  zxh_osd_show(state, state->video.mellow ? "MELLOW ON" : "MELLOW OFF");
}

static void zxh_action_toggle_mouse_steering(zxh_sdl_state_t *state)
{
  state->mouse_steering = !state->mouse_steering;
  state->mouse_wheel     = 0;

  zxkempston_assign(&state->kempston, zxjoystick_LEFT,  0);
  zxkempston_assign(&state->kempston, zxjoystick_RIGHT, 0);
  zxkempston_assign(&state->kempston, zxjoystick_FIRE,  0); // GEAR; see zxh_sdl_mouse_button
  zxkeyset_assign(&state->keys, zxkey_SPACE, 0);            // BOOST default key; see zxh_sdl_mouse_button

  zxh_osd_show(state, state->mouse_steering ? "MOUSE STEERING ON" : "MOUSE STEERING OFF");
}

static void zxh_action_toggle_backbuffer(zxh_sdl_state_t *state)
{
  if (state->ops->debug_buffer == NULL)
    return;

  state->video.show_backbuffer = !state->video.show_backbuffer;
  zxh_osd_show(state, state->video.show_backbuffer ? "BACKBUFFER ON" : "BACKBUFFER OFF");
}

static void zxh_action_toggle_test_mode(zxh_sdl_state_t *state)
{
  int on;

  if (state->ops->toggle_debug == NULL)
    return;

  on = state->ops->toggle_debug(state->game);
  zxh_osd_show(state, on ? "TEST MODE ON" : "TEST MODE OFF");
}

static void zxh_action_randomise_screen(zxh_sdl_state_t *state)
{
  zxspectrum_randomise_screen(state->zx);
  zxh_osd_show(state, "GLITCH");
}

static void zxh_action_toggle_ay_channel(zxh_sdl_state_t *state, int ch)
{
  char buf[32];

  state->audio.ay_channel_muted[ch] = !state->audio.ay_channel_muted[ch];
  SDL_LockMutex(state->audio.lock);
  zxsound_enable_ay_channel(state->audio.sound, ch, !state->audio.ay_channel_muted[ch]);
  SDL_UnlockMutex(state->audio.lock);
  SDL_snprintf(buf, sizeof(buf), "AY CHANNEL %c %s", "ABC"[ch],
               state->audio.ay_channel_muted[ch] ? "OFF" : "ON");
  zxh_osd_show(state, buf);
}

static void zxh_action_toggle_speaker_mute(zxh_sdl_state_t *state)
{
  state->audio.speaker_muted = !state->audio.speaker_muted;
  SDL_LockMutex(state->audio.lock);
  zxsound_mute_speaker(state->audio.sound, state->audio.speaker_muted);
  SDL_UnlockMutex(state->audio.lock);
  zxh_osd_show(state, state->audio.speaker_muted ? "SPEAKER MUTE ON" : "SPEAKER MUTE OFF");
}

static void zxh_action_toggle_fullscreen(zxh_sdl_state_t *state)
{
  state->video.fullscreen = !state->video.fullscreen;
  SDL_SetWindowFullscreen(state->video.window, state->video.fullscreen);
  zxh_osd_show(state, state->video.fullscreen ? "FULLSCREEN ON" : "FULLSCREEN OFF");
}

static void zxh_action_adjust_scale(zxh_sdl_state_t *state, SDL_Keycode sym)
{
  int scale;

  scale = CLAMP(state->video.scale + (sym == SDLK_MINUS ? -1 : 1),
                SCALE_MIN, SCALE_MAX);

  if (scale != state->video.scale)
  {
    state->video.scale = scale;
    SDL_SetWindowSize(state->video.window,
                      zxh_window_width(scale),
                      zxh_window_height(scale));
  }
}

static void zxh_action_adjust_speed(zxh_sdl_state_t *state, SDL_Keycode sym, int shift, int ctrl)
{
  int  speed;
  char buf[32];
  int  step;

  if (sym == SDLK_BACKSLASH)
    speed = SPEED_DEFAULT;
  else if (ctrl)
    speed = sym == SDLK_LEFTBRACKET ? SPEED_MIN : SPEED_MAX;
  else
  {
    step  = shift ? 1 : SPEED_STEP;
    speed = CLAMP(state->speed + (sym == SDLK_LEFTBRACKET ? -step : step),
                  SPEED_MIN, SPEED_MAX);
  }

  state->speed = speed;
  SDL_LockMutex(state->audio.lock);
  zxsound_set_speed(state->audio.sound, speed);
  SDL_UnlockMutex(state->audio.lock);
  zxh_update_window_title(state);

  SDL_snprintf(buf, sizeof(buf), "SPEED %d PC", speed);
  zxh_osd_show(state, buf);
}

static void zxh_action_adjust_volume(zxh_sdl_state_t *state, SDL_Keycode sym)
{
  int  volume;
  char buf[32];

  volume = CLAMP(state->audio.volume + (sym == SDLK_F5 ? -VOLUME_STEP : VOLUME_STEP),
                 VOLUME_MIN, VOLUME_MAX);

  state->audio.volume = volume;
  zxh_update_window_title(state);

  SDL_snprintf(buf, sizeof(buf), "VOLUME %d PC", volume);
  zxh_osd_show(state, buf);
}

/* Shift-TAB steps backwards. Adding COUNT keeps the modulus operand
 * positive, since C's % on a negative left operand would give -1 rather
 * than the last index.
 */
static void zxh_action_crt_param_next(zxh_sdl_state_t *state, int shift)
{
  const zxh_crt_param_desc_t *desc;
  int                         step;
  char                        buf[32];

  step = shift ? ZXH_CRT_PARAM_COUNT - 1 : 1;

  state->video.crt_param_index = (state->video.crt_param_index + step) % ZXH_CRT_PARAM_COUNT;
  desc = &zxh_crt_param_descs[state->video.crt_param_index];
  SDL_snprintf(buf, sizeof(buf), "%s %.2f", desc->name,
              *zxh_crt_param_field(&state->video.crt_params, desc));
  zxh_osd_show(state, buf);
}

static void zxh_action_crt_param_adjust(zxh_sdl_state_t *state, SDL_Keycode sym)
{
  const zxh_crt_param_desc_t *desc;
  float                      *field;
  char                        buf[32];

  desc   = &zxh_crt_param_descs[state->video.crt_param_index];
  field  = zxh_crt_param_field(&state->video.crt_params, desc);
  *field = CLAMP(*field + (sym == SDLK_PAGEDOWN ? -desc->step : desc->step),
                 desc->min, desc->max);
  SDL_snprintf(buf, sizeof(buf), "%s %.2f", desc->name, *field);
  zxh_osd_show(state, buf);
}

static void zxh_action_crt_params_tuned(zxh_sdl_state_t *state)
{
  state->video.crt_params = crt_tuned_params;
  zxh_osd_show(state, "CRT EFFECT TUNED");
}

static void zxh_action_crt_params_default(zxh_sdl_state_t *state)
{
  state->video.crt_params = crt_default_params;
  zxh_osd_show(state, "CRT EFFECT RESET");
}

static void zxh_action_joystick_or_key(zxh_sdl_state_t *state,
                                       const SDL_KeyboardEvent *k,
                                       zxjoystick_t             j)
{
  int down;

  down = k->down;

  if (j != zxjoystick_UNKNOWN)
  {
    zxkempston_assign(&state->kempston, j, down);
  }
  else
  {
    zxkeyset_t *keys;

    keys = &state->keys;
    if (down)
      zxkeyset_setchar(keys, k->key);
    else
      zxkeyset_clearchar(keys, k->key);
  }
}

static void zxh_sdl_key_pressed(zxh_sdl_state_t         *state,
                                const SDL_KeyboardEvent *k)
{
  SDL_Keycode  sym;
  zxjoystick_t j;

  sym = k->key;

  switch (sym)
  {
  case SDLK_F1:
    if (k->down && !k->repeat)
      zxh_action_toggle_pause(state);
    return;

  case SDLK_F2:
    if (k->down && !k->repeat)
      zxh_action_toggle_mute(state);
    return;

  case SDLK_F3:
    if (k->down && !k->repeat)
      zxh_action_toggle_dirty_overlay(state);
    return;

  case SDLK_F4:
    if (k->down && !k->repeat)
      zxh_action_toggle_crt(state);
    return;

  case SDLK_F7:
  case SDLK_F8:
  case SDLK_F9:
    if (k->down && !k->repeat)
      zxh_action_toggle_ay_channel(state, sym - SDLK_F7);
    return;

  case SDLK_F10:
    if (k->down && !k->repeat)
      zxh_action_toggle_speaker_mute(state);
    return;

  case SDLK_F11:
    if (k->down && !k->repeat)
      zxh_action_toggle_fullscreen(state);
    return;

  case SDLK_F12:
    if (k->down && !k->repeat)
      zxh_action_toggle_backbuffer(state);
    return;

  case SDLK_M:
    if (k->mod & SDL_KMOD_CTRL)
    {
      if (k->down && !k->repeat)
        zxh_action_toggle_monochrome(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_N:
    if (k->mod & SDL_KMOD_CTRL)
    {
      if (k->down && !k->repeat)
        zxh_action_toggle_mellow(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_G:
    if (k->mod & SDL_KMOD_CTRL)
    {
      if (k->down && !k->repeat)
        zxh_action_toggle_mouse_steering(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_Y:
    if (k->mod & SDL_KMOD_CTRL)
    {
      if (k->down && !k->repeat)
        zxh_action_randomise_screen(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_X:
    if (k->mod & SDL_KMOD_CTRL)
    {
      if (k->down && !k->repeat)
        zxh_action_toggle_test_mode(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_MINUS:
  case SDLK_EQUALS:
    if (k->down && !k->repeat)
      zxh_action_adjust_scale(state, sym);
    return;

  case SDLK_LEFTBRACKET:
  case SDLK_RIGHTBRACKET:
  case SDLK_BACKSLASH:
    if (k->down)
      zxh_action_adjust_speed(state, sym, k->mod & SDL_KMOD_SHIFT, k->mod & SDL_KMOD_CTRL);
    return;

  case SDLK_F5:
  case SDLK_F6:
    if (k->down && !k->repeat)
      zxh_action_adjust_volume(state, sym);
    return;

  /* The shader tuning keys only belong to the shader. With the plain
   * renderer up they fall through to the game like any other key.
   */
  case SDLK_TAB:
    if (state->video.crt_enabled)
    {
      if (k->down && !k->repeat)
        zxh_action_crt_param_next(state, k->mod & SDL_KMOD_SHIFT);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_PAGEUP:
  case SDLK_PAGEDOWN:
    if (state->video.crt_enabled)
    {
      if (k->down)
        zxh_action_crt_param_adjust(state, sym);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_R:
    if (state->video.crt_enabled && (k->mod & SDL_KMOD_CTRL))
    {
      if (k->down && !k->repeat)
        zxh_action_crt_params_default(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_T:
    if (state->video.crt_enabled && (k->mod & SDL_KMOD_CTRL))
    {
      if (k->down && !k->repeat)
        zxh_action_crt_params_tuned(state);
      return;
    }
    j = zxjoystick_UNKNOWN;
    break;

  case SDLK_LEFT:  j = zxjoystick_LEFT;    break;
  case SDLK_RIGHT: j = zxjoystick_RIGHT;   break;
  case SDLK_UP:    j = zxjoystick_UP;      break;
  case SDLK_DOWN:  j = zxjoystick_DOWN;    break;
  case '.':        j = zxjoystick_FIRE;    break;
  default:         j = zxjoystick_UNKNOWN; break;
  }

  zxh_action_joystick_or_key(state, k, j);
}

static void zxh_sdl_mouse_motion(zxh_sdl_state_t           *state,
                                 const SDL_MouseMotionEvent *m)
{
  if (!state->mouse_steering)
    return;

  state->mouse_wheel = CLAMP(state->mouse_wheel + (int) m->xrel,
                             -MOUSE_WHEEL_RANGE, MOUSE_WHEEL_RANGE);

  zxkempston_assign(&state->kempston, zxjoystick_LEFT,
                    state->mouse_wheel < -MOUSE_WHEEL_DEADZONE);
  zxkempston_assign(&state->kempston, zxjoystick_RIGHT,
                    state->mouse_wheel > MOUSE_WHEEL_DEADZONE);
}

// Left click stands in for GEAR via the kempston FIRE bit, same joystick
// path as arrow-key steering -- both need the in-game "KEMPSTON JOYSTICK"
// control scheme selected (or auto-detected) to take effect. Right click is
// turbo boost via zxkey_SPACE, its default keyboard binding
// (Bank3.c default_control_keys[7]), which is scanned regardless of scheme.
static void zxh_sdl_mouse_button(zxh_sdl_state_t            *state,
                                 const SDL_MouseButtonEvent *b)
{
  if (!state->mouse_steering)
    return;

  switch (b->button)
  {
  case SDL_BUTTON_LEFT:
    zxkempston_assign(&state->kempston, zxjoystick_FIRE, b->down);
    break;

  case SDL_BUTTON_RIGHT:
    zxkeyset_assign(&state->keys, zxkey_SPACE, b->down);
    break;

  default:
    break;
  }
}

/* Outlines the screen regions zxh_draw_handler reported dirty since the last
 * frame, so refreshed areas are visible over the rendered texture. Rects are
 * in game pixel space (256x192, bottom-left origin); (x, y) is the top-left
 * of the game view within the window, already scaled.
 *
 * Called every frame whichever backend is up, because it also drains the
 * list the game thread keeps filling. Only the plain renderer can draw it;
 * under the CRT shader it just empties the list and returns.
 */
#define DIRTYOVERLAY_THICKNESS (8) // outline thickness in window pixels

/* Draws 'rect' as a filled-in outline DIRTYOVERLAY_THICKNESS pixels thick by
 * insetting and stroking it repeatedly (SDL_RenderRect has no line-width).
 */
static void zxh_render_thick_rect(SDL_Renderer *renderer, const SDL_FRect *rect)
{
  int i;

  for (i = 0; i < DIRTYOVERLAY_THICKNESS; i++)
  {
    SDL_FRect inset;

    inset.x = rect->x + i;
    inset.y = rect->y + i;
    inset.w = rect->w - i * 2;
    inset.h = rect->h - i * 2;
    if (inset.w <= 0 || inset.h <= 0)
      break;

    SDL_RenderRect(renderer, &inset);
  }
}

static void zxh_draw_dirty_overlay(zxh_sdl_state_t *state, int x, int y)
{
  zxbox_t rects[MAXDIRTYRECTS];
  int     count;
  int     full_screen;
  int     scale;
  int     i;

  SDL_LockMutex(state->video.dirty_mutex);
  count       = state->video.dirty_count;
  full_screen = state->video.dirty_full_screen;
  for (i = 0; i < count; i++)
    rects[i] = state->video.dirty_rects[i];
  state->video.dirty_count       = 0;
  state->video.dirty_full_screen = 0;
  SDL_UnlockMutex(state->video.dirty_mutex);

  if (!state->video.show_dirty_overlay || state->video.renderer == NULL)
    return;

  if (count > 0 || full_screen)
    fprintf(stderr, "[dirty] count=%d full_screen=%d\n", count, full_screen); // TEMP debug

  scale = state->video.scale;

  if (full_screen)
  {
    SDL_FRect rect;

    rect.x = (float) x;
    rect.y = (float) y;
    rect.w = (float) (SCREEN_WIDTH  * scale);
    rect.h = (float) (SCREEN_HEIGHT * scale);

    SDL_SetRenderDrawColor(state->video.renderer, 0xFF, 0x00, 0x00, 0xFF); // red: full-screen refresh
    zxh_render_thick_rect(state->video.renderer, &rect);
  }

  SDL_SetRenderDrawColor(state->video.renderer, 0x00, 0xFF, 0x00, 0xFF); // green: partial refresh
  for (i = 0; i < count; i++)
  {
    const zxbox_t *box = &rects[i];
    SDL_FRect      rect;

    // box is bottom-left origin; flip to the window's top-down space.
    rect.x = (float) (x + box->x0 * scale);
    rect.y = (float) (y + (SCREEN_HEIGHT - box->y1) * scale);
    rect.w = (float) ((box->x1 - box->x0) * scale);
    rect.h = (float) ((box->y1 - box->y0) * scale);

    zxh_render_thick_rect(state->video.renderer, &rect);
  }
}

// Applies one SDL event to the instance that owns its window. Split out of
// zxh_sdl_main_loop because SDL_PollEvent drains one process-wide queue --
// with multiple instances, every event (regardless of which window it's for)
// must be dispatched to the right zxh_sdl_state_t rather than each instance
// polling (and starving its siblings of input).
static void zxh_handle_event(zxh_sdl_state_t *state, const SDL_Event *event)
{
  switch (event->type)
  {
  case SDL_EVENT_QUIT:
    ZXH_FLAG_SET(state, ZXH_FLAG_QUIT);
    break;

  case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    ZXH_FLAG_SET(state, ZXH_FLAG_QUIT);
    break;

  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
    zxh_sdl_key_pressed(state, &event->key);
    break;

  case SDL_EVENT_TEXT_EDITING:
  case SDL_EVENT_TEXT_INPUT:
    break;

  case SDL_EVENT_MOUSE_MOTION:
    zxh_sdl_mouse_motion(state, &event->motion);
    break;

  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
    zxh_sdl_mouse_button(state, &event->button);
    break;

  case SDL_EVENT_MOUSE_WHEEL:
    break;

  default:
    break;
  }
}

// Dispatches every pending SDL event (single process-wide queue) to the
// instance whose window it belongs to. SDL_EVENT_QUIT has no window ID (it's
// not tied to any one window), so it is broadcast to all instances.
static void zxh_dispatch_events(zxh_sdl_state_t *instances, int count)
{
  SDL_Event  event;
  int        n;
  int        have_windowID;
  Uint32     windowID;

  while (SDL_PollEvent(&event))
  {
    if (event.type == SDL_EVENT_QUIT)
    {
      for (n = 0; n < count; n++)
        zxh_handle_event(&instances[n], &event);
      continue;
    }

    // SDL_Event is a tagged union -- only read the member that matches
    // event.type, rather than assuming every event carries a window.window
    // field at the same offset.
    have_windowID = 1;
    switch (event.type)
    {
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      windowID = event.window.windowID;
      break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      windowID = event.key.windowID;
      break;

    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
      windowID = event.text.windowID;
      break;

    case SDL_EVENT_MOUSE_MOTION:
      windowID = event.motion.windowID;
      break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      windowID = event.button.windowID;
      break;

    case SDL_EVENT_MOUSE_WHEEL:
      windowID = event.wheel.windowID;
      break;

    default:
      have_windowID = 0;
      windowID      = 0;
      break;
    }

    if (!have_windowID)
      continue;

    for (n = 0; n < count; n++)
      if (SDL_GetWindowID(instances[n].video.window) == windowID)
      {
        zxh_handle_event(&instances[n], &event);
        break;
      }
  }
}

// type: em_arg_callback_func
static void zxh_sdl_main_loop(void *opaque)
{
  zxh_sdl_state_t *state = opaque;
  int              x, y, w, h; // destination rect: game view within the window
  int              ww, wh;     // actual window size (may exceed scale*game size in fullscreen)
  int              border;     // border colour 0..7: GRB bits
  int              r, g, b;    // border colour, non-bright, 0..255

  if (ZXH_FLAG_TEST(state, ZXH_FLAG_QUIT))
    return;

  // ponytail: ignores monochrome and mellow modes
  border = state->video.border;
  r      = (border & 2) ? 0xCD : 0x00;
  g      = (border & 4) ? 0xCD : 0x00;
  b      = (border & 1) ? 0xCD : 0x00;

  w = SCREEN_WIDTH  * state->video.scale;
  h = SCREEN_HEIGHT * state->video.scale;
  SDL_GetWindowSize(state->video.window, &ww, &wh);
  x = (ww - w) / 2; // centred; equals BORDER*scale in windowed mode, letterboxes in fullscreen
  y = (wh - h) / 2;

  if (state->video.crt_enabled)
  {
    int             osd_active;
    const uint32_t *override_pixels;

    osd_active      = zxh_render_osd_mask(state);
    override_pixels = state->video.show_backbuffer ?
                       zxh_build_backbuffer_pixels(state) : NULL;

    crt_set_border(state->video.crt.crt, r / 255.0f, g / 255.0f, b / 255.0f);
    zxh_CRT_shader_render(&state->video.crt, state->video.window, state->zx,
                          x, y, w, h, SCREEN_WIDTH, SCREEN_HEIGHT,
                          &state->video.crt_params,
                          osd_active ? state->video.osd_mask : NULL,
                          override_pixels);
    zxh_draw_dirty_overlay(state, x, y); // drains the list; draws nothing
  }
  else
  {
    /* Update the texture from the game's converted screen buffer. */
    const zx_frame_t *frame;
    SDL_FRect         dstrect;

    dstrect.x = (float) x;
    dstrect.y = (float) y;
    dstrect.w = (float) w;
    dstrect.h = (float) h;

    if (state->video.show_backbuffer)
    {
      const uint32_t *pixels = zxh_build_backbuffer_pixels(state);

      SDL_UpdateTexture(state->video.texture, NULL, pixels, SCREEN_WIDTH * (int) sizeof(uint32_t));
    }
    else
    {
      frame = zxspectrum_claim_screen(state->zx);
      SDL_UpdateTexture(state->video.texture, NULL, frame->pixels, frame->stride);
      zxspectrum_release_screen(state->zx);
    }

    /* Clear screen to the border colour */
    SDL_SetRenderDrawColor(state->video.renderer, r, g, b, 0xFF);
    SDL_RenderClear(state->video.renderer);

    /* Offset the image */
    // Note that this will inhibit image stretching.

    SDL_RenderTexture(state->video.renderer, state->video.texture, NULL, &dstrect);
    zxh_draw_dirty_overlay(state, x, y);
    zxh_draw_osd(state, x, y);
    SDL_RenderPresent(state->video.renderer);
  }
}

// Brings up one game instance: window, renderer, audio device, game thread.
// Instances are otherwise independent -- each owns its own zxh_sdl_state_t,
// so this exists purely to let two-up mode spin up N of them without
// duplicating the whole of main().
static int zxh_instance_create(zxh_sdl_state_t *state, const zxgame_ops_t *ops, int mode_128k, int index, int count, int quiet, int scale, int speed)
{
  zxconfig_t  zxconfig;
  SDL_Window *window;

  memset(state, 0, sizeof(*state));
  state->ops            = ops;
  state->launch.model   = mode_128k ? ZXGAME_MODEL_128K : ZXGAME_MODEL_48K;
  state->launch.load_file = NULL;
  state->instance       = index;
  state->instance_count = count;

  zxkeyset_clear(&state->keys);
  state->kempston = 0;
  state->flags    = 0;
  ZXH_FLAG_ASSIGN(state, ZXH_FLAG_MODE_128K, mode_128k);
  state->video.scale           = scale;
  state->speed                 = speed;
  state->audio.volume          = VOLUME_DEFAULT;
  state->video.crt_params      = crt_tuned_params;
  state->video.crt_param_index = 0;
  state->audio.muted           = quiet;

  window = SDL_CreateWindow(ops->name,
                            zxh_window_width(state->video.scale),
                            zxh_window_height(state->video.scale),
#if defined(__EMSCRIPTEN__)
                            /* The WebGL CRT backend renders via
                             * SDL_GL_CreateContext, which requires the
                             * window to have been created with this flag. */
                            SDL_WINDOW_OPENGL
#else
                            0
#endif
                            );
  if (window == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateWindow: %s\n", SDL_GetError());
    return 0;
  }

  // With more than one instance, tile windows in a grid instead of leaving
  // them all centred on top of each other; a single instance keeps SDL's
  // default centred placement. SDL_WINDOWPOS_UNDEFINED/CENTERED are magic
  // encoded values, not coordinates -- arithmetic on them corrupts the
  // encoding rather than offsetting the position, so real usable-desktop
  // bounds are read via SDL_GetDisplayUsableBounds and tiled from there.
  if (count > 1)
  {
    SDL_Rect bounds = {0};
    int      cols   = (int) SDL_ceil(SDL_sqrt((double) count));
    int      col    = index % cols;
    int      row    = index / cols;

    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds);
    SDL_SetWindowPosition(window,
                          bounds.x + col * zxh_window_width(state->video.scale),
                          bounds.y + row * zxh_window_height(state->video.scale));
  }

  state->video.window = window;

  zxh_update_window_title(state);

  /* The GPU texture is always SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM (R in the
   * lowest memory byte), which is what Screen.c's palette_abgr table packs
   * when ZX_PIXEL_ABGR8888 is selected - the format selects which
   * palette table to use, not which byte order it produces. See the
   * 0x00RRGGBB/0x00BBGGRR comments in Screen.c.
   *
   * Conv: this is fixed rather than matched to the renderer's preferred
   * format, as it was when the backend was chosen at compile time. The
   * backend can now change at any keypress but the conversion palette is
   * picked once, so both backends have to agree; the CRT one cannot bend, so
   * the plain renderer takes an ABGR8888 texture to suit (see
   * zxh_renderer_create).
   */
  zxconfig.width        = SCREEN_WIDTH / 8;
  zxconfig.height       = SCREEN_HEIGHT / 8;
  zxconfig.opaque       = state;
  zxconfig.draw         = &zxh_draw_handler;
  zxconfig.stamp        = &zxh_stamp_handler;
  zxconfig.sleep        = &zxh_sleep_handler;
  zxconfig.key          = &zxh_key_handler;
  zxconfig.border       = &zxh_border_handler;
  zxconfig.speaker      = &zxh_speaker_handler;
  zxconfig.ay_out       = &zxh_ay_out_handler;
  zxconfig.pixel_format = ZX_PIXEL_ABGR8888; /* R in lower byte */

  state->zx = zxspectrum_create(&zxconfig);
  if (state->zx == NULL)
    return 0;

  state->audio.lock = SDL_CreateMutex();
  if (state->audio.lock == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateMutex: %s\n", SDL_GetError());
    return 0;
  }

  state->video.dirty_mutex = SDL_CreateMutex();
  if (state->video.dirty_mutex == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateMutex: %s\n", SDL_GetError());
    return 0;
  }

  {
    SDL_AudioSpec desired = {0};
    SDL_AudioSpec device;

    desired.freq     = AY_SAMPLE_RATE;
    desired.format   = SDL_AUDIO_S16;
    desired.channels = 2;

    state->audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                   &desired,
                                                   &zxh_audio_callback,
                                                   state);
    if (state->audio.stream == NULL)
    {
      fprintf(stderr, "Error: SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
      return 0;
    }

    /* Match the stream's source rate to whatever the device actually opened
     * at, so SDL has no resampling to do in the callback. The device rate is
     * not negotiable on every backend -- Emscripten forces it to
     * audioContext.sampleRate -- and the resampler would run on the same
     * main browser thread as the ScriptProcessorNode callback. The AY is
     * then emulated at that rate directly.
     */
    state->audio.sample_rate = AY_SAMPLE_RATE;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(state->audio.stream),
                                 &device, NULL) &&
        device.freq >= 8000 && device.freq <= 192000)
    {
      state->audio.sample_rate = device.freq;
    }

    if (state->audio.sample_rate != desired.freq)
    {
      desired.freq = state->audio.sample_rate;
      SDL_SetAudioStreamFormat(state->audio.stream, &desired, NULL);
    }
  }

  /* Mono output, duplicated into both channels: zxsound_t puts the AY in mono
   * mode. The chip defaults to ABC stereo separation (left = A+B, right =
   * B+C), which sounds right-heavy or left-heavy depending on which channels a
   * given tune favours; we don't know whether a game's music assumes ABC, ACB,
   * or no separation at all, so mono sidesteps the question.
   */
  state->audio.sound = zxsound_create(state->audio.sample_rate,
                                      mode_128k ? Z80_CLOCK_128K : Z80_CLOCK_48K,
                                      SOUND_CUSHION_MS,
                                      2);
  if (state->audio.sound == NULL)
    return 0;
  zxsound_set_speed(state->audio.sound, state->speed);

  SDL_ResumeAudioStreamDevice(state->audio.stream);

  /* Bring up the starting backend. A CRT request that cannot be met falls
   * back to the plain renderer rather than failing to start; only losing
   * both is fatal.
   */
  zxh_set_crt_enabled(state, ZXH_CRT_SHADER);
  if (!state->video.crt_enabled && state->video.renderer == NULL)
    return 0;

  // The cassette loading screen is drawn and held by the game itself now
  // (entry_common's show_loading_screen, called from ops->run on the game
  // thread below) -- on real hardware the tape loader blits it before the
  // BASIC loader ever runs the machine code, so the game reproducing that
  // ordering internally is truer than the host faking it beforehand.
  state->game = ops->create(state->zx);
  if (state->game == NULL)
    return 0;

  state->game_thread = SDL_CreateThread(zxh_game_thread, "game", state);
  if (state->game_thread == NULL)
  {
    fprintf(stderr, "Error: SDL_CreateThread: %s\n", SDL_GetError());
    return 0;
  }

  return 1;
}

// Tears down one instance created by zxh_instance_create. Mirrors main()'s
// old single-instance shutdown sequence.
static void zxh_instance_destroy(zxh_sdl_state_t *state)
{
  // Stop the audio device before anything else. zxh_stop only signals the
  // game thread, which longjmps out mid-frame and leaves the AY holding
  // whatever tone the tune was playing; without this the callback keeps
  // sounding that tone for the whole shutdown wait below. Pause before
  // clear, or the callback refills between the two.
  SDL_PauseAudioStreamDevice(state->audio.stream);
  SDL_ClearAudioStream(state->audio.stream);

  // game/game_thread are NULL if zxh_instance_create failed before reaching
  // zxh_create/SDL_CreateThread.
  if (state->game != NULL)
  {
    if (state->ops->stop != NULL)
      state->ops->stop(state->game);
    SDL_WaitThread(state->game_thread, NULL);
    state->ops->destroy(state->game);
  }

  zxspectrum_destroy(state->zx);

  SDL_DestroyAudioStream(state->audio.stream);
  zxsound_destroy(state->audio.sound);
  SDL_DestroyMutex(state->audio.lock);
  SDL_DestroyMutex(state->video.dirty_mutex);

  if (state->video.crt_enabled)
    zxh_CRT_shader_destroy(&state->video.crt, state->video.window);
  else
    zxh_renderer_destroy(state);
  SDL_DestroyWindow(state->video.window);
}

// Parses a decimal integer strictly: rejects empty strings, non-numeric
// input and trailing garbage, rather than letting atoi silently coerce them
// to 0. Returns 1 on success (with *out set), 0 on failure.
static int zxh_parse_int(const char *text, int *out)
{
  char *end;
  long  value;

  if (text[0] == '\0')
    return 0;

  value = strtol(text, &end, 10);
  if (*end != '\0')
    return 0;

  *out = (int) value;
  return 1;
}

// Destroys every instance in [0, created) that hasn't already torn itself
// down, then frees the array and shuts SDL down. Shared by every exit path
// out of main() so a failure partway through startup doesn't leak windows,
// audio devices or the SDL subsystem.
static void zxh_shutdown_all(zxh_sdl_state_t *instances, int created)
{
  int n;

  for (n = 0; n < created; n++)
  {
    if (!ZXH_FLAG_TEST(&instances[n], ZXH_FLAG_CLOSED))
      zxh_instance_destroy(&instances[n]);
  }

  free(instances);

  SDL_Quit();
}

#ifdef __EMSCRIPTEN__
/* Conv: native builds pump the game loop with a blocking do/while on the
 * calling thread. That's fine on desktop, where the OS keeps compositing the
 * window regardless. Under Emscripten the browser only ever repaints between
 * yields back to its own event loop -- a thread that blocks in a loop never
 * yields, so the canvas stays black and the tab looks wedged.
 * emscripten_set_main_loop schedules each iteration via
 * requestAnimationFrame instead, which yields naturally.
 */
typedef struct
{
  zxh_sdl_state_t *instances;
  int               count;
} zxh_web_ctx_t;

static void zxh_web_main_loop(void *opaque)
{
  zxh_web_ctx_t *ctx;
  int             n, quit_count;

  ctx        = (zxh_web_ctx_t *) opaque;
  quit_count = 0;

  zxh_dispatch_events(ctx->instances, ctx->count);
  for (n = 0; n < ctx->count; n++)
  {
    if (!ZXH_FLAG_TEST(&ctx->instances[n], ZXH_FLAG_QUIT))
    {
      zxh_sdl_main_loop(&ctx->instances[n]);
    }
    else
    {
      if (!ZXH_FLAG_TEST(&ctx->instances[n], ZXH_FLAG_CLOSED))
      {
        zxh_instance_destroy(&ctx->instances[n]);
        ZXH_FLAG_SET(&ctx->instances[n], ZXH_FLAG_CLOSED);
      }
      quit_count++;
    }
  }

  if (quit_count >= ctx->count)
  {
    emscripten_cancel_main_loop();
    zxh_shutdown_all(ctx->instances, ctx->count);
    free(ctx);
  }
}

#endif

int zxgame_sdl3_main(int argc, char *argv[], const zxgame_ops_t *ops)
{
  zxh_sdl_state_t *instances;
  int              n, count, arg, mode_128k, quiet, scale, scale_random, speed, speed_random;
#ifndef __EMSCRIPTEN__
  int              quit_count;
#endif

  count        = 1;
  mode_128k    = (ops->default_model == ZXGAME_MODEL_128K);
  quiet        = 0;
  scale        = SCALE_DEFAULT;
  scale_random = 0;
  speed        = SPEED_DEFAULT;
  speed_random = 0;

  for (arg = 1; arg < argc; arg++)
  {
    if (strcmp(argv[arg], "-48k") == 0 && (ops->models & ZXGAME_MODEL_48K))
    {
      mode_128k = 0;
    }
    else if (strcmp(argv[arg], "-128k") == 0 && (ops->models & ZXGAME_MODEL_128K))
    {
      mode_128k = 1;
    }
    else if (strcmp(argv[arg], "-n") == 0 && arg + 1 < argc)
    {
      if (!zxh_parse_int(argv[++arg], &count))
      {
        fprintf(stderr, "Error: -n expects an integer, got '%s'\n", argv[arg]);
        return EXIT_FAILURE;
      }
    }
    else if (strcmp(argv[arg], "-quiet") == 0)
    {
      quiet = 1;
    }
    else if (strcmp(argv[arg], "-scale") == 0 && arg + 1 < argc)
    {
      arg++;
      if (strcmp(argv[arg], "random") == 0)
      {
        scale_random = 1;
      }
      else if (!zxh_parse_int(argv[arg], &scale))
      {
        fprintf(stderr, "Error: -scale expects an integer or 'random', got '%s'\n", argv[arg]);
        return EXIT_FAILURE;
      }
    }
    else if (strcmp(argv[arg], "-speed") == 0 && arg + 1 < argc)
    {
      arg++;
      if (strcmp(argv[arg], "random") == 0)
      {
        speed_random = 1;
      }
      else if (!zxh_parse_int(argv[arg], &speed))
      {
        fprintf(stderr, "Error: -speed expects an integer or 'random', got '%s'\n", argv[arg]);
        return EXIT_FAILURE;
      }
    }
    else
    {
      fprintf(stderr, "Usage: %s [-48k | -128k (where supported)] [-n count] [-quiet] [-scale factor|random] [-speed percent|random]\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (count < 1)
  {
    fprintf(stderr, "Error: -n count must be at least 1\n");
    return EXIT_FAILURE;
  }

  if (!scale_random && (scale < SCALE_MIN || scale > SCALE_MAX))
  {
    fprintf(stderr, "Error: -scale must be between %d and %d, or 'random'\n", SCALE_MIN, SCALE_MAX);
    return EXIT_FAILURE;
  }

  if (!speed_random && (speed < SPEED_MIN || speed > SPEED_MAX))
  {
    fprintf(stderr, "Error: -speed must be between %d and %d, or 'random'\n", SPEED_MIN, SPEED_MAX);
    return EXIT_FAILURE;
  }

  if (speed_random || scale_random)
    srand((unsigned) SDL_GetTicks());

#ifdef __APPLE__
  /* Conv: disable macOS press-and-hold accent popover so held keys repeat
   * instead of opening the accent picker.
   */
  CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"),
                            kCFBooleanFalse,
                            kCFPreferencesCurrentApplication);
  CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
#endif

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
  {
    fprintf(stderr, "Error: SDL_Init: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  instances = calloc((size_t) count, sizeof(*instances));
  if (instances == NULL)
  {
    fprintf(stderr, "Error: out of memory\n");
    SDL_Quit();
    return EXIT_FAILURE;
  }

  for (n = 0; n < count; n++)
  {
    int instance_speed = speed_random ? SPEED_DEFAULT + rand() % (SPEED_MAX - SPEED_DEFAULT + 1) : speed;
    int instance_scale = scale_random ? SCALE_MIN + rand() % (SCALE_MAX - SCALE_MIN + 1) : scale;

    if (!zxh_instance_create(&instances[n], ops, mode_128k, n, count, quiet, instance_scale, instance_speed))
    {
      fprintf(stderr, "Error: failed to start instance #%d\n", n + 1);
      zxh_shutdown_all(instances, n);
      return EXIT_FAILURE;
    }
  }

#ifdef __EMSCRIPTEN__
  // Conv: browser build hands the game loop to emscripten_set_main_loop
  // (see zxh_web_main_loop above) instead of blocking this thread -- a
  // blocking loop never yields to the browser's compositor, which would
  // leave the canvas black.
  {
    zxh_web_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
    {
      fprintf(stderr, "Error: out of memory\n");
      zxh_shutdown_all(instances, count);
      return EXIT_FAILURE;
    }
    ctx->instances = instances;
    ctx->count     = count;
    emscripten_set_main_loop_arg(zxh_web_main_loop, ctx, 0, 1);
  }

  return EXIT_SUCCESS;
#else
  // Two-up test harness: pumps every instance's event/game loop from a
  // single thread, same as the single-instance case did for one. Each
  // instance's game logic still runs on its own game thread (spawned in
  // zxh_instance_create); this loop only drives SDL events/rendering, which
  // is inherently single-threaded per process in SDL3.
  do
  {
    quit_count = 0;
    zxh_dispatch_events(instances, count);
    for (n = 0; n < count; n++)
    {
      if (!ZXH_FLAG_TEST(&instances[n], ZXH_FLAG_QUIT))
      {
        zxh_sdl_main_loop(&instances[n]);
      }
      else
      {
        // Tear this instance down (and close its window) the moment it
        // quits, rather than waiting for every other instance to catch up --
        // otherwise closing window #2 leaves it hanging on screen until #1
        // quits too.
        if (!ZXH_FLAG_TEST(&instances[n], ZXH_FLAG_CLOSED))
        {
          zxh_instance_destroy(&instances[n]);
          ZXH_FLAG_SET(&instances[n], ZXH_FLAG_CLOSED);
        }
        quit_count++;
      }
    }
  }
  while (quit_count < count);

  zxh_shutdown_all(instances, count);

  return EXIT_SUCCESS;
#endif
}
