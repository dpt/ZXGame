// main.cpp
//
// Windows front-end for ZXGame
//
// Copyright (c) David Thomas, 2016-2022. <dave@davespace.co.uk>
//

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>

#include <windows.h>

#include "ZXSpectrum/Spectrum.h"
#include "ZXSpectrum/Keyboard.h"
#include "ZXSpectrum/Kempston.h"

#include "ZXGame/Game.h"

///////////////////////////////////////////////////////////////////////////////

// Configuration
//
#define GAMEWIDTH       (256)   // pixels
#define GAMEHEIGHT      (192)   // pixels
#define GAMEBORDER      (16)    // pixels

#define MAXSTAMPS       (4)     // max depth of timestamps stack
#define SPEEDQ          (20)    // smallest unit of speed (percent)
#define NORMSPEED       (100)   // normal speed (percent)
#define MAXSPEED        (99999) // fastest possible game (percent)

///////////////////////////////////////////////////////////////////////////////

static const char szGameWindowClassName[] = "ZXGameWindowsApp";

// The game being run and the icon resource to use (0 for none). There is one
// game per process.
static const zxgame_ops_t *g_ops;
static int                 g_icon;

///////////////////////////////////////////////////////////////////////////////

typedef struct gamewin
{
  HINSTANCE       instance;
  HWND            window;
  BITMAPINFO      bitmapinfo;

  zxspectrum_t   *zx;
  void           *game;

  HANDLE          thread;
  DWORD           threadId;

  zxkeyset_t      keys;
  zxkempston_t    kempston;

  int             speed;  // percent
  BOOL            paused;

  bool            quit;

  ULONGLONG       stamps[MAXSTAMPS];
}
gamewin_t;

///////////////////////////////////////////////////////////////////////////////

static void draw_handler(const zxbox_t *dirty,
                         void          *opaque)
{
  gamewin_t *gamewin = (gamewin_t *) opaque;
  RECT       rect;

  rect.left   = 0;
  rect.top    = 0;
  rect.right  = 65536; // FIXME: Find a symbol for whole window dimension.
  rect.bottom = 65536;

  // kick the window
  InvalidateRect(gamewin->window, &rect, FALSE);
}

static void stamp_handler(void *opaque)
{
  gamewin_t *gamewin = (gamewin_t *) opaque;

  // The Spectrum facade keeps the stack depth; index our timestamps by it
  int depth = zxspectrum_stamp_depth(gamewin->zx);

  assert(depth > 0 && depth <= MAXSTAMPS);
  if (depth <= 0 || depth > MAXSTAMPS)
    return;
  gamewin->stamps[depth - 1] = GetTickCount64();
}

static int sleep_handler(int durationTStates, void *opaque)
{
  gamewin_t *gamewin = (gamewin_t *) opaque;

  // The segment ending is now at index 'depth' (even if we're paused)
  int depth = zxspectrum_stamp_depth(gamewin->zx);

  assert(depth >= 0 && depth < MAXSTAMPS);
  if (depth < 0 || depth >= MAXSTAMPS)
    return gamewin->quit;

  // Quit straight away if signalled
  if (gamewin->quit)
    return TRUE;

  if (gamewin->paused)
  {
    // Check twice per second for unpausing
    // FIXME: Slow spinwait without any synchronisation
    while (gamewin->paused)
      Sleep(500); /* 0.5s */
  }
  else
  {
    // A Spectrum 48K has 69,888 T-states per frame and its Z80 runs at
    // 3.5MHz (~50Hz) for a total of 3,500,000 T-states per second.
    const double tstatesPerSec = 3.5e6;

    ULONGLONG now;
    double    duration; /* seconds */
    ULONGLONG then;
    ULONGLONG delta;
    double    consumed; /* seconds */

    now = GetTickCount64(); // get time now before anything else

    // 'duration' tells us how long the operation should take since the previous mark call.
    // Turn T-state duration into seconds
    duration = durationTStates / tstatesPerSec;
    // Adjust the game speed
    duration = duration * NORMSPEED / gamewin->speed;

    then = gamewin->stamps[depth];

    delta = now - then;

    consumed = delta / 1e6;
    if (consumed < duration)
    {
      double delay; /* seconds */
      DWORD  udelay; /* milliseconds */

      // We didn't take enough time - sleep for the remainder of our duration
      delay = duration - consumed;
      udelay = (DWORD)(delay * 1e3);
      Sleep(udelay);
    }
  }

  return FALSE;
}

