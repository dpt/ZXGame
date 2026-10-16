# Front-ends

All front-ends drive a game through `zxgame_ops_t` (`include/ZXGame/Game.h`).

| Directory | Status | Entry point |
|-----------|--------|-------------|
| `headless` | Built and tested | `zxgame_headless_main` |
| `sdl3` | Built and run on macOS | `zxgame_sdl3_main` |
| `macos` | Built and launched on macOS (Cocoa/OpenGL) | `zxgame_macos_main` |
| `windows` | Ported, **not compiled** | `zxgame_windows_main` |
| `riscos` | Moved from The Great Escape, generalised, **not built** | `zxgame_riscos_main` |

## macOS

An app supplies `main()`, its xibs, `Info.plist` and icon. The xibs instantiate
`AppDelegate`, `ZXGameWindowController`, `ZXGameWindow` and `ZXGameView`.
Link `ZXGameMacOS`.

## Windows

An app supplies `WinMain` and its resources, and calls `zxgame_windows_main`.
Link `ZXGameWindows`.

## RISC OS

The GCCSDK/AppEngine/OSLib Wimp application. An app supplies:

* `main()`, calling `zxgame_riscos_main(argc, argv, &ops, &app)`, where
  `zxgame_riscos_app_t` gives the app name (for `<appname>Res:` and
  `<appname>$ScreenMode`), file type, Filer window sprite and default save name.
* Its bundle: `!Run`, `!Boot`, `!Help`, sprites and `Resources` (Templates,
  Messages, Sprites). The templates must have the windows and menus the host
  expects (see `menunames.h`).
* A build that generates `iconnames.h` from the templates with `templheadr`,
  and gives `ZXGameRISCOS` the AppEngine, DPTLib and OSLib include paths and
  libraries. See The Great Escape's `platform/riscos/!GtEscape/CMakeLists.txt`.

Save and screenshot UI is always compiled in. Whether saving is offered at
run time depends on the game supplying `save`, `load` and `in_menu`; a game
without saves should ship templates whose menu still contains the save entries.
