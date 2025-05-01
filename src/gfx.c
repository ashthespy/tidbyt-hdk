#include "gfx.h"

#include <assets.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>
#include <webp/demux.h>

#include "display.h"
#include "ota_server.h"  // don't starve
#include "util.h"

static const char *TAG = "gfx";

#define GFX_TASK_CORE 1
#define GFX_TASK_PRIO 2
#define GFX_TASK_STACK_SIZE 4092

#define DRAW_SLOT 0
#define WEBP_LIST_MAX 4

struct gfx_state {
  TaskHandle_t task;
  SemaphoreHandle_t mutex;
  webp_item_t *slots[WEBP_LIST_MAX + 1];  // [0]=draw, [1..4]=apps
  uint32_t counter;
  uint8_t last_slot;
  QueueHandle_t cmd_queue;
};

typedef enum {
  CMD_DRAW_SLOT,
  CMD_DRAW_BUFFER,
  CMD_CLEAR,
  CMD_SET_PALETTE
} gfx_cmd_type_t;

typedef struct {
  gfx_cmd_type_t type;
  uint8_t slot;  // which slot this command targets (if applicable)

  union {
    struct {
      const void *buf;
      size_t len;
    } draw_buffer;

    struct {
      gfx_palette_t palette;
    } set_palette;
    // CLEAR has no extra data
  } u;
} gfx_cmd_t;

static struct gfx_state *_state = NULL;

// minimal WebP decoder state + API
typedef struct {
  WebPAnimDecoder *dec;
  WebPAnimInfo info;
  uint32_t last_ts;
  uint32_t frame_idx;   // which frame in current loop
  uint32_t loop_count;  // how many loops completed
} webp_decoder_t;

// private
static void gfx_loop(void *arg);
// static draw_result_t draw_webp(const uint8_t *buf, size_t len);
uint8_t gfx_activate_slot(uint8_t k);
static bool validate_webp_signature(const uint8_t *data, size_t len);
static inline int webp_decoder_init(webp_decoder_t *d, const uint8_t *buf,
                                    size_t len);
static inline bool webp_decoder_next_frame(webp_decoder_t *d,
                                           uint8_t **out_pixels,
                                           int *out_delay_ms);
static inline void webp_decoder_deinit(webp_decoder_t *d);

int gfx_initialize(const void *boot_webp, size_t boot_len) {
  // Only initialize once
  if (_state) {
    ESP_LOGE(TAG, "Already initialized");
    return 1;
  }

  size_t heapl = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  ESP_LOGI(TAG, "Allocating buffer of %zu on heap of %zu", boot_len, heapl);

  // Initialize state
  _state = calloc(1, sizeof(struct gfx_state));
  _state->mutex = xSemaphoreCreateMutex();
  if (_state->mutex == NULL) {
    ESP_LOGE(TAG, "could not create gfx mutex");
    return 1;
  }

  _state->cmd_queue = xQueueCreate(8, sizeof(gfx_cmd_t));
  if (!_state->cmd_queue) {
    ESP_LOGE(TAG, "failed to create gfx command queue");
    return 1;
  }

  // zero out all slots
  for (uint8_t i = 0; i < WEBP_LIST_MAX; ++i) {
    _state->slots[i] = NULL;
  }

  // ─── pre‐populate slot 0 with the boot WebP ───────────────────
  struct webp_item *boot = malloc(sizeof *boot);
  boot->buf = malloc(boot_len);
  memcpy(boot->buf, boot_webp, boot_len);

  boot->len = boot_len;
  boot->size = boot_len;
  boot->meta =
      (webp_meta_t){.dwell_secs = 0, .palette_mode = 0};  // replay forever
  _state->slots[DRAW_SLOT] = boot;

  // Initialize the display
  if (display_initialize()) {
    return 1;
  }

  // Launch the graphics loop in separate task
  BaseType_t ret = xTaskCreatePinnedToCore(gfx_loop,             // pvTaskCode
                                           "gfx_loop",           // pcName
                                           GFX_TASK_STACK_SIZE,  // usStackDepth
                                           NULL,                 // pvParameters
                                           GFX_TASK_PRIO,        // uxPriority
                                           &_state->task,  // pxCreatedTask
                                           GFX_TASK_CORE   // xCoreID
  );
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Could not create gfx task");
    return 1;
  }

  // Kick things off by drawing our boot screen
  gfx_draw_slot(DRAW_SLOT);
  return 0;
}

