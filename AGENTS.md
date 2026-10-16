# AGENTS.md

## What this is

A portable front-end library for ZX Spectrum game ports written in C (C90 for the core; the RISC OS host uses C99). It is consumed by other repositories (The Great Escape in C, Chase H.Q. in C) through `FetchContent` or `add_subdirectory`, so CMake targets and public headers are the API.

## Commands

```sh
cmake -S . -B build -G Ninja      # build/ is gitignored and already configured
cmake --build build
ctest --test-dir build            # runs: NullGameHeadless -frames 1000
build/NullGameHeadless -frames 1000
```

`cmake --build build --target NullGameMacOS` builds a Cocoa smoke test (`test/NullGameMac.m`, builds the window in code, `-seconds N` auto-quits) that opens a window; run it by hand.

The only automated test is that smoke test (`test/NullGame.c`, a minimal game). There is no lint target; `.clang-format` exists. `NullGameSDL3` builds only when SDL3 is found (`-DZXGAME_SDL3=OFF` skips it). Windows, RISC OS and macOS-Cocoa hosts are `EXCLUDE_FROM_ALL` and built only from a game's project.

## Architecture

- `include/ZXGame/Game.h`: `zxgame_ops_t`, the vtable a game supplies. Every front-end drives a game only through it. Read its comments for the contract: `run` (blocking, on a game thread) versus `step` (cooperative, for front-ends with no thread; `run` wins if both are given), optional `save`/`load`/`dispose_error` (UI offered only if all three exist), `scripted_key` (headless input), `osd_glyph`.
- `src/ZXSpectrum` + `include/ZXSpectrum`: the virtual Spectrum (screen conversion, keyboard, Kempston, 128K paging, AY, timing). It has **no Z80 CPU emulation**. `Z80.h`/`Macros.h` supply macros and helpers that emulate Z80 instructions for code translated from a disassembly.
- `hosts/<name>`: one front-end per platform, each exposing `zxgame_<name>_main`. See `hosts/README.md` for the status of each (headless and SDL3 built and tested; macOS built; Windows and RISC OS not compiled).
  - `sdl3`: CRT shader in `hosts/sdl3/shaders`. Backend varies: Metal on macOS, SPIR-V on Linux (compiled at build time with `glslangValidator` through `cmake/EmbedSPIRV.cmake`), GLSL ES raw GL on Emscripten. `zxgame_sdl3_app(target [shell.html])` applies per-executable fix-ups (Emscripten shell page, service worker, Windows SDL DLL copy).
  - `macos`: classes are referenced only from the app's xibs, hence the `-ObjC` interface link option.
  - `riscos`: the app supplies `!Run`, Resources and `iconnames.h`; the menu layout is fixed.
- Root `CMakeLists.txt` is guarded by `if(NOT TARGET ZXSpectrum)` so it is safe to include from a parent project. The smoke test is built only when ZXGame is the top-level project.

## Gotchas

- Emscripten builds need `-pthread` on every object, including the game's own libraries. `ZXSpectrum` propagates it publicly.
- Keep code C90-compatible in `src/` and `include/`.
