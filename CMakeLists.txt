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
  src/ZXSpectrum/Spectrum.c
  src/ZXSpectrum/Z80.c
  src/ZXSpectrum/slopay-chip.c)

target_include_directories(ZXSpectrum
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- Headless ---------------------------------------------------------------

add_library(ZXGameHeadless STATIC hosts/headless/HeadlessHost.c)
target_link_libraries(ZXGameHeadless PUBLIC ZXSpectrum)

# ---- SDL3 -------------------------------------------------------------------

if(ZXGAME_SDL3)
  if(EMSCRIPTEN)
    set(SDL3_FOUND TRUE)
  else()
    find_package(SDL3 QUIET)
  endif()

  if(SDL3_FOUND)
    set(ZXGAME_SDL3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/hosts/sdl3)

    add_library(ZXGameSDL3 STATIC
      hosts/sdl3/SDLHost.c
      hosts/sdl3/CRTShader.c)
    target_link_libraries(ZXGameSDL3 PUBLIC ZXSpectrum)
    target_include_directories(ZXGameSDL3 PRIVATE hosts/sdl3)

    if(EMSCRIPTEN)
      target_compile_options(ZXGameSDL3 PUBLIC "-sUSE_SDL=3" "-pthread")
      # CRT shader on the web build is GLSL ES over a raw GL context
      # (WebGL1/GLES2), not SDL_GPU. Compiled at runtime by the browser.
      target_compile_definitions(ZXGameSDL3 PRIVATE
        ZXH_CRT_SHADER=1 ZXH_CRT_SHADER_GLES=1)
    else()
      target_link_libraries(ZXGameSDL3 PUBLIC SDL3::SDL3)
    endif()

    if(APPLE)
      target_link_libraries(ZXGameSDL3 PUBLIC "-framework CoreFoundation")
      target_compile_definitions(ZXGameSDL3 PRIVATE ZXH_CRT_SHADER=1)
    endif()

    # The Vulkan backend wants SPIR-V; compile the GLSL at build time.
    if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
      find_program(GLSLANG_VALIDATOR glslangValidator)
      if(GLSLANG_VALIDATOR)
        set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
        set(SPIRV_HEADER ${GEN_DIR}/CRTShaderSPIRV.h)
        file(MAKE_DIRECTORY ${GEN_DIR})
        add_custom_command(
          OUTPUT ${SPIRV_HEADER}
          COMMAND ${GLSLANG_VALIDATOR} -V ${ZXGAME_SDL3_DIR}/shaders/crt.vert -o ${GEN_DIR}/crt.vert.spv
          COMMAND ${GLSLANG_VALIDATOR} -V ${ZXGAME_SDL3_DIR}/shaders/crt.frag -o ${GEN_DIR}/crt.frag.spv
          COMMAND ${CMAKE_COMMAND} -DINPUT=${GEN_DIR}/crt.vert.spv -DOUTPUT=${GEN_DIR}/crt_vert_spirv.h -DVARNAME=zxh_crt_vertex_spirv -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSPIRV.cmake
          COMMAND ${CMAKE_COMMAND} -DINPUT=${GEN_DIR}/crt.frag.spv -DOUTPUT=${GEN_DIR}/crt_frag_spirv.h -DVARNAME=zxh_crt_fragment_spirv -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSPIRV.cmake
          COMMAND ${CMAKE_COMMAND} -E cat ${GEN_DIR}/crt_vert_spirv.h ${GEN_DIR}/crt_frag_spirv.h > ${SPIRV_HEADER}
          DEPENDS ${ZXGAME_SDL3_DIR}/shaders/crt.vert ${ZXGAME_SDL3_DIR}/shaders/crt.frag ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSPIRV.cmake
          VERBATIM)
        add_custom_target(zxgame_crt_shader_spirv DEPENDS ${SPIRV_HEADER})
        add_dependencies(ZXGameSDL3 zxgame_crt_shader_spirv)
        target_include_directories(ZXGameSDL3 PRIVATE ${GEN_DIR})
        target_compile_definitions(ZXGameSDL3 PRIVATE
          ZXH_CRT_SHADER=1 ZXH_CRT_SHADER_SPIRV=1)
      else()
        message(STATUS "glslangValidator not found - no CRT shader on this platform")
      endif()
    endif()

    # zxgame_sdl3_app(<target>): per-executable platform fix-ups.
    function(zxgame_sdl3_app target)
      if(EMSCRIPTEN)
        set_target_properties(${target} PROPERTIES SUFFIX ".html")
        target_link_options(${target} PRIVATE
          "-sUSE_SDL=3" "-pthread" "-sPTHREAD_POOL_SIZE=4"
          "-sINITIAL_MEMORY=67108864" "-sEXIT_RUNTIME=0"
          "--shell-file=${ZXGAME_SDL3_DIR}/shell.html")
        set_target_properties(${target} PROPERTIES LINK_DEPENDS
          "${ZXGAME_SDL3_DIR}/shell.html")
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

# ---- macOS (Cocoa/OpenGL) ---------------------------------------------------
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
    hosts/macos/bitfifo.c)
  target_include_directories(ZXGameMacOS PUBLIC hosts/macos)
  target_compile_options(ZXGameMacOS PRIVATE -fobjc-arc)
  target_compile_definitions(ZXGameMacOS PRIVATE GL_SILENCE_DEPRECATION)
  target_link_libraries(ZXGameMacOS PUBLIC ZXSpectrum
    "-framework AudioUnit" "-framework Cocoa" "-framework CoreImage"
    "-framework OpenGL" "-framework GLUT")
  # The classes are only referenced from xibs, so keep them all.
  target_link_options(ZXGameMacOS INTERFACE -ObjC)
endif()

# ---- Windows (Win32/GDI) ----------------------------------------------------
#
# An app provides WinMain, calling zxgame_windows_main(), plus its resources.

if(WIN32 AND NOT TARGET ZXGameWindows)
  add_library(ZXGameWindows STATIC EXCLUDE_FROM_ALL hosts/windows/WinHost.cpp)
  target_link_libraries(ZXGameWindows PUBLIC ZXSpectrum)
endif()

endif() # NOT TARGET ZXSpectrum

# ---- Smoke test -------------------------------------------------------------

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_executable(NullGameHeadless test/NullGame.c)
  target_link_libraries(NullGameHeadless ZXGameHeadless)

  enable_testing()
  add_test(NAME headless COMMAND NullGameHeadless -frames 1000)

  if(TARGET ZXGameSDL3)
    add_executable(NullGameSDL3 test/NullGame.c)
    target_compile_definitions(NullGameSDL3 PRIVATE NULLGAME_SDL)
    target_link_libraries(NullGameSDL3 ZXGameSDL3)
    zxgame_sdl3_app(NullGameSDL3)
  endif()
endif()