static int key_handler(uint16_t port, void *opaque)
{
  gamewin_t *gamewin = (gamewin_t *) opaque;
  int        k;

  if (port == port_KEMPSTON_JOYSTICK)
    k = gamewin->kempston;
  else
    k = zxkeyset_for_port(port, &gamewin->keys);

  return k;
}

static void border_handler(int colour, void *opaque)
{
  // does nothing presently
}

static void speaker_handler(int on_off, zxclock_t tstates, void *opaque)
{
  // does nothing presently
}

///////////////////////////////////////////////////////////////////////////////

static DWORD WINAPI gamewin_thread(LPVOID lpParam)
{
  gamewin_t      *win = (gamewin_t *) lpParam;
  zxgame_launch_t launch;

  launch.model     = g_ops->default_model;
  launch.load_file = NULL;

  // Runs until sleep() reports a quit
  g_ops->run(win->game, &launch);

  return 0;
}

static int CreateGame(gamewin_t *gamewin)
{
  zxconfig_t        zxconfig;
  zxspectrum_t     *zx;
  void             *game;
  HANDLE            thread;
  DWORD             threadId;
  BITMAPINFOHEADER *bmih;

  zxconfig.width  = GAMEWIDTH / 8;
  zxconfig.height = GAMEHEIGHT / 8;
  zxconfig.opaque = gamewin;
  zxconfig.draw   = draw_handler;
  zxconfig.stamp  = stamp_handler;
  zxconfig.sleep  = sleep_handler;
  zxconfig.key    = key_handler;
  zxconfig.border = border_handler;
  zxconfig.speaker = speaker_handler;
  zxconfig.ay_out  = NULL;
  zxconfig.pixel_format = ZX_PIXEL_ARGB8888; // BGRA bytes, as StretchDIBits wants

  zx = zxspectrum_create(&zxconfig);
  if (zx == NULL)
    goto failure;

  game = g_ops->create(zx);
  if (game == NULL)
    goto failure;

  // The thread reads these, so they must be set before it starts
  gamewin->zx       = zx;
  gamewin->game     = game;
  gamewin->quit     = false;
  gamewin->paused   = false;
  gamewin->speed    = NORMSPEED;
  zxkeyset_clear(&gamewin->keys);
  gamewin->kempston = 0;
  memset(gamewin->stamps, 0, sizeof(gamewin->stamps));

  thread = CreateThread(NULL,           // default security attributes
                        0,              // use default stack size
                        gamewin_thread, // thread function name
                        gamewin,        // argument to thread function
                        0,              // use default creation flags
                        &threadId);     // returns the thread identifier
  if (thread == NULL)
    goto failure;

  bmih = &gamewin->bitmapinfo.bmiHeader;
  bmih->biSize          = sizeof(BITMAPINFOHEADER);
  bmih->biWidth         = GAMEWIDTH;
  bmih->biHeight        = -GAMEHEIGHT; // negative height flips the image
  bmih->biPlanes        = 1;
  bmih->biBitCount      = 32;
  bmih->biCompression   = BI_RGB;
  bmih->biSizeImage     = 0; // set to zero for BI_RGB bitmaps
  bmih->biXPelsPerMeter = 0;
  bmih->biYPelsPerMeter = 0;
  bmih->biClrUsed       = 0;
  bmih->biClrImportant  = 0;

  gamewin->thread   = thread;
  gamewin->threadId = threadId;

  return 0;


failure:
  return 1;
}

static void DestroyGame(gamewin_t *doomed)
{
  doomed->quit = true;
  if (g_ops->stop)
    g_ops->stop(doomed->game);

  WaitForSingleObject(doomed->thread, INFINITE);
  CloseHandle(doomed->thread);

  g_ops->destroy(doomed->game);
  zxspectrum_destroy(doomed->zx);
}

///////////////////////////////////////////////////////////////////////////////