// Our API wrappers to our FSM
// internal enqueue helper

// Read only copy of meta for peeks
bool gfx_get_slot_meta(uint8_t slot, webp_meta_t *out) {
  if (slot >= WEBP_LIST_MAX || !out) return false;

  if (xSemaphoreTake(_state->mutex, portMAX_DELAY) == pdTRUE) {
    if (_state->slots[slot]) {
      *out = _state->slots[slot]->meta;  // struct copy
      xSemaphoreGive(_state->mutex);
      return true;
    }
    xSemaphoreGive(_state->mutex);
  }
  return false;
}

static inline int _send_cmd(const gfx_cmd_t *cmd) {
  return xQueueSend(_state->cmd_queue, cmd, pdMS_TO_TICKS(100)) == pdTRUE ? 0
                                                                          : 1;
}

int gfx_draw_slot(uint8_t slot) {
  gfx_cmd_t cmd = {.type = CMD_DRAW_SLOT, .slot = slot};

  return _send_cmd(&cmd);
}

int gfx_draw_buffer(const void *buf, size_t len) {
  gfx_cmd_t cmd = {.type = CMD_DRAW_BUFFER,
                   .slot = 1,  // unused
                   .u.draw_buffer = {.buf = buf, .len = len}};
  return _send_cmd(&cmd);
}

int gfx_set_palette(uint8_t slot, gfx_palette_t palette) {
  gfx_cmd_t cmd = {.type = CMD_SET_PALETTE,
                   .slot = slot,
                   .u.set_palette = {.palette = palette}};
  return _send_cmd(&cmd);
}

int gfx_clear(void) {
  gfx_cmd_t cmd = {.type = CMD_CLEAR};
  return _send_cmd(&cmd);
}

// Draw our OTA progress in batches
int gfx_show_ota(uint8_t step) {
  const void *buf = NULL;
  size_t len = 0;

  switch (step) {
    case 0:
      buf = ASSET_OTA_PROG_0_WEBP;
      len = ASSET_OTA_PROG_0_LEN;
      break;
    case 25:
      buf = ASSET_OTA_PROG_25_WEBP;
      len = ASSET_OTA_PROG_25_LEN;
      break;
    case 50:
      buf = ASSET_OTA_PROG_50_WEBP;
      len = ASSET_OTA_PROG_50_LEN;
      break;
    case 75:
      buf = ASSET_OTA_PROG_75_WEBP;
      len = ASSET_OTA_PROG_75_LEN;
      break;
    case 100:
      buf = ASSET_OTA_PROG_100_WEBP;
      len = ASSET_OTA_PROG_100_LEN;
      break;
    default:
      ESP_LOGW(TAG, "Unknown OTA step %d", step);
      return -1;
  }

  ESP_LOGI(TAG, "Showing OTA update stage %d", step);
  return gfx_draw_buffer(buf, len);
}

void cycle_display_palette(void) {
  uint8_t slot = DRAW_SLOT;

  webp_meta_t meta;
  if (!gfx_get_slot_meta(slot, &meta)) {
    ESP_LOGW(TAG, "cycle_palette: unable to get slot meta");
    return;
  }

  gfx_palette_t next = (meta.palette_mode + 1) % PALETTE_COUNT;

  gfx_cmd_t cmd = {
      .type = CMD_SET_PALETTE,
      .slot = slot,
      .u.set_palette = {.palette = next},
  };

  // Send to gfx loop, just like other commands
  if (xQueueSend(_state->cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "cycle_palette: failed to send command");
  }
}

// Thin wrapper while we refactor
// Takes a remotely filled buffer, and copies it to our screen queue
int gfx_update(const void *webp, size_t len, const webp_meta_t *meta) {
  // We default to slot 1 for now
  const uint8_t slot = 1;
  if (gfx_update_slot(slot, webp, len, meta) != 0) {
    ESP_LOGW(TAG, "failed pushing webp(%zu) to slot %d", len, slot);
    return 1;
  }

  // Swap slots[0]⇄slots[1] atomically
  if (gfx_activate_slot(slot) != 0) {
    ESP_LOGE(TAG, "gfx_update: could not activate slot %d", slot);
    return 1;
  }

  ESP_LOGI(TAG, "gfx_update: webp (%zu) copied  to slot %d", len, slot);
  // Notify our gfx task to draw this
  return gfx_draw_slot(slot);
}

