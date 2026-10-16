/* NullGameMac.m: smoke test for the Cocoa front-end.
 *
 * Builds the window in code instead of from xibs, so it bypasses
 * AppDelegate and ZXGameWindowController and exercises ZXGameView only.
 * Paints a test pattern, cycles the border colour and plays an AY tone.
 *
 * Usage: NullGameMacOS [-seconds N]
 */
#import <Cocoa/Cocoa.h>

#import "ZXGame/Game.h"
#import "ZXSpectrum/slopay-chip.h"
#import "ZXGameView.h"

int zxgame_macos_main(int argc, char *argv[], const zxgame_ops_t *ops);

static void *null_create(zxspectrum_t *zx) { return zx; }
static void null_destroy(void *game) { (void) game; }

static void null_run(void *game, const zxgame_launch_t *launch)
{
  zxspectrum_t *zx = game;
  unsigned      frame;
  int           i;

  (void) launch;

  for (i = 0; i < SCREEN_BITMAP_LENGTH; i++)
    zx->screen.pixels[i] = (i / SCREEN_BITMAP_ROWBYTES) & 1 ? 0x55 : 0xAA;
  for (i = 0; i < SCREEN_ATTRIBUTES_LENGTH; i++)
    zx->screen.attributes[i] = (i % 7) + 1;

  /* Channel A: tone at about 440Hz, full volume */
  zx->out(zx, port_AY_REGISTER, AY_REG_CHANNEL_A_FINE_PITCH);
  zx->out(zx, port_AY_DATA,     0xFC);
  zx->out(zx, port_AY_REGISTER, AY_REG_CHANNEL_A_COARSE_PITCH);
  zx->out(zx, port_AY_DATA,     0x00);
  zx->out(zx, port_AY_REGISTER, AY_REG_MIXER);
  zx->out(zx, port_AY_DATA,     AY_MIXER_ALL_OFF & ~AY_MIXER_NO_TONE_A);
  zx->out(zx, port_AY_REGISTER, AY_REG_CHANNEL_A_VOLUME);
  zx->out(zx, port_AY_DATA,     0x0F);

  for (frame = 0; !zxspectrum_quit_requested(zx); frame++)
  {
    zx->stamp(zx);
    zx->out(zx, 0xFE, (frame / 50) & 7);
    zx->draw(zx, NULL);
    zx->sleep(zx, 69888);
  }
}

static const zxgame_ops_t null_ops =
{
  "Null Game", ZXGAME_MODEL_48K, ZXGAME_MODEL_48K,
  null_create, null_destroy, null_run
};

@interface Helper : NSObject <NSApplicationDelegate, ZXGameViewDelegate>
@property (strong) ZXGameView *view;
@end

@implementation Helper

- (NSURL *)getStartupGame
{
  return nil;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
  return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
  [self.view stop];
}

@end

static Helper *helper;
static double  seconds;

static void launch(void)
{
  NSRect    rect = NSMakeRect(0, 0, 256 + 32, 192 + 32);
  NSWindow *window;
  ZXGameView *view;

  helper = [[Helper alloc] init];
  [NSApp setDelegate:helper];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  window = [[NSWindow alloc] initWithContentRect:rect
                                       styleMask:NSWindowStyleMaskTitled |
                                                 NSWindowStyleMaskClosable |
                                                 NSWindowStyleMaskResizable
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
  window.title = @"Null Game";
  window.releasedWhenClosed = NO;

  view = [[ZXGameView alloc] initWithFrame:rect device:nil];
  view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [view awakeFromNib];
  view.gameDelegate = helper;
  helper.view = view;

  window.contentView = view;
  [window center];
  [window makeKeyAndOrderFront:nil];
  [window makeFirstResponder:view];
  [NSApp activateIgnoringOtherApps:YES];

  [view start];

  if (seconds > 0)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

int main(int argc, char *argv[])
{
  int i;

  for (i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], "-seconds") == 0)
      seconds = atof(argv[i + 1]);

  dispatch_async(dispatch_get_main_queue(), ^{ launch(); });

  return zxgame_macos_main(argc, argv, &null_ops);
}