LRESULT CALLBACK GameWindowProcedure(HWND   hwnd,
                                     UINT   message,
                                     WPARAM wParam,
                                     LPARAM lParam)
{
  gamewin_t *gamewin;

  gamewin = (gamewin_t *) GetWindowLongPtr(hwnd, GWLP_USERDATA);

  switch (message)
  {
    case WM_CREATE:
      {
        CREATESTRUCT *pCreateStruct = reinterpret_cast<CREATESTRUCT *>(lParam);
        gamewin = reinterpret_cast<gamewin_t *>(pCreateStruct->lpCreateParams);

        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR) gamewin);

        /////////

        int rc;

        rc = CreateGame(gamewin);
        // if (rc)
        //   error;
      }
      break;

    case WM_DESTROY:
      DestroyGame(gamewin);
      PostQuitMessage(0);
      break;

    case WM_PAINT:
      {
        const bool    snap = true;

        float         gameWidth, gameHeight;
        const zx_frame_t *frame;
        PAINTSTRUCT   ps;
        HDC           hdc;
        RECT          clientrect;
        float         windowWidth, windowHeight;
        float         gameWidthsPerWindow, gameHeightsPerWindow;
        float         gamesPerWindow;
        int           drawWidth, drawHeight;
        int           xOffset, yOffset;

        gameWidth  = gamewin->zx->screen.width  * 8;
        gameHeight = gamewin->zx->screen.height * 8;

        frame = zxspectrum_claim_screen(gamewin->zx);

        hdc = BeginPaint(hwnd, &ps);

        GetClientRect(hwnd, &clientrect);

        windowWidth  = clientrect.right  - clientrect.left;
        windowHeight = clientrect.bottom - clientrect.top;

        // How many natural-scale games fit comfortably into the window?
        gameWidthsPerWindow  = (windowWidth  - GAMEBORDER * 2) / gameWidth;
        gameHeightsPerWindow = (windowHeight - GAMEBORDER * 2) / gameHeight;
        gamesPerWindow = min(gameWidthsPerWindow, gameHeightsPerWindow);

        // Try again without a border if the window is very small
        if (gamesPerWindow < 1.0)
        {
          gameWidthsPerWindow  = windowWidth  / gameWidth;
          gameHeightsPerWindow = windowHeight / gameHeight;
          gamesPerWindow = min(gameWidthsPerWindow, gameHeightsPerWindow);
        }

        // Snap the game scale to whole units
        if (gamesPerWindow > 1.0 && snap)
        {
          const float snapSteps = 1.0; // set to 2.0 for scales of 1.0 / 1.5 / 2.0 etc.
          gamesPerWindow = floor(gamesPerWindow * snapSteps) / snapSteps;
        }

        drawWidth  = gameWidth  * gamesPerWindow;
        drawHeight = gameHeight * gamesPerWindow;

        // Centre the game within the window (note that conversion to int floors the values)
        xOffset = (windowWidth  - drawWidth)  / 2;
        yOffset = (windowHeight - drawHeight) / 2;

        StretchDIBits(hdc,
                      xOffset, yOffset,
                      drawWidth, drawHeight,
                      0, 0,
                      GAMEWIDTH, GAMEHEIGHT,
                      frame->pixels,
                      &gamewin->bitmapinfo,
                      DIB_RGB_COLORS,
                      SRCCOPY);

        EndPaint(hwnd, &ps);

        zxspectrum_release_screen(gamewin->zx);
      }
      break;

      // later:
      // case WM_CLOSE: // sent to the window when "X" is pressed
      //     if (MessageBox(hwnd, L"Really quit?", g_ops->name, MB_OKCANCEL) == IDOK)
      //         DestroyWindow(hwnd); // this is the default anyway
      //     break;

    case WM_KEYDOWN:
    case WM_KEYUP:
      {
        bool         down = (message == WM_KEYDOWN);
        zxjoystick_t j;

        switch (wParam)
        {
        case VK_UP:         j = zxjoystick_UP;      break;
        case VK_DOWN:       j = zxjoystick_DOWN;    break;
        case VK_LEFT:       j = zxjoystick_LEFT;    break;
        case VK_RIGHT:      j = zxjoystick_RIGHT;   break;
        case VK_OEM_PERIOD: j = zxjoystick_FIRE;    break;
        default:            j = zxjoystick_UNKNOWN; break;
        }

        if (j != zxjoystick_UNKNOWN)
        {
          zxkempston_assign(&gamewin->kempston, j, down);
        }
        else
        {
          if (wParam == VK_CONTROL)
            zxkeyset_assign(&gamewin->keys, zxkey_CAPS_SHIFT, down);
          else if (wParam == VK_SHIFT)
            zxkeyset_assign(&gamewin->keys, zxkey_SYMBOL_SHIFT, down);
          else
          {
            if (down)
              zxkeyset_setchar(&gamewin->keys, wParam);
            else
              zxkeyset_clearchar(&gamewin->keys, wParam);
          }
        }
      }
      break;

      // later:
      // case WM_COMMAND:
      //     {
      //         int wmId = LOWORD(wParam);
      //         switch (wmId)
      //         {
      //             case 0:// temp
      //             //case ...:
      //             //  break;
      //
      //             default:
      //                 return DefWindowProc(hwnd, message, wParam, lParam);
      //         }
      //     }
      //     break;

    case WM_SIZING:
      return TRUE;

    default:
      return DefWindowProc(hwnd, message, wParam, lParam);
  }

  return 0;
}