// swap slot k into DRAW_SLOT and wake the render task
uint8_t gfx_activate_slot(uint8_t k) {
  if (k <= 0 || k >= WEBP_LIST_MAX) {
    ESP_LOGE(TAG, "activate_slot: index %d out of range", k);
    return 1;
  }

  // Atomic
  if (xSemaphoreTake(_state->mutex, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "activate_slot: mutex take failed");
    return 1;
  }

  // pointer swap DRAW_SLOT <--> slot k
  webp_item_t *tmp = _state->slots[DRAW_SLOT];
  _state->slots[DRAW_SLOT] = _state->slots[k];
  _state->slots[k] = tmp;

  // Release lock
  if (xSemaphoreGive(_state->mutex) != pdTRUE) {
    ESP_LOGE(TAG, "activate_slot: mutex give failed");
    return 1;
  }

  return 0;
}

// Free memory of a slot.. Use with care!
void gfx_free_slot(uint8_t slot) {
  if (slot <= 0 || slot >= WEBP_LIST_MAX) return;
  xSemaphoreTake(_state->mutex, portMAX_DELAY);
  if (_state->slots[slot]) {
    free(_state->slots[slot]->buf);
    free(_state->slots[slot]);
    _state->slots[slot] = NULL;
  }
  xSemaphoreGive(_state->mutex);
}

void gfx_shutdown() {
  // TODO: tear down slots[ ], free buffers here
  vTaskDelete(_state->task);
  vSemaphoreDelete(_state->mutex);
  free(_state);
  display_shutdown();
}

// Copy a buffer into the prescribed slot
// Caller should free buffer
uint8_t gfx_update_slot(uint8_t slot, const void *webp, size_t len,
                        const webp_meta_t *meta) {
  if (slot == DRAW_SLOT || slot >= WEBP_LIST_MAX) {
    ESP_LOGE(TAG, "update_slot: slot %d is not writable", slot);
    return 1;
  }

  // Buffer sanity checks
  if (webp == NULL || len == 0 || !validate_webp_signature(webp, len)) {
    ESP_LOGE(TAG, "update_slot: buffer (%zu) isn't valid WebP", len);
    return 1;
  }
  // Atomic swap
  // Take mutex
  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "update_slot: could not take gfx mutex");
    return 1;
  }

  webp_item_t *old = _state->slots[slot];

  if (!old) {
    old = calloc(1, sizeof(*old));
    if (!old) {
      ESP_LOGE(TAG, "update_slot: calloc failed");
      xSemaphoreGive(_state->mutex);
      return 1;
    }
    _state->slots[slot] = old;  // Bail gracefully down the line
  }

  // Reallocate buffer if needed
  if (!old->buf || old->size < len) {
    ESP_LOGI(TAG, "update_slot: heap before update: %zu",
             heap_caps_get_free_size(MALLOC_CAP_8BIT));
    uint8_t *new_buf = realloc(old->buf, len);
    if (!new_buf) {
      ESP_LOGE(TAG, "update_slot: realloc(%zu) failed", len);
      xSemaphoreGive(_state->mutex);  // release mutex and bail
      return 1;
    }
    old->buf = new_buf;
    old->size = len;
  }

  // Copy over
  memcpy(old->buf, webp, len);
  old->len = len;
  // copy struct by value
  if (meta) {
    old->meta = *meta;
  } else {
    // Default or previous?
    ESP_LOGW(TAG, "update_slot: no metadata, using previous");
  }

  // Release mutex
  if (pdTRUE != xSemaphoreGive(_state->mutex)) {
    ESP_LOGE(TAG, "update_slot: could not give gfx mutex");
    return 1;
  }

  return 0;
}

