/* MacMain.m
 *
 * Entry point for the Cocoa front-end.
 *
 * A game's app supplies its own xibs, Info.plist and icon; the xibs
 * instantiate the ZXGame* classes in this directory.
 */

#import <Cocoa/Cocoa.h>

#import "ZXGame/Game.h"
#import "ZXGameHost.h"

static const zxgame_ops_t *current_ops;

const zxgame_ops_t *zxgame_macos_ops(void)
{
  return current_ops;
}

int zxgame_macos_main(int argc, char *argv[], const zxgame_ops_t *ops)
{
  current_ops = ops;

  return NSApplicationMain(argc, (const char **) argv);
}
