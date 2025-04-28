#pragma once

#include <stddef.h>

int gfx_initialize(const void* webp, size_t len);
void gfx_update(const void* webp, size_t len, uint8_t dwell_seconds,
                uint8_t palette_mode);
void gfx_shutdown();
void cycle_display_palette();