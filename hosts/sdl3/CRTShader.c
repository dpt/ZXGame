/**
 * CRTShader.c
 *
 * Part of the ZXGame SDL3 front-end. Derived from the Chase H.Q. in C
 * renderer. Copyright (c) 2023-2026 David Thomas.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CRTShader.h"

int zxh_CRT_shader_create(zxh_CRT_shader_t *shader,
                          SDL_Window       *window,
                          int               game_width,
                          int               game_height)
{
  shader->scratch = malloc((size_t) game_width * game_height * 4);
  if (shader->scratch == NULL)
  {
    fprintf(stderr, "Error: out of memory for the CRT shader\n");
    return 0;
  }

  /* ABGR8888 pixels are R,G,B,A in memory: see palette_abgr in Screen.c */
  shader->crt = crt_create(window, crt_FORMAT_RGBX8888);
  if (shader->crt == NULL)
  {
    free(shader->scratch);
    shader->scratch = NULL;
    return 0;
  }

  return 1;
}

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
                           const uint32_t         *override_pixels)
{
  size_t            size;
  const zx_frame_t *frame;
  int               i;
  SDL_Rect          viewport;

  size = (size_t) game_width * game_height * 4;

  /* Copy the screen rather than present it directly: crt_render can wait for
   * vsync and the game mustn't be kept from its screen for that long. */
  if (override_pixels != NULL)
  {
    memcpy(shader->scratch, override_pixels, size);
  }
  else
  {
    frame = zxspectrum_claim_screen(zx);
    memcpy(shader->scratch, frame->pixels, size);
    zxspectrum_release_screen(zx);
  }

  /* Mask value 1 = bright green text, matching the plain-renderer OSD; 2 =
   * black outline stamped behind the text so it reads over any background. */
  if (osd_mask != NULL)
  {
    for (i = 0; i < game_width * game_height; i++)
    {
      if (osd_mask[i] == 1)
        shader->scratch[i] = 0xFF40FF40u; /* green interior */
      else if (osd_mask[i] == 2)
        shader->scratch[i] = 0xFF000000u; /* black outline */
    }
  }

  viewport.x = x;
  viewport.y = y;
  viewport.w = w;
  viewport.h = h;

  crt_render(shader->crt, window, shader->scratch, game_width, game_height,
             &viewport, params);
}

void zxh_CRT_shader_destroy(zxh_CRT_shader_t *shader, SDL_Window *window)
{
  crt_destroy(shader->crt, window);
  free(shader->scratch);
}
