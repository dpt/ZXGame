/**
 * CRTShader.h
 *
 * Part of the ZXGame SDL3 front-end. Derived from the Chase H.Q. in C
 * renderer. Copyright (c) 2023-2026 David Thomas.
 */

#ifndef ZXH_CRT_SHADER_H
#define ZXH_CRT_SHADER_H

#include <SDL3/SDL.h>

#include "sdl/crt.h"

#include "ZXSpectrum/Spectrum.h"

/* CRT post-effect: renders the game's converted screen buffer through DPTLib's
 * CRT shader, burning in the OSD on the way, as this path has no SDL_Renderer
 * to draw it with.
 */
typedef struct
{
  crt_t    *crt;
  uint32_t *scratch; // the frame being presented, with any OSD composited
} zxh_CRT_shader_t;

typedef crt_params_t zxh_CRT_params_t;

/**
 * Creates the shader. Must be called after the SDL window is created.
 *
 * \param[out] shader      Shader state to initialise.
 * \param[in]  window      SDL window to claim.
 * \param[in]  game_width  Width in pixels of the game's screen buffer.
 * \param[in]  game_height Height in pixels of the game's screen buffer.
 * \return 1 on success, 0 on failure (an error is printed to stderr), in which
 *         case nothing needs releasing.
 */
int zxh_CRT_shader_create(zxh_CRT_shader_t *shader,
                          SDL_Window       *window,
                          int               game_width,
                          int               game_height);

/**
 * Uploads the game's current screen buffer and renders one frame through
 * the CRT shader pipeline.
 *
 * \param[in,out] shader      Shader state, as created by zxh_CRT_shader_create.
 * \param[in]     window      SDL window to present into.
 * \param[in]     zx          Facade instance to claim the screen buffer from.
 * \param[in]     x           Destination viewport X offset, in window units.
 * \param[in]     y           Destination viewport Y offset, in window units.
 * \param[in]     w           Destination viewport width, in window units.
 * \param[in]     h           Destination viewport height, in window units.
 * \param[in]     game_width  Width in pixels of the game's screen buffer.
 * \param[in]     game_height Height in pixels of the game's screen buffer.
 * \param[in]     params      Shader tuning parameters for this frame.
 * \param[in]     osd_mask    Optional game_width*game_height byte mask,
 *                            top-down: 0 = untouched, 1 = bright green OSD
 *                            text, 2 = black outline stamped behind the
 *                            text. Composited over the uploaded frame
 *                            before the shader pass. NULL to skip.
 * \param[in]     override_pixels Optional game_width*game_height ABGR8888
 *                            pixel buffer to upload instead of the game's
 *                            screen buffer (e.g. the backbuffer debug view).
 *                            NULL to use zx's screen as normal.
 */
void zxh_CRT_shader_render(zxh_CRT_shader_t       *shader,
                           SDL_Window             *window,
                           zxspectrum_t           *zx,
                           int                     x,
                           int                     y,
                           int                     w,
                           int                     h,
                           int                     game_width,
                           int                     game_height,
                           const zxh_CRT_params_t *params,
                           const unsigned char    *osd_mask,
                           const uint32_t         *override_pixels);

/**
 * Releases everything created by zxh_CRT_shader_create.
 *
 * \param[in,out] shader Shader state to release.
 * \param[in]     window SDL window to release.
 */
void zxh_CRT_shader_destroy(zxh_CRT_shader_t *shader, SDL_Window *window);

#endif /* ZXH_CRT_SHADER_H */
