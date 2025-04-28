#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PALETTE_NORMAL = 0,
  PALETTE_NIGHT,
  PALETTE_DIMMED,
  PALETTE_COOL,
  PALETTE_WARM,
  PALETTE_PASTEL,
  PALETTE_MOONLIGHT,
  PALETTE_DUSK,
  PALETTE_VINTAGE,
  PALETTE_BW,
  PALETTE_SUNRISE,
  PALETTE_CYBER,
  PALETTE_COUNT
} gfx_palette_t;

const char *gfx_palette_name(gfx_palette_t mode);
const float (*gfx_palette_matrix(gfx_palette_t mode))[3];
void gfx_palette_apply(uint8_t *pix, int w, int h, const float matrix[3][3]);
void gfx_palette_apply_frame(uint8_t *pix, int w, int h, const float matrix[3][3]);
void gfx_palette_apply_frame_rbg(uint8_t *pix, int w, int h, const float matrix[3][3]);


#ifdef __cplusplus
}
#endif