static void gfx_loop(void *arg) {
  ESP_LOGI(TAG, "gfx_task: running on core %d", xPortGetCoreID());

  webp_decoder_t dec;
  bool anim_active = false;
  TickType_t next_delay = portMAX_DELAY;
  uint8_t *pixels;
  int delay_ms;

  // dwell state
  uint64_t draw_start_us = 0;
  uint32_t dwell_secs = 0;

  for (;;) {
    gfx_cmd_t cmd;
    bool got = xQueueReceive(_state->cmd_queue, &cmd, next_delay);

    if (got) {
      switch (cmd.type) {
        case CMD_DRAW_SLOT: {
          // grab buffer & meta (atomically)
          const uint8_t *buf;
          size_t len;
          webp_meta_t meta;
          xSemaphoreTake(_state->mutex, portMAX_DELAY);
          buf = _state->slots[DRAW_SLOT]->buf;
          len = _state->slots[DRAW_SLOT]->len;
          meta = _state->slots[DRAW_SLOT]->meta;
          _state->counter++;  // Keep track
          xSemaphoreGive(_state->mutex);

          // dwell instrumentation
          dwell_secs = meta.dwell_secs;
          draw_start_us = esp_timer_get_time();

          // init decoder
          int err = webp_decoder_init(&dec, buf, len);
          if (err == 0) {
            anim_active = true;
            next_delay = 0;  // first frame immediately
            ESP_LOGI(TAG, "[#%lu] drawing (dwell=%lus)", _state->counter,
                     dwell_secs);
          } else {
            ESP_LOGE(TAG, "[#%lu] decoder init failed (%d)", _state->counter,
                     err);
            anim_active = false;
            next_delay = portMAX_DELAY;
          }
          break;
        }
        case CMD_DRAW_BUFFER: {
          // pull the buf+len directly out of the command
          const uint8_t *buf = cmd.u.draw_buffer.buf;
          size_t len = cmd.u.draw_buffer.len;
          // Keep our stats
          dwell_secs = 0;  // show until next command
          draw_start_us = esp_timer_get_time();
          // Init our decoder on this buffer
          if (webp_decoder_init(&dec, buf, len) == 0) {
            anim_active = true;
            next_delay = 0;
          } else {
            ESP_LOGE(TAG, "gfx_task: DRAW_BUFFER decoder init failed");
            display_clear();
            anim_active = false;
            next_delay = pdMS_TO_TICKS(1000);
          }
          break;
        }

        case CMD_CLEAR: {
          if (anim_active) {
            webp_decoder_deinit(&dec);
            anim_active = false;
          }
          display_clear();
          ESP_LOGI(TAG, "CMD_CLEAR");
          next_delay = portMAX_DELAY;
          break;
        }
        case CMD_SET_PALETTE: {
          // Change palette on the given slot (e.g. DRAW_SLOT)
          xSemaphoreTake(_state->mutex, portMAX_DELAY);
          _state->slots[cmd.slot]->meta.palette_mode =
              cmd.u.set_palette.palette;
          ESP_LOGI(TAG, "[#%lu] Palette changed to %s", _state->counter,
                   gfx_palette_name(cmd.u.set_palette.palette));
          xSemaphoreGive(_state->mutex);
          // We need to kick a frame redraw from here..
          // Else let it happen the next frame?
          break;
        }
        default:
          ESP_LOGW(TAG, "gfx_task: unknown command type %d", cmd.type);
          break;
      }
    } else if (anim_active) {
      // check dwell expiry
      if (dwell_secs > 0 && (esp_timer_get_time() - draw_start_us) >=
                                ((uint64_t)dwell_secs * 1000000ULL)) {
        ESP_LOGI(TAG, "[#%lu] dwell (%lus) expired after %lu loops",
                 _state->counter, dwell_secs, dec.loop_count + 1);
        webp_decoder_deinit(&dec);
        anim_active = false;
        next_delay = portMAX_DELAY;
        continue;
      }

      // step one frame
      int64_t t0 = esp_timer_get_time();
      if (webp_decoder_next_frame(&dec, &pixels, &delay_ms)) {
        // draw and schedule next
        gfx_palette_t palette_mode =
            _state->slots[DRAW_SLOT]->meta.palette_mode;
        if (palette_mode != PALETTE_NORMAL) {
          const float (*matrix)[3] = gfx_palette_matrix(palette_mode);
          gfx_palette_apply(pixels, dec.info.canvas_width,
                            dec.info.canvas_height, matrix);
          if (dec.frame_idx == 1) {
            ESP_LOGD(TAG, "[#%lu] palette shDfted to %s in %lu ms ",
                     _state->counter, gfx_palette_name(palette_mode),
                     (uint32_t)(esp_timer_get_time() - t0) / 1000);
          }
        }

        display_draw(pixels, dec.info.canvas_width, dec.info.canvas_height, 4,
                     0, 1, 2);
        int64_t t1 = esp_timer_get_time();
        // Finished playing one loop of our webp
        if (dec.loop_count > 0 && dec.frame_idx == 1) {
          uint32_t loop_ms = (t1 - draw_start_us) / 1000;
          ESP_LOGI(TAG, "[#%lu] loop %lu: %lu ms (%lu frames @ ~%lu ms/frame)",
                   _state->counter, dec.loop_count, loop_ms,
                   dec.info.frame_count, loop_ms / dec.info.frame_count);
        }
        // Track how long each frame took to decode + draw
        // ESP_LOGI(TAG, "gfx: frame %2d/%2lu loop %2d decode %4lldµs delay
        // %3dms",
        //          dec.frame_idx, dec.info.frame_count, dec.loop_count + 1,
        //          (long long)(t1 - t0), delay_ms);
        next_delay = pdMS_TO_TICKS(delay_ms);

      } else {
        uint64_t t1 = esp_timer_get_time();
        uint32_t total_ms = (t1 - draw_start_us) / 1000;
        uint32_t loops = (dec.info.loop_count > 0 ? dec.info.loop_count
                                                  : dec.loop_count + 1);
        uint32_t total_frames = dec.info.frame_count * loops;

        ESP_LOGI(TAG,
                 "[#%lu] total loops %lu: %lu ms (%lu frames @ ~%lu ms/frame)",
                 _state->counter, loops, total_ms, total_frames,
                 total_ms / total_frames);
        webp_decoder_deinit(&dec);
        anim_active = false;
        next_delay = portMAX_DELAY;
      }
    }
    // else idle waiting for next command…
  }
}

