/* Game.h
 *
 * The interface between a ZX Spectrum game port and the ZXGame front-ends.
 *
 * A game supplies one zxgame_ops_t. Every front-end (SDL, headless, macOS,
 * Windows, RISC OS) drives the game only through it, so a port needs no
 * platform code of its own.
 *
 * Copyright (c) David Thomas, 2013-2026. <dave@davespace.co.uk>
 */

#ifndef ZXGAME_GAME_H
#define ZXGAME_GAME_H

#include "ZXSpectrum/Spectrum.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Spectrum models a game may support. */
typedef enum zxgame_model
{
  ZXGAME_MODEL_48K  = 1 << 0,
  ZXGAME_MODEL_128K = 1 << 1
}
zxgame_model_t;

/** Options chosen by the front-end when it starts the game. */
typedef struct zxgame_launch
{
  /** Model to run as. One of the bits in zxgame_ops_t::models. */
  zxgame_model_t model;

  /** Saved game to load instead of showing the menu, or NULL. */
  const char *load_file;
}
zxgame_launch_t;

/** Per-game entry points. All pointers are optional unless noted. */
typedef struct zxgame_ops
{
  /** Human readable name, used for window titles. (Required) */
  const char *name;

  /** Bitwise OR of zxgame_model_t values supported. (Required) */
  unsigned int models;

  /** Model used when the front-end is not told otherwise. (Required) */
  zxgame_model_t default_model;

  /** Create a game instance bound to the given Spectrum. (Required) */
  void *(*create)(zxspectrum_t *zx);

  /** Destroy a game instance. (Required) */
  void (*destroy)(void *game);

  /**
   * Run the game until the front-end asks it to quit, i.e. until
   * zxspectrum_quit_requested() becomes non-zero. Blocks. Called on the game
   * thread by front-ends which have one.
   *
   * (Required unless 'step' is supplied. If both are supplied threaded
   * front-ends use 'run' and cooperative ones use 'step'.)
   */
  void (*run)(void *game, const zxgame_launch_t *launch);

  /**
   * Called from another thread to make 'run' return promptly, where the game
   * does not notice a quit request returned from sleep() quickly enough.
   */
  void (*stop)(void *game);

  /**
   * Cooperative alternative to 'run' for front-ends with no game thread.
   * Called repeatedly. Runs the game until it would next yield, then returns.
   * The first call performs start up, using 'launch'.
   *
   * \return Non-zero if the game has finished or been asked to quit.
   */
  int (*step)(void *game, const zxgame_launch_t *launch);

  /* Saved games. A front-end offers save/load UI only if all three are
   * supplied. Called on the game thread, only while the game is paused. */

  /** \return Zero on success. */
  int (*save)(void *game, const char *filename);

  /** \return Zero on success. On failure *error is a message to release with
   *          dispose_error. */
  int (*load)(void *game, const char *filename, char **error);

  void (*dispose_error)(char *error);

  /** Saved game filename extension without the dot, e.g. "tge". */
  const char *save_extension;

  /**
   * Scripted input for the headless front-end, to get through start-up menus.
   * Called for every key port read, with a running count of such reads.
   *
   * \return The 5 key bits to report for the port (0x1F: nothing pressed),
   *         or for port_KEMPSTON_JOYSTICK the active-high joystick bits.
   */
  int (*scripted_key)(uint16_t port, unsigned poll_count);

  /* Optional on-screen-display and debug aids (SDL front-end). */

  /**
   * Glyph for an on-screen-display character: seven rows of eight pixels,
   * most significant bit leftmost, or NULL for blank. If this is NULL the
   * front-end reports status in the window title only.
   */
  const uint8_t *(*osd_glyph)(char c);

  /**
   * Toggle a game-specific debug mode.
   *
   * \return Non-zero if now enabled.
   */
  int (*toggle_debug)(void *game);

  /**
   * Return a snapshot of an off-screen buffer for display: linear rows of
   * SCREEN_WIDTH x SCREEN_HEIGHT one-bit pixels, most significant bit
   * leftmost, a set bit being black. The pointer stays valid until the next
   * call. May be inconsistent if the game thread is writing it.
   */
  const uint8_t *(*debug_buffer)(void *game);

  /**
   * \return Non-zero while the game is showing a start-up menu, where saving
   *         makes no sense. If NULL the game is taken to have none.
   */
  int (*in_menu)(void *game);
}
zxgame_ops_t;

/* Front-end entry points. Each takes over the thread until the program ends
 * and returns the process exit code. A game's main() is typically just
 * `return zxgame_sdl3_main(argc, argv, &my_game_ops);`. */

int zxgame_sdl3_main(int argc, char *argv[], const zxgame_ops_t *ops);
int zxgame_headless_main(int argc, char *argv[], const zxgame_ops_t *ops);

/* macOS only: the Cocoa front-end. The app's xibs instantiate its classes. */
int zxgame_macos_main(int argc, char *argv[], const zxgame_ops_t *ops);

/* Windows only: the Win32 front-end. Call it from WinMain with the HINSTANCE
 * and nCmdShow. 'icon_resource' is an icon resource id in the app, or 0. */
int zxgame_windows_main(void *hinstance, int ncmdshow, const zxgame_ops_t *ops, int icon_resource);

/* RISC OS only: the Wimp front-end. The app bundle (Resources, !Run etc.)
 * and its build supply the templates and messages; see hosts/README.md. */
typedef struct zxgame_riscos_app
{
  /** Short name used for the <appname>Res: path and <appname>$ScreenMode
   *  variable, e.g. "GtEscape". */
  const char *appname;

  /** File type of saved games. */
  unsigned int filetype;

  /** Sprite shown in Filer windows' info (7 characters max). */
  const char *window_sprite;

  /** Default saved game leaf name. */
  const char *save_name;
}
zxgame_riscos_app_t;

int zxgame_riscos_main(int argc, char *argv[], const zxgame_ops_t *ops, const zxgame_riscos_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* ZXGAME_GAME_H */

/* vim: ts=8 sts=2 sw=2 et */
