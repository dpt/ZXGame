/* --------------------------------------------------------------------------
 *    Name: globals.h
 * Purpose: Global variables
 *  Author: David Thomas
 * ----------------------------------------------------------------------- */

#ifndef GLOBALS_H
#define GLOBALS_H

#include "oslib/wimp.h"

#include "appengine/wimp/dialogue.h"

#include "ZXGame/Game.h"

#include "zxgame.h"

/* ----------------------------------------------------------------------- */

#define APPFILETYPE (GLOBALS.app->filetype)
#define OPS         (GLOBALS.ops)

/* Build "<appname><suffix>" in a static buffer, e.g. "GtEscapeRes:Sprites". */
const char *app_path(const char *suffix);

/* ----------------------------------------------------------------------- */

enum
{
  Flag_Quit                     = 1 << 0,
  Flag_HaveWideColourTrans      = 1 << 1,
  Flag_HaveSharedSoundBuffer    = 1 << 2
};

typedef unsigned int Flags;

extern struct ZXGameGlobals
{
  const zxgame_ops_t         *ops;
  const zxgame_riscos_app_t  *app;

  Flags            flags;
  wimp_t           task_handle;
  wimp_version_no  wimp_version;

  wimp_w           zxgame_w; // template game window
  wimp_menu       *zxgame_m;

  zxgame_t        *current_zxgame;
}
GLOBALS;

/* ----------------------------------------------------------------------- */

#endif /* GLOBALS_H */

// vim: ts=8 sts=2 sw=2 et
