//
//  ZXGameView.m
//  The Great Escape
//
//  Created by David Thomas on 11/10/2014.
//  Copyright (c) 2014-2020 David Thomas. All rights reserved.
//

#import <ctype.h>
#import <sys/time.h>

#import <pthread/pthread.h>

#import <Foundation/Foundation.h>

#import <Cocoa/Cocoa.h>

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#import "ZXSpectrum/Spectrum.h"
#import "ZXSpectrum/Keyboard.h"
#import "ZXSpectrum/Kempston.h"

#import "ZXGame/Game.h"
#import "ZXGameHost.h"

#import "ZXGameWindowController.h"

#import "ZXAudio.h"

#import "ZXGameView.h"

// -----------------------------------------------------------------------------

// Configuration
//
#define GAMEWIDTH       (256)   // pixels
#define GAMEHEIGHT      (192)   // pixels
#define GAMEBORDER      (16)    // pixels

#define MAXSTAMPS       (4)     // max depth of timestamps stack
#define SPEEDQ          (20)    // smallest unit of speed (percent)
#define NORMSPEED       (100)   // normal speed (percent)
#define MAXSPEED        (99999) // fastest possible game (percent)

// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------

// Draws the frame texture into the viewport as a four vertex strip.
//
static NSString *const shaderSource =
  @"#include <metal_stdlib>\n"
  @"using namespace metal;\n"
  @"struct Out { float4 pos [[position]]; float2 uv; };\n"
  @"vertex Out vert(uint vid [[vertex_id]])\n"
  @"{\n"
  @"  float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);\n"
  @"  Out o;\n"
  @"  o.pos = float4(p, 0.0, 1.0);\n"
  @"  o.uv  = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
  @"  return o;\n"
  @"}\n"
  @"fragment float4 frag(Out in [[stage_in]],\n"
  @"                     texture2d<float> tex [[texture(0)]],\n"
  @"                     constant uint &mono [[buffer(0)]])\n"
  @"{\n"
  @"  constexpr sampler s(filter::nearest);\n"
  @"  float3 c = tex.sample(s, in.uv).rgb;\n"
  @"  if (mono)\n"
  @"    c = float3(dot(c, float3(0.299, 0.587, 0.114)));\n"
  @"  return float4(c, 1.0);\n"
  @"}\n";

// -----------------------------------------------------------------------------

#pragma mark - UIView

@interface ZXGameView()
{
  zxspectrum_t   *zx;
  const zxgame_ops_t *ops;
  void           *game;

  pthread_t       thread;

  id<MTLCommandQueue>        queue;
  id<MTLRenderPipelineState> pipeline;
  id<MTLTexture>             texture;

  MTLViewport     viewport;
  CGFloat         scale;
  int             borderSize;
  BOOL            snap;
  BOOL            monochromatic;

  // -- Start of variables accessed from game thread must be @synchronized

  BOOL            quit;
  BOOL            paused;

  NSURL          *startupGameURL;
  NSURL          *saveGameURL;
  BOOL            gameStarted;
  BOOL            oldPaused;

  int             speed;    // percent

  struct timeval  stamps[MAXSTAMPS];

  zxkeyset_t      keys;
  zxkempston_t    kempston;

  double          backgroundRed, backgroundGreen, backgroundBlue;

  zxaudio_t      *audio;

  // -- End of variables accessed from game thread must be @synchronized
}

@end

// -----------------------------------------------------------------------------

@implementation ZXGameView

// -----------------------------------------------------------------------------

#pragma mark - Standard methods