BOOL RegisterGameWindowClass(HINSTANCE hInstance)
{
  WNDCLASSEXA wcx;
  ATOM       atom;

  wcx.cbSize        = sizeof(wcx);
  wcx.style         = CS_HREDRAW | CS_VREDRAW; // redraw the entire window whenever its size changes
  wcx.lpfnWndProc   = GameWindowProcedure;
  wcx.cbClsExtra    = 0; // no extra bytes after this class
  wcx.cbWndExtra    = 0;
  wcx.hInstance     = hInstance;
  wcx.hIcon         = g_icon ? LoadIconA(hInstance, MAKEINTRESOURCEA(g_icon)) : NULL;
  wcx.hCursor       = LoadCursorA(NULL, IDC_ARROW);
  wcx.hbrBackground = CreateSolidBrush(0x00000000);
  wcx.lpszMenuName  = NULL;
  wcx.lpszClassName = szGameWindowClassName;
  wcx.hIconSm       = wcx.hIcon;

  atom = RegisterClassExA(&wcx);
  if (!atom)
    return FALSE;

  return TRUE;
}

///////////////////////////////////////////////////////////////////////////////

BOOL CreateGameWindow(HINSTANCE hInstance, int nCmdShow, gamewin_t **new_gamewin)
{
  gamewin_t *gamewin;
  RECT       rect;
  BOOL       result;
  HWND       window;

  *new_gamewin = NULL;

  gamewin = (gamewin_t *) malloc(sizeof(*gamewin));
  if (gamewin == NULL)
    return FALSE;

  // Required window dimensions
  rect.left   = 0;
  rect.top    = 0;
  rect.right  = GAMEWIDTH  + GAMEBORDER * 2;
  rect.bottom = GAMEHEIGHT + GAMEBORDER * 2;

  // Adjust window dimensions for window furniture
  result = AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  if (result == 0)
    goto failure;

  window = CreateWindowExA(0,
                          szGameWindowClassName,
                          g_ops->name,
                          WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT,
                          CW_USEDEFAULT,
                          rect.right - rect.left, rect.bottom - rect.top,
                          NULL,
                          NULL,
                          hInstance,
                          gamewin);
  if (!window)
    goto failure;

  ShowWindow(window, nCmdShow);
  UpdateWindow(window);

  gamewin->window = window;

  *new_gamewin = gamewin;

  return TRUE;


failure:
  free(gamewin);

  return FALSE;
}

void DestroyGameWindow(gamewin_t *doomed)
{
  free(doomed);
}

///////////////////////////////////////////////////////////////////////////////

int zxgame_windows_main(void *hinstance,
                       int         nCmdShow,
                       const zxgame_ops_t *ops,
                       int         icon_resource)
{
  HINSTANCE  hInstance = (HINSTANCE) hinstance;
  int        rc;
  gamewin_t *game1;
  MSG        msg;

  g_ops  = ops;
  g_icon = icon_resource;

  rc = RegisterGameWindowClass(hInstance);
  if (!rc)
    return 0;

  rc = CreateGameWindow(hInstance, nCmdShow, &game1);
  if (!rc)
    return 0;

  while (GetMessage(&msg, NULL, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  DestroyGameWindow(game1);

  return msg.wParam;
}

// vim: ts=8 sts=2 sw=2 et
