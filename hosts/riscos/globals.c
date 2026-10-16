/* --------------------------------------------------------------------------
 *    Name: globals.c
 * Purpose: Global variables
 *  Author: David Thomas
 * ----------------------------------------------------------------------- */

#include <stdio.h>

#include "globals.h"

struct ZXGameGlobals GLOBALS;

const char *app_path(const char *suffix)
{
  static char buf[64];

  sprintf(buf, "%s%s", GLOBALS.app->appname, suffix);

  return buf;
}