- (BOOL)setupMetal
{
  id<MTLDevice>         device;
  id<MTLLibrary>        library;
  MTLRenderPipelineDescriptor *desc;
  MTLTextureDescriptor *texDesc;
  NSError              *error;

  device = MTLCreateSystemDefaultDevice();
  if (device == nil)
    return NO;

  library = [device newLibraryWithSource:shaderSource options:nil error:&error];
  if (library == nil)
  {
    NSLog(@"ZXGameView: shader: %@", error);
    return NO;
  }

  desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction                  = [library newFunctionWithName:@"vert"];
  desc.fragmentFunction                = [library newFunctionWithName:@"frag"];
  desc.colorAttachments[0].pixelFormat = self.colorPixelFormat;

  pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
  if (pipeline == nil)
  {
    NSLog(@"ZXGameView: pipeline: %@", error);
    return NO;
  }

  // ZX_PIXEL_ABGR8888 is R,G,B,A in memory on little endian
  texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:GAMEWIDTH
                                                              height:GAMEHEIGHT
                                                           mipmapped:NO];
  texture = [device newTextureWithDescriptor:texDesc];
  queue   = [device newCommandQueue];

  self.device = device;

  // Draw only when asked to, from drawRect:
  self.enableSetNeedsDisplay = YES;
  self.paused                = YES;

  return texture != nil && queue != nil;
}

- (void)awakeFromNib
{
  const zxconfig_t zxconfig =
  {
    GAMEWIDTH / 8, GAMEHEIGHT / 8, // screen dimensions in UDGs
    (__bridge void *)(self),
    &draw_handler,
    &stamp_handler,
    &sleep_handler,
    &key_handler,
    &border_handler,
    &speaker_handler,
    &ay_handler,
    ZX_PIXEL_ABGR8888 // MTLPixelFormatRGBA8Unorm on little endian
  };

  ops             = zxgame_macos_ops();
  zx              = NULL;
  game            = NULL;

  thread          = NULL;

  borderSize      = GAMEBORDER;
  snap            = YES;
  monochromatic   = NO;

  quit            = NO;
  paused          = NO;

  startupGameURL  = nil;
  saveGameURL     = nil;
  gameStarted     = NO;
  oldPaused       = NO;

  speed           = NORMSPEED;

  memset(stamps, 0, sizeof(stamps));

  zxkeyset_clear(&keys);
  kempston        = 0;

  backgroundRed = backgroundGreen = backgroundBlue = 0.0;

  scale           = 1.0;


  zx = zxspectrum_create(&zxconfig);
  if (zx == NULL)
    goto failure;

  game = ops->create(zx);
  if (game == NULL)
    goto failure;

  if (![self setupMetal])
    goto failure;

  audio = zxaudio_create();

  return;


failure:
  if (game)
    ops->destroy(game);
  zxspectrum_destroy(zx);
}

// -----------------------------------------------------------------------------

- (void)start
{
  assert(self.gameDelegate);

  startupGameURL = [self.gameDelegate getStartupGame];

  pthread_create(&thread,
                 NULL, // pthread_attr_t
                 game_thread,
                 (__bridge void *)(self));
}

- (void)stop
{
  // Signal to the game thread to quit
  @synchronized(self)
  {
    if (quit)
      return; // already stopped
    quit = YES;
    paused = NO;
  }

  // Games which don't act on the quit result from sleep() need this to leave
  // their run loop, else the join below never returns.
  if (ops->stop)
    ops->stop(game);

  // Wait for it to yield
  // Is this is a bad idea to wait like this on the UI thread?
  pthread_join(thread, NULL);

  if (game)
    ops->destroy(game);
  zxspectrum_destroy(zx);

  zxaudio_destroy(audio);
  audio = NULL;
}

// -----------------------------------------------------------------------------


- (IBAction)saveDocumentAs:(id)sender
{
  if (!ops->save)
    return;

  @synchronized(self)
  {
    // Immediately pause the game when a save is requested.
    oldPaused = paused;
    paused = YES;

    NSSavePanel *savePanel = [NSSavePanel savePanel];
    savePanel.allowedFileTypes = @[@(ops->save_extension ? ops->save_extension : "sav")];
    savePanel.allowsOtherFileTypes = YES;
    savePanel.canCreateDirectories = YES;
    savePanel.delegate = self;
    savePanel.nameFieldStringValue = @"EscapeAttempt";

    [savePanel beginWithCompletionHandler:^(NSModalResponse result) {
      if (result == NSModalResponseOK)
      {
        NSURL *url = savePanel.URL;
        if (url)
          // The URL flags to cause a save when next able.
          saveGameURL = [url copy];
      }
      else
      {
        paused = oldPaused;
      }
    }];
  }
}


// -----------------------------------------------------------------------------

