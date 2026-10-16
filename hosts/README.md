# Front-ends

All front-ends drive a game through `zxgame_ops_t` (`include/ZXGame/Game.h`).

| Directory | Status | Entry point |
|-----------|--------|-------------|
| `headless` | Built and tested | `zxgame_headless_main` |
| `sdl3` | Built and run on macOS | `zxgame_sdl3_main` |
| `macos` | Built and launched on macOS (Cocoa/OpenGL) | `zxgame_macos_main` |
| `windows` | Ported, **not compiled** | `zxgame_windows_main` |
| RISC OS | **Not yet extracted** | see below |

## macOS

An app supplies `main()`, its xibs, `Info.plist` and icon. The xibs instantiate
`AppDelegate`, `ZXGameWindowController`, `ZXGameWindow` and `ZXGameView`.
Link `ZXGameMacOS`.

## Windows

An app supplies `WinMain` and its resources, and calls `zxgame_windows_main`.
Link `ZXGameWindows`.

## RISC OS

Two hosts exist and neither is in this repository yet:

* The Great Escape's GCCSDK/AppEngine Wimp application
  (`platform/riscos/!GtEscape`). It has been updated to the current
  `ZXSpectrum` API and drives the game through `zxgame_ops_t::step`, but is
  still TGE specific.
* Chase H.Q.'s Norcroft/RISC_OSLib application (`apps/riscos`). It uses the
  `ZXSpectrum` sources from this repository directly.

Neither has been built since the change.
