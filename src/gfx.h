#pragma once

#include <esp_log.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx_palette.h"

// Metadata for a WebP image slot
typedef struct webp_meta {
  uint8_t dwell_secs;    // Seconds to dwell on this image
  uint8_t palette_mode;  // Palette/transform mode
} webp_meta_t;

// Full image slot containing data and metadata
typedef struct webp_item {
  uint8_t* buf;      // Pointer to WebP image data
  size_t len;        // Actual length of data
  size_t size;       // Allocated size of buffer
  webp_meta_t meta;  // Associated metadata
} webp_item_t;

// GFX Initialization and teardown
int gfx_initialize(const void* webp, size_t len);  // boot WebP on slot 0
void gfx_shutdown(void);

// Slot based API
int gfx_draw_slot(uint8_t slot);
uint8_t gfx_update_slot(uint8_t slot, const void* webp, size_t len,
                        const webp_meta_t* meta);  // Not yet used
int gfx_set_palette(uint8_t slot, gfx_palette_t palette);

// WebP updates
int gfx_update(const void* webp, size_t len,
               const webp_meta_t*
                   meta);  // copies WebP to slot 1 // Caller must free buffer!
int gfx_draw_buffer(const void* buf, size_t len);

// Visual helpers
int gfx_clear(void);
int gfx_show_ota(uint8_t pct);
void cycle_display_palette(void);  // Cycle palette mode on the current slot