// Simulate a black and white TV
- (IBAction)toggleMonochromatic:(id)sender
{
  monochromatic = !monochromatic;
  [self setNeedsDisplay:YES];
}

// -----------------------------------------------------------------------------

#pragma mark - UI

- (BOOL)acceptsFirstResponder
{
  return YES;
}

- (BOOL)mouseDownCanMoveWindow
{
  return YES;
}

- (BOOL)validateUserInterfaceItem:(id <NSValidatedUserInterfaceItem>)anItem
{
  NSMenuItem *menuItem;

  if ([anItem action] == @selector(toggleSnap:))
  {
    menuItem = (NSMenuItem *) anItem;
    // Ensure that it's a menu item
    if ([menuItem respondsToSelector:@selector(setState:)])
      [menuItem setState:snap ? NSControlStateValueOn : NSControlStateValueOff];
  }
  else if ([anItem action] == @selector(toggleMonochromatic:))
  {
    menuItem = (NSMenuItem *) anItem;
    // Ensure that it's a menu item
    if ([menuItem respondsToSelector:@selector(setState:)])
      [menuItem setState:monochromatic ? NSControlStateValueOn : NSControlStateValueOff];
  }
  else if ([anItem action] == @selector(saveDocumentAs:))
  {
    if (!ops->save || !self->gameStarted)
      return NO;
  }

  return YES;
}

// -----------------------------------------------------------------------------

#pragma mark - Key handling

- (void)keyUpOrDown:(NSEvent *)event isDown:(BOOL)down
{
  NSEventModifierFlags  modifierFlags;
  NSEventModifierFlags  modifiersToReject;
  NSString             *chars;
  unichar               u;
  zxjoystick_t          j;

  // Ignore key repeat events. We're only interested in state changes so we can
  // set/clear bits in the zxkeyset bitfield. Also, if we don't ignore repeats
  // it will result in the self->mutex being taken too often which will starve
  // the main game thread.
  if (event.isARepeat)
    return;

  modifierFlags = [event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
  modifiersToReject = NSEventModifierFlagControl |
                      NSEventModifierFlagOption  |
                      NSEventModifierFlagCommand;
  if (modifierFlags & modifiersToReject)
    return;

  chars = [event characters];
  if ([chars length] == 0)
    return;

  u = [chars characterAtIndex:0];
  switch (u)
  {
    case NSUpArrowFunctionKey:    j = zxjoystick_UP;      break;
    case NSDownArrowFunctionKey:  j = zxjoystick_DOWN;    break;
    case NSLeftArrowFunctionKey:  j = zxjoystick_LEFT;    break;
    case NSRightArrowFunctionKey: j = zxjoystick_RIGHT;   break;
    case '.':                     j = zxjoystick_FIRE;    break;
    default:                      j = zxjoystick_UNKNOWN; break;
  }

  @synchronized(self)
  {
    if (j != zxjoystick_UNKNOWN)
    {
      zxkempston_assign(&kempston, j, down);
    }
    else
    {
      // zxkeyset_set/clearchar converts generic keypresses for us
      if (down)
        zxkeyset_setchar(&keys, u);
      else
        zxkeyset_clearchar(&keys, u);
    }
  }
}

- (void)keyUp:(NSEvent *)event
{
  [self keyUpOrDown:event isDown:NO];
}

- (void)keyDown:(NSEvent *)event
{
  [self keyUpOrDown:event isDown:YES];
}

- (void)flagsChanged:(NSEvent *)event
{
  // Unlike keyDown and keyUp, flagsChanged is a single event delivered when
  // any one of the modifier key states change, down or up.

  NSEventModifierFlags modifierFlags = [event modifierFlags];
  bool shift = (modifierFlags & NSEventModifierFlagShift)  != 0;
  bool alt   = (modifierFlags & NSEventModifierFlagOption) != 0;

  // For reference:
  //
  // bool control = (modifierFlags & NSControlKeyMask) != 0;
  // bool command = (modifierFlags & NSCommandKeyMask) != 0;

  @synchronized(self)
  {
    zxkeyset_assign(&keys, zxkey_CAPS_SHIFT,   shift);
    zxkeyset_assign(&keys, zxkey_SYMBOL_SHIFT, alt);
  }
}

// -----------------------------------------------------------------------------

#pragma mark - Action handlers

- (IBAction)setSpeed:(id)sender
{
  NSInteger tag;

  tag = [sender tag];

  @synchronized(self)
  {
    if (tag == 0) // pause/unpause
    {
      paused = !paused;
      return;
    }

    // any other action results in unpausing
    paused = NO;

    switch (tag)
    {
      default:
      case 100: // normal speed
        speed = NORMSPEED;
        break;

      case -1: // maximum speed
        speed = MAXSPEED;
        break;

      case 1: // increase speed
        speed += SPEEDQ;
        break;

      case 2: // decrease speed
        speed -= SPEEDQ;
        break;
    }

    if (speed < SPEEDQ)
      speed = SPEEDQ;
    else if (speed > MAXSPEED)
      speed = MAXSPEED;

    zxaudio_set_speed(audio, speed);
  }
}

- (IBAction)toggleSnap:(id)sender
{
  snap = !snap;

  // (un)tick the menu entry
  [sender setState:snap ? NSControlStateValueOn : NSControlStateValueOff];

  [self setNeedsDisplay:YES];
}

- (IBAction)toggleSound:(id)sender
{
  zxaudio_toggle(audio);
}

// -----------------------------------------------------------------------------

- (void)setupDrawing
{
  CGSize  gameSize;
  CGSize  viewSize;
  CGFloat gameWidthsPerView, gameHeightsPerView, gamesPerView;
  int     reducedBorder;
  double  drawWidth, drawHeight;

  gameSize.width  = zx->screen.width  * 8;
  gameSize.height = zx->screen.height * 8;

  // The drawable is in pixels, so retina screens are already accounted for
  viewSize = self.drawableSize;

  // How many 1:1 game windows fit comfortably into the view?
  // Try to fit while reducing the border if the view is very small
  reducedBorder = borderSize;
  do
  {
    gameWidthsPerView  = (viewSize.width  - reducedBorder * 2) / gameSize.width;
    gameHeightsPerView = (viewSize.height - reducedBorder * 2) / gameSize.height;
    gamesPerView = MIN(gameWidthsPerView, gameHeightsPerView);
    reducedBorder--;
  }
  while (reducedBorder >= 0 && gamesPerView < 1.0);

  // Snap the game scale to whole units
  if (gamesPerView > 1.0 && snap)
  {
    const CGFloat snapSteps = 1.0; // set to 2.0 for scales of 1.0 / 1.5 / 2.0 etc.
    gamesPerView = floor(gamesPerView * snapSteps) / snapSteps;
  }

  drawWidth  = floor(gameSize.width  * gamesPerView);
  drawHeight = floor(gameSize.height * gamesPerView);
  scale = gamesPerView;

  // Centre the game within the view
  viewport.originX = floor((viewSize.width  - drawWidth)  / 2);
  viewport.originY = floor((viewSize.height - drawHeight) / 2);
  viewport.width   = drawWidth;
  viewport.height  = drawHeight;
  viewport.znear   = 0.0;
  viewport.zfar    = 1.0;
}

- (void)drawRect:(NSRect)dirtyRect
{
  const zx_frame_t        *frame;
  double                   r, g, b;
  uint32_t                 mono;
  MTLRenderPassDescriptor *pass;
  id<CAMetalDrawable>      drawable;
  id<MTLCommandBuffer>     commands;
  id<MTLRenderCommandEncoder> encoder;

  (void) dirtyRect;

  if (pipeline == nil || self.drawableSize.width <= 0 || self.drawableSize.height <= 0)
    return;

  [self setupDrawing];
  if (scale <= 0.0)
    return;

  r = backgroundRed;
  g = backgroundGreen;
  b = backgroundBlue;
  mono = monochromatic;
  if (mono)
    r = g = b = 0.299 * r + 0.587 * g + 0.114 * b;

  // Clear the background every frame or you'll see junk in the border.
  self.clearColor = MTLClearColorMake(r, g, b, 1.0);

  pass     = self.currentRenderPassDescriptor;
  drawable = self.currentDrawable;
  if (pass == nil || drawable == nil)
    return;

  frame = zxspectrum_claim_screen(zx);
  [texture replaceRegion:MTLRegionMake2D(0, 0, GAMEWIDTH, GAMEHEIGHT)
             mipmapLevel:0
               withBytes:frame->pixels
             bytesPerRow:GAMEWIDTH * 4];
  zxspectrum_release_screen(zx);

  commands = [queue commandBuffer];
  encoder  = [commands renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:pipeline];
  [encoder setViewport:viewport];
  [encoder setFragmentTexture:texture atIndex:0];
  [encoder setFragmentBytes:&mono length:sizeof(mono) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  [encoder endEncoding];
  [commands presentDrawable:drawable];
  [commands commit];
}

// -----------------------------------------------------------------------------

#pragma mark - Game thread


static void modalSheetAlert(const ZXGameView *view, NSString *message)
{
  dispatch_async(dispatch_get_main_queue(), ^(void) {
    NSAlert *alert;

    alert = [[NSAlert alloc] init];
    [alert setMessageText:message];
    [alert addButtonWithTitle:@"OK"];
    [alert setAlertStyle:NSAlertStyleCritical];
    [alert beginSheetModalForWindow:view.window completionHandler:nil];
  });
}

static int saveIfRequested(const ZXGameView *view)
{
  if (view->saveGameURL)
  {
    int rc;

    rc = view->ops->save(view->game, [view->saveGameURL fileSystemRepresentation]);
    view->saveGameURL = nil;
    if (rc)
    {
      modalSheetAlert(view, @"The game failed to save");
      return 1;
    }

    // Restore previous pause state
    view->paused = view->oldPaused;
  }

  return 0;
}


static void *game_thread(void *arg)
{
  ZXGameView     *view = (__bridge id) arg;
  zxgame_launch_t launch;

  launch.model     = view->ops->default_model;
  launch.load_file = view->startupGameURL ?
                     [view->startupGameURL fileSystemRepresentation] : NULL;

  // The game's own loop runs until sleep() reports a quit. Games with a
  // start-up menu bypass it when load_file is set.
  view->ops->run(view->game, &launch);

  return NULL;
}

// -----------------------------------------------------------------------------

#pragma mark - Game thread callbacks

static void draw_handler(const zxbox_t *dirty,
                         void          *opaque)
{
  ZXGameView *view = (__bridge id) opaque;

  dispatch_async(dispatch_get_main_queue(), ^{
    // Odd: This refreshes the whole window no matter what size of rect is specified
    [view setNeedsDisplayInRect:NSMakeRect(0, 0, 0, 0)];
  });
}

static void stamp_handler(void *opaque)
{
  ZXGameView *view = (__bridge id) opaque;

  // The Spectrum facade keeps the stack depth; index our timestamps by it
  int depth = zxspectrum_stamp_depth(view->zx);

  assert(depth > 0 && depth <= MAXSTAMPS);
  if (depth <= 0 || depth > MAXSTAMPS)
    return;
  gettimeofday(&view->stamps[depth - 1], NULL);
}

static int sleep_handler(int durationTStates, void *opaque)
{
  ZXGameView *view = (__bridge id) opaque;
  BOOL paused;
  int  depth;

  @synchronized(view)
  {
    // The segment ending is now at index 'depth' (even if we're paused)
    depth = zxspectrum_stamp_depth(view->zx);
    assert(depth >= 0 && depth < MAXSTAMPS);
    if (depth < 0 || depth >= MAXSTAMPS)
      return view->quit;

    // Quit straight away if signalled
    if (view->quit)
      return TRUE;

    paused = view->paused;
  }

  if (paused)
  {
    // If paused, sit in this loop, checking twice per second for unpausing
    for (;;)
    {
      // Saves are handled when the game is paused
      (void) saveIfRequested(view);

      @synchronized(view)
      {
        paused = view->paused;
      }

      if (!paused)
        break;

      usleep(500000); // 0.5s
    }
  }
  else
  {
    // A Spectrum 48K has 69,888 T-states per frame and its Z80 runs at
    // 3.5MHz (~50Hz) for a total of 3,500,000 T-states per second.
    const double          tstatesPerSec = 3.5e6;

    struct timeval        now;
    double                duration; // seconds
    const struct timeval *then;
    struct timeval        delta;
    double                consumed; // seconds

    gettimeofday(&now, NULL); // get time now before anything else

    @synchronized(view)
    {
      // 'duration' tells us how long the operation should take since the previous mark call.
      // Turn T-state duration into seconds
      duration = durationTStates / tstatesPerSec;
      // Adjust the game speed
      duration = duration * 100 / view->speed;

      then = &view->stamps[depth];
    }

    delta.tv_sec  = now.tv_sec  - then->tv_sec;
    delta.tv_usec = now.tv_usec - then->tv_usec;

    consumed = delta.tv_sec + delta.tv_usec / 1e6;
    if (consumed < duration)
    {
      double     delay; // seconds
      useconds_t udelay;

      // We didn't take enough time - sleep for the remainder of our duration
      delay = duration - consumed;
      udelay = delay * 1e6;
      usleep(udelay);
    }
  }

  return FALSE;
}

static int key_handler(uint16_t port, void *opaque)
{
  ZXGameView *view = (__bridge id) opaque;
  int         k;

  @synchronized(view)
  {
    if (port == port_KEMPSTON_JOYSTICK)
      k = view->kempston;
    else
      k = zxkeyset_for_port(port, &view->keys);
  }

  return k;
}

static void border_handler(int colour, void *opaque)
{
  ZXGameView *view = (__bridge id) opaque;
  double      r, g, b;

  switch (colour)
  {
    case 0: r = 0; g = 0; b = 0; break;
    case 1: r = 0; g = 0; b = 1; break;
    case 2: r = 1; g = 0; b = 0; break;
    case 3: r = 1; g = 0; b = 1; break;
    case 4: r = 0; g = 1; b = 0; break;
    case 5: r = 0; g = 1; b = 1; break;
    case 6: r = 1; g = 1; b = 0; break;
    case 7: r = 1; g = 1; b = 1; break;
    default: return;
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    view->backgroundRed   = r;
    view->backgroundGreen = g;
    view->backgroundBlue  = b;
  });
}

static void ay_handler(uint16_t port, uint8_t byte, zxclock_t tstates, void *opaque)
{
  ZXGameView *view = (__bridge id) opaque;

  (void) tstates;

  zxaudio_ay(view->audio, port, byte);
}

static void speaker_handler(int on_off, zxclock_t tstates, void *opaque)
{
  ZXGameView  *view = (__bridge id) opaque;

  (void) tstates;

  zxaudio_speaker(view->audio, on_off);
}

// -----------------------------------------------------------------------------

#pragma mark - Queries

- (void)getDefaultViewSize:(CGSize *)size border:(int *)border
{
  assert(zx);

  size->width  = zx->screen.width  * 8;
  size->height = zx->screen.height * 8;
  *border      = borderSize;
}

- (void)getSuggestedViewSize:(CGSize *)size border:(int *)border forSize:(CGSize)maxSizePoints direction:(int)direction
{
  static const CGFloat scales[] = { 0.125, 0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 };
  const int            nscales  = (int) (sizeof(scales) / sizeof(scales[0]));

  int     game_w_px,game_h_px;
  CGFloat minscale;
  int     i;
  int     w_pt,h_pt;
  CGFloat scaleFactor;

  assert(zx);

  direction = MIN(MAX(direction, -1), 1); // clamp

  game_w_px = zx->screen.width  * 8;
  game_h_px = zx->screen.height * 8;

  minscale = MIN(maxSizePoints.width / game_w_px, maxSizePoints.height / game_h_px);
  for (i = 0; i < nscales; i++)
    if (scales[i] > minscale)
    {
      i--;
      break;
    }
  i = MIN(MAX(i + direction, 0), nscales - 1); // clamp
  scaleFactor = scales[i];

  w_pt = game_w_px * scaleFactor;
  h_pt = game_h_px * scaleFactor;

  size->width  = w_pt;
  size->height = h_pt;
  *border = borderSize; // doesn't scale
}

// -----------------------------------------------------------------------------

@end

// vim: ts=8 sts=2 sw=2 et
