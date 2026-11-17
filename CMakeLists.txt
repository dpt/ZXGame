# CMakeLists.txt
#
# ZXGame: a portable front-end library for ZX Spectrum game ports.
#
# Provides:
#   ZXSpectrum      - virtual Spectrum (screen, keyboard, Kempston, AY, timing)
#   ZXGameHeadless  - headless front-end (zxgame_headless_main)
#   ZXGameSDL3      - SDL3 front-end (zxgame_sdl3_main), if SDL3 is available
#
# Use from a game's CMakeLists.txt:
#   add_subdirectory(../ZXGame ZXGame)
#   target_link_libraries(MyGame PRIVATE ZXGameSDL3)
#
# Native front-ends for macOS, Windows and RISC OS live in hosts/ and are
# described in hosts/README.md.
#
# vim: sw=2 ts=8 et

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET ZXSpectrum)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ZXGame C)
  set(CMAKE_C_STANDARD 90)
endif()

option(ZXGAME_SDL3 "Build the SDL3 front-end if SDL3 is found" ON)

# ---- ZXSpectrum -------------------------------------------------------------

add_library(ZXSpectrum STATIC
  src/ZXSpectrum/Kempston.c
  src/ZXSpectrum/Keyboard.c
  src/ZXSpectrum/Screen.c
  src/ZXSpectrum/Sound.c
  src/ZXSpectrum/Spectrum.c
  src/ZXSpectrum/Z80.c
  src/ZXSpectrum/slopay-chip.c)

target_include_directories(ZXSpectrum
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Every object linked into a threaded wasm module needs atomics, so the
# games' libraries must be compiled with -pthread too.
if(EMSCRIPTEN)
  target_compile_options(ZXSpectrum PUBLIC "-pthread")
endif()

# ---- Headless ---------------------------------------------------------------

add_library(ZXGameHeadless STATIC hosts/headless/HeadlessHost.c)
target_link_libraries(ZXGameHeadless PUBLIC ZXSpectrum)

# ---- SDL3 -------------------------------------------------------------------

if(ZXGAME_SDL3)
  if(EMSCRIPTEN)
    set(SDL3_FOUND TRUE)
  else()
    # GLOBAL (CMake 3.24+) keeps SDL3::SDL3 visible to the parent project,
    # which zxgame_sdl3_app() refers to from there.
    if(CMAKE_VERSION VERSION_LESS 3.24)
      find_package(SDL3 QUIET)
    else()
      find_package(SDL3 QUIET GLOBAL)
    endif()
  endif()

  if(SDL3_FOUND)
    # Cached so zxgame_sdl3_app() works when called from the parent project.
    set(ZXGAME_SDL3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/hosts/sdl3 CACHE INTERNAL "")

    add_library(ZXGameSDL3 STATIC
      hosts/sdl3/SDLHost.c
      hosts/sdl3/CRTShader.c)
    target_link_libraries(ZXGameSDL3 PUBLIC ZXSpectrum)
    target_include_directories(ZXGameSDL3 PRIVATE hosts/sdl3)

    if(EMSCRIPTEN)
      target_compile_options(ZXGameSDL3 PUBLIC "-sUSE_SDL=3" "-pthread")
    else()
      target_link_libraries(ZXGameSDL3 PUBLIC SDL3::SDL3)
    endif()

    if(APPLE)
      target_link_libraries(ZXGameSDL3 PUBLIC "-framework CoreFoundation")
    endif()

    # The CRT effect, taken alone from DPTLib so as not to need its libpng. For
    # local work on both repos, configure with
    # -DFETCHCONTENT_SOURCE_DIR_DPTLIB_CRT=../DPTLib
    if(NOT TARGET DPTLibCRT)
      include(FetchContent)
      FetchContent_Declare(dptlib_crt
                           GIT_REPOSITORY https://github.com/dpt/DPTLib.git
                           GIT_TAG 1d2c9db4dcfdf7b18a35c4100c2d06d9c45814ce
                           SOURCE_SUBDIR libraries/sdl/crt)
      FetchContent_MakeAvailable(dptlib_crt)
    endif()
    target_link_libraries(ZXGameSDL3 PUBLIC DPTLibCRT)
    if(EMSCRIPTEN)
      # Everything linked into a threaded wasm must be built for threads
      target_compile_options(DPTLibCRT PRIVATE "-pthread")
    endif()

    # zxgame_sdl3_app(<target> [<shell.html>]): per-executable platform fix-ups.
    # The optional shell replaces the default web page for Emscripten builds.
    function(zxgame_sdl3_app target)
      set(shell "${ZXGAME_SDL3_DIR}/shell.html")
      if(ARGC GREATER 1)
        set(shell "${ARGV1}")
      endif()
      if(EMSCRIPTEN)
        set_target_properties(${target} PROPERTIES SUFFIX ".html")
        target_link_options(${target} PRIVATE
          "-sUSE_SDL=3" "-pthread" "-sPTHREAD_POOL_SIZE=4"
          "-sINITIAL_MEMORY=67108864" "-sEXIT_RUNTIME=0"
          "--shell-file=${shell}")
        set_target_properties(${target} PROPERTIES LINK_DEPENDS
          "${shell}")
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy
            "${ZXGAME_SDL3_DIR}/coi-serviceworker.js"
            "$<TARGET_FILE_DIR:${target}>/coi-serviceworker.js")
      endif()
      if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:${target}>
          VERBATIM)
      endif()
    endfunction()
  else()
    message(STATUS "SDL3 not found - skipping the ZXGame SDL3 front-end")
  endif()
