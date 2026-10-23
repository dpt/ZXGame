# ZXGame

A portable front-end library for ZX Spectrum game ports written in C.

* `src/ZXSpectrum`: a virtual Spectrum (screen conversion, keyboard, Kempston,
  128K paging, AY, timing, and macros and helpers that emulate Z80
  instructions for code translated from a disassembly). It has no Z80 CPU
  emulation.
* `include/ZXGame/Game.h`: `zxgame_ops_t`, the vtable a game supplies.
* `hosts/`: front-ends for SDL3, headless, macOS, Windows and RISC OS. See
  `hosts/README.md` for the state of each.

Used by [The Great Escape in C](https://github.com/dpt/The-Great-Escape-in-C) and
[Chase H.Q. in C](https://github.com/dpt/ChaseHQ).

## Using it

```cmake
include(FetchContent)
FetchContent_Declare(ZXGame
  GIT_REPOSITORY https://github.com/dpt/ZXGame
  GIT_TAG 0.1.0)
FetchContent_MakeAvailable(ZXGame)
target_link_libraries(MyGame PRIVATE ZXGameSDL3)   # or ZXGameHeadless
zxgame_sdl3_app(MyGame)
```

```c
int main(int argc, char *argv[])
{
  return zxgame_sdl3_main(argc, argv, &my_game_ops);
}
```

`test/NullGame.c` is a minimal game. Run `ctest` in a build directory to check
the headless front-end.
