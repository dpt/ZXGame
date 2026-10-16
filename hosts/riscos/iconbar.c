/* --------------------------------------------------------------------------
 *    Name: iconbar.c
 * Purpose: Icon bar icon
 *  Author: David Thomas
 * ----------------------------------------------------------------------- */

#include <stdio.h>

#include "oslib/types.h"
#include "oslib/wimp.h"

#include "appengine/base/errors.h"
#include "appengine/gadgets/iconbar.h"
#include "appengine/wimp/window.h"

#include "globals.h"
#include "iconbar.h"
#include "menunames.h"
#include "zxgame.h"

static void icon_clicked(const wimp_pointer *pointer, void *opaque)
{
  NOT_USED(opaque);

  if (pointer->buttons & wimp_CLICK_SELECT)
  {
    result_t  err;
    zxgame_t *zxgame;

    err = zxgame_create(&zxgame, NULL);
    if (err)
    {
      result_report(err);
      return;
    }

    zxgame_open(zxgame);
  }
}

static void menu_selected(const wimp_selection *selection, void *opaque)
{
  char cmd[80];

  NOT_USED(opaque);

  switch (selection->items[0])
  {
    case ICONBAR_HELP:
      sprintf(cmd, "Filer_Run %s", app_path("Res:!Help"));
      xos_cli(cmd);
      break;

    case ICONBAR_INSTRUCTIONS:
      sprintf(cmd, "Filer_Run %s", app_path("Res:Instruct"));
      xos_cli(cmd);
      break;

    case ICONBAR_QUIT:
      GLOBALS.flags |= Flag_Quit;
      break;
  }
}

result_t zxgame_icon_bar_init(void)
{
  result_t err;

  err = icon_bar_init();
  if (err)
    return err;

  icon_bar_set_handlers(icon_clicked, menu_selected, NULL, NULL);

  return result_OK;
}

void zxgame_icon_bar_fin(void)
{
  icon_bar_fin();
}

// vim: ts=8 sts=2 sw=2 et