endif()

# ---- macOS (Cocoa/Metal) ---------------------------------------------------
#
# An app provides its own main() calling zxgame_macos_main(), its xibs, plist
# and icon. The xibs instantiate AppDelegate, ZXGameWindowController,
# ZXGameWindow and ZXGameView from this library.

if(APPLE AND NOT TARGET ZXGameMacOS)
  add_library(ZXGameMacOS STATIC EXCLUDE_FROM_ALL
    hosts/macos/AppDelegate.m
    hosts/macos/MacMain.m
    hosts/macos/ZXGameView.m
    hosts/macos/ZXGameWindow.m
    hosts/macos/ZXGameWindowController.m
    hosts/macos/ZXAudio.c
    hosts/macos/bitfifo.c)
  target_include_directories(ZXGameMacOS PUBLIC hosts/macos)
  target_compile_options(ZXGameMacOS PRIVATE -fobjc-arc)
  target_link_libraries(ZXGameMacOS PUBLIC ZXSpectrum
    "-framework AudioUnit" "-framework Cocoa"
    "-framework Metal" "-framework MetalKit")
  # The classes are only referenced from xibs, so keep them all.
  target_link_options(ZXGameMacOS INTERFACE -ObjC)
endif()

# ---- Windows (Win32/GDI) ----------------------------------------------------
#
# An app provides WinMain, calling zxgame_windows_main(), plus its resources.

if(WIN32 AND NOT TARGET ZXGameWindows)
  # The host is C++ and a game's project() may list only C.
  enable_language(CXX)
  add_library(ZXGameWindows STATIC EXCLUDE_FROM_ALL hosts/windows/WinHost.cpp)
  target_link_libraries(ZXGameWindows PUBLIC ZXSpectrum)
endif()

# ---- RISC OS (Wimp, GCCSDK) -------------------------------------------------
#
# An app provides main(), calling zxgame_riscos_main(), plus its Resources and
# !Run files. The app's build must give this target the AppEngine and DPTLib
# include paths and link libraries, and the directory holding the generated
# iconnames.h. See hosts/README.md.

if(TARGET_RISCOS AND NOT TARGET ZXGameRISCOS)
  file(GLOB ZXGAME_RISCOS_SOURCES CONFIGURE_DEPENDS hosts/riscos/*.c)
  add_library(ZXGameRISCOS STATIC EXCLUDE_FROM_ALL ${ZXGAME_RISCOS_SOURCES})
  target_include_directories(ZXGameRISCOS PRIVATE hosts/riscos)
  target_link_libraries(ZXGameRISCOS PUBLIC ZXSpectrum)
  set_target_properties(ZXGameRISCOS PROPERTIES C_STANDARD 99)
endif()

endif() # NOT TARGET ZXSpectrum

# ---- Smoke test -------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(NullGameHeadless test/NullGame.c)
  target_link_libraries(NullGameHeadless ZXGameHeadless)

  enable_testing()
  add_test(NAME headless COMMAND NullGameHeadless -frames 1000)

  add_executable(SoundTest test/SoundTest.c)
  target_link_libraries(SoundTest ZXSpectrum)
  add_test(NAME sound COMMAND SoundTest)

  if(TARGET ZXGameSDL3)
    add_executable(NullGameSDL3 test/NullGame.c)
    target_compile_definitions(NullGameSDL3 PRIVATE NULLGAME_SDL)
    target_link_libraries(NullGameSDL3 ZXGameSDL3)
    zxgame_sdl3_app(NullGameSDL3)
  endif()

  if(TARGET ZXGameMacOS)
    # Cocoa smoke test: run it by hand, it opens a window.
    add_executable(NullGameMacOS EXCLUDE_FROM_ALL test/NullGameMac.m)
    target_compile_options(NullGameMacOS PRIVATE -fobjc-arc)
    target_include_directories(NullGameMacOS PRIVATE hosts/macos)
    target_link_libraries(NullGameMacOS ZXGameMacOS)
  endif()
endif()
