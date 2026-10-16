/* ZXGameHost.h
 *
 * Private interface between the macOS front-end's main() and its views.
 */

#ifndef ZXGAME_MACOS_ZXGAMEHOST_H
#define ZXGAME_MACOS_ZXGAMEHOST_H

#import "ZXGame/Game.h"

/** The game chosen by zxgame_macos_main(). */
const zxgame_ops_t *zxgame_macos_ops(void);

#endif /* ZXGAME_MACOS_ZXGAMEHOST_H */