static bool validate_webp_decode(const uint8_t *data, size_t len) {
  int width = 0, height = 0;
  // returns 0 on failure (invalid WebP), non‑zero on success
  return WebPGetInfo(data, len, &width, &height) && width > 0 && height > 0;
}

static bool validate_webp_signature(const uint8_t *data, size_t len) {
  // need at least the 12‑byte RIFF+size+WEBP
  if (len < 12) return false;

  // check for “RIFF” … “WEBP”
  if (memcmp(data + 0, "RIFF", 4) != 0) return false;
  if (memcmp(data + 8, "WEBP", 4) != 0) return false;

  return true;
}

static inline int webp_decoder_init(webp_decoder_t *d, const uint8_t *buf,
                                    size_t len) {
  // Ensure we don't mangle existing buffer?
  // Or where should this be?
  webp_decoder_deinit(d);

  WebPData data;

  WebPDataInit(&data);

  data.bytes = buf;
  data.size = len;
  WebPAnimDecoderOptions opt;
  WebPAnimDecoderOptionsInit(&opt);
  opt.color_mode = MODE_rgbA;  // Premultipleid

  d->dec = WebPAnimDecoderNew(&data, &opt);
  if (!d->dec) {
    ESP_LOGE(TAG, "webp_decoder_init: creation failed");
    return -1;
  }
  if (!WebPAnimDecoderGetInfo(d->dec, &d->info)) {
    ESP_LOGE(TAG, "webp_decoder_init: could not get animation info");
    WebPAnimDecoderDelete(d->dec);
    return -2;
  }

  ESP_LOGI(TAG, "webp_info: %lux%lu %lu frame(s) %lu loops",
           d->info.canvas_width, d->info.canvas_height, d->info.frame_count,
           d->info.loop_count);
  d->last_ts = 0;
  d->frame_idx = 0;
  d->loop_count = 0;
  return 0;
}

static inline bool webp_decoder_next_frame(webp_decoder_t *d,
                                           uint8_t **out_pixels,
                                           int *out_delay_ms) {
  int ts;
  if (!WebPAnimDecoderGetNext(d->dec, out_pixels, &ts)) {
    // end of one cycle
    d->loop_count++;
    if (d->info.loop_count > 0 && d->loop_count >= d->info.loop_count) {
      ESP_LOGI(TAG, "webp_decoder_next_frame: loop_count %lu reached limit %lu",
               d->loop_count, d->info.loop_count);
      return false;
    }
    WebPAnimDecoderReset(d->dec);
    d->last_ts = 0;
    d->frame_idx = 0;
    if (!WebPAnimDecoderGetNext(d->dec, out_pixels, &ts)) {
      ESP_LOGE(TAG, "webp_decoder_next_frame: GetNext failed after reset");
      return false;
    }
  }
  int dt = ts - d->last_ts;
  *out_delay_ms = (dt > 0 ? dt : 1);
  d->last_ts = ts;
  d->frame_idx++;
  return true;
}

static inline void webp_decoder_deinit(webp_decoder_t *d) {
  if (d->dec) {
    WebPAnimDecoderDelete(d->dec);
    d->dec = NULL;
  }
}