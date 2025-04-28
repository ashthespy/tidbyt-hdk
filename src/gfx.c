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
#include "gfx_palette.h"
#include "ota_server.h"  // don't starve

static const char *TAG = "gfx";

#define GFX_TASK_CORE 1
#define GFX_TASK_PRIO 2
#define GFX_TASK_STACK_SIZE 4092

struct gfx_state {
  TaskHandle_t task;
  SemaphoreHandle_t mutex;
  void *buf;
  size_t len;
  uint32_t counter;
  uint8_t dwell_secs;
  uint8_t palette_mode;
};

static struct gfx_state *_state = NULL;

static void gfx_loop(void *arg);
static int draw_webp(uint8_t *webp, size_t len);

int gfx_initialize(const void *webp, size_t len) {
  // Only initialize once
  if (_state) {
    ESP_LOGE(TAG, "Already initialized");
    return 1;
  }

  size_t heapl = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  ESP_LOGI(TAG, "Allocating buffer of %zu on heap of %zu", len, heapl);

  // Initialize state
  _state = calloc(1, sizeof(struct gfx_state));
  _state->len = len;
  _state->buf = calloc(1, len);
  _state->dwell_secs = 1;    // Defaults to 1 second
  _state->palette_mode = 0;  // No mapping

  if (_state->buf == NULL) {
    ESP_LOGE("gfx", "Memory allocation failed!");
    return 1;
  }
  memcpy(_state->buf, webp, len);

  _state->mutex = xSemaphoreCreateMutex();
  if (_state->mutex == NULL) {
    ESP_LOGE(TAG, "Could not create gfx mutex");
    return 1;
  }

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

  // Trigger first draw
  xTaskNotifyGive(_state->task);
  return 0;
}

int gfx_update(const void *webp, size_t len, uint8_t dwell_secs,
               uint8_t palette_mode) {
  // Take mutex
  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "gfx_update: could not take gfx mutex");
    return 1;
  }

  // Update state buffer safely if needed
  if (len > _state->len) {
    void *new_buf = malloc(len);
    if (!new_buf) {
      ESP_LOGE(TAG, "gfx_update: malloc(%u) failed, keeping old buffer", len);
      // release mutex and bail
      xSemaphoreGive(_state->mutex);
      return 1;
    }
    // Free only after allocation
    free(_state->buf);
    _state->buf = new_buf;
  }

  memcpy(_state->buf, webp, len);
  _state->len = len;
  _state->dwell_secs = dwell_secs;
  _state->palette_mode = palette_mode;

  _state->counter++;
  // Notify gfx_loop of update
  xTaskNotifyGive(_state->task);

  // Release mutex
  if (pdTRUE != xSemaphoreGive(_state->mutex)) {
    ESP_LOGE(TAG, "gfx_update: could not give gfx mutex");
    return 1;
  }

  return 0;
}

void gfx_shutdown() {
  vTaskDelete(_state->task);
  vSemaphoreDelete(_state->mutex);
  free(_state->buf);
  free(_state);
  display_shutdown();
}

static void gfx_loop(void *arg) {
  void *webp = NULL;
  size_t buf_len = 0;
  EventGroupHandle_t ev = ota_event_group();
  uint32_t local_counter = -1;

  ESP_LOGI(TAG, "gfx_loop: running on core %d", xPortGetCoreID());

  for (;;) {
    EventBits_t bits = xEventGroupGetBits(ev);
    ESP_LOGI(TAG, "EventGroup bits: 0x%02lx", bits);
    if ((bits & (OTA_READY | OTA_IN_PROGRESS_BIT)) &&
        !(bits & (OTA_SUCCESS_BIT | OTA_FAILED_BIT))) {
      ESP_LOGI(TAG, "gfx_loop: OTA in progress, waiting for completion");
      draw_webp(ASSET_SIMPLE_OTA_WEBP, ASSET_SIMPLE_OTA_WEBP_LEN);
      xEventGroupWaitBits(ev, OTA_SUCCESS_BIT | OTA_FAILED_BIT, pdFALSE,
                          pdFALSE, portMAX_DELAY);
      ESP_LOGI(TAG, "gfx_loop: OTA done, resuming display");
    }
    // Wait for notification
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // ─── Instrumentation ───────────────
    uint32_t frame_count = 0;
    int64_t total_decode_us = 0;
    int64_t last_loop_us = esp_timer_get_time();
    // ───────────────────────────────────

    static int64_t dwell_start_us = 0;
    static int64_t dwell_duration_us = 0;
    static uint8_t dwell_secs = 0;

    // Take mutex and copy latest blob to local buffer if required
    if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
      ESP_LOGE(TAG, "gfx_loop: could not take mutex");
      break;
    }

    // We have a new webp buffer
    if (_state->counter != local_counter) {
      ESP_LOGI(TAG, "gfx_loop: Grabbing WebP #%lu", _state->counter);
      size_t new_len = _state->len;
      if (new_len > buf_len) {
        uint8_t *new_buf = malloc(new_len);
        if (!new_buf) {
          ESP_LOGE(TAG, "gfx_loop: malloc(%zu) failed, keeping old buffer",
                   new_len);
          // Release mutex, skip copy and move on after a bit
          xSemaphoreGive(_state->mutex);
          vTaskDelay(pdMS_TO_TICKS(10));
          continue;
        }
        // allocation succeeded — toss the old buffer
        free(webp);
        webp = new_buf;
        buf_len = new_len;
      }

      // Copy
      memcpy(webp, _state->buf, buf_len);
      local_counter = _state->counter;
      dwell_secs = _state->dwell_secs;
      dwell_start_us = esp_timer_get_time();
      dwell_duration_us = dwell_secs * 1000000LL;
    }

    // Give mutex
    if (pdTRUE != xSemaphoreGive(_state->mutex)) {
      ESP_LOGE(TAG, "gfx_loop: could not give gfx mutex");
      continue;
    }

    ESP_LOGI(TAG, "gfx_loop: Drawing WebP #%lu", local_counter);

    int64_t decode_start = esp_timer_get_time();
    int frames = draw_webp(webp, buf_len);
    int64_t decode_end = esp_timer_get_time();

    if (frames < 0) {
      ESP_LOGE(TAG, "gfx_loop: draw_webp failed");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    total_decode_us += (decode_end - decode_start);
    frame_count += frames;

    int64_t now = esp_timer_get_time();
    int64_t loop_us = now - last_loop_us;
    last_loop_us = now;

    ESP_LOGI(TAG,
             "Stats[frames=%lu]: avg decode = %.1f ms, last loop = %.1f ms",
             (unsigned long)frame_count,
             total_decode_us / (frame_count * 1000.0), loop_us / 1000.0);

    if (frames == 1) {
      ESP_LOGI(TAG, "gfx_loop: static WebP — spinning for %d seconds",
               dwell_secs);
      vTaskDelay(pdMS_TO_TICKS(dwell_secs * 1000));
    } else {
      // We have an animation: so we replay it
      uint8_t replay_count = 0;
      while ((esp_timer_get_time() - dwell_start_us) < dwell_duration_us) {
        if (ota_in_progress()) {
          ESP_LOGI(TAG, "OTA in progress breaking out of ani replay");
          break;
        }
        // — time *each* replayed frame too —
        decode_start = esp_timer_get_time();
        frames = draw_webp(webp, buf_len);
        decode_end = esp_timer_get_time();
        if (frames <= 0) {
          ESP_LOGE(TAG, "gfx_loop: draw_webp failed during animation replay");
          break;
        }
        total_decode_us += (decode_end - decode_start);
        frame_count += frames;
        replay_count++;
        now = esp_timer_get_time();
        loop_us = now - last_loop_us;
        last_loop_us = now;
        // ESP_LOGI(TAG,
        //          "Stats[frames=%lu]: avg decode = %.1f ms, last loop = %.1f
        //          ms", (unsigned long)frame_count, total_decode_us /
        //          (frame_count * 1000.0), loop_us / 1000.0);
        vTaskDelay(pdMS_TO_TICKS(10));  // Don't be greedy
      }
      ESP_LOGI(TAG,
               "Stats: Animation replayed %d time: %lu frames in %.1f ms avg "
               "(total %.1f s), last loop = %.1f ms",
               replay_count, (unsigned long)frame_count,
               total_decode_us / (frame_count * 1000.0),
               total_decode_us / 1000000.0, loop_us / 1000.0);
    }
  }

  free(webp);
  vTaskDelete(NULL);
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

void cycle_display_palette() {
  if (pdTRUE == xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    _state->palette_mode = (_state->palette_mode + 1) % PALETTE_COUNT;
    ESP_LOGI(TAG, "Palette changed to %s",
             gfx_palette_name(_state->palette_mode));
    xSemaphoreGive(_state->mutex);

    // Bump for redraw
    xTaskNotifyGive(_state->task);
  }
}

static int draw_webp(uint8_t *buf, size_t len) {
  // if (!validate_webp_signature(buf, len)) {
  //   ESP_LOGE(TAG, "Invalid WebP header");
  //   return 1;
  // }
  // if (!validate_webp_decode(buf, len)) {
  //   ESP_LOGE(TAG, "Corrupt WebP data");
  //   return 1;
  // }
  // Set up WebP decoder
  WebPData webpData;
  WebPDataInit(&webpData);
  webpData.bytes = buf;
  webpData.size = len;

  WebPAnimDecoderOptions decoderOptions;
  WebPAnimDecoderOptionsInit(&decoderOptions);
  // decoderOptions.color_mode = MODE_RGBA;
  decoderOptions.color_mode = MODE_rgbA;  // premultiplied alpha output

  WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webpData, &decoderOptions);
  if (decoder == NULL) {
    ESP_LOGE(TAG, "Could not create WebP decoder: %u", len);
    return -1;
  }

  WebPAnimInfo animation;
  if (!WebPAnimDecoderGetInfo(decoder, &animation)) {
    ESP_LOGE(TAG, "Could not get WebP animation");
    WebPAnimDecoderDelete(decoder);
    return -1;
  }

  ESP_LOGI(TAG, "WebP: %lu X %lu, %lu frame(s)", animation.canvas_width,
           animation.canvas_height, animation.frame_count);

  int lastTimestamp = 0;
  int delay = 0;
  TickType_t drawStartTick = xTaskGetTickCount();

  // Draw each frame, and sleep for the delay
  for (int j = 0; j < animation.frame_count; j++) {
    uint8_t *pix;
    int timestamp;
    WebPAnimDecoderGetNext(decoder, &pix, &timestamp);
    int64_t t0 = esp_timer_get_time();

    if (_state->palette_mode != PALETTE_NORMAL) {
      ESP_LOGI(TAG, "Frame palette_mode: %d", _state->palette_mode);
      const float (*matrix)[3] = gfx_palette_matrix(_state->palette_mode);
      // SMD optimised - but not really great for our small buffers.
      // We spend more time allocating and freeing - maybe helpful on the S3?
      // gfx_palette_apply_frame(pix, animation.canvas_width,
      // animation.canvas_height, matrix); gfx_palette_apply_frame_rbg(pix,
      // animation.canvas_width, animation.canvas_height, matrix);

      // C-loopys
      gfx_palette_apply(pix, animation.canvas_width, animation.canvas_height,
                        matrix);
      int64_t dt = esp_timer_get_time() - t0;
      ESP_LOGI(TAG, "Frame %d colour shifted to [%d]%s in %lld μs", j,
               _state->palette_mode, gfx_palette_name(_state->palette_mode),
               dt);
    }

    if (delay > 0) xTaskDelayUntil(&drawStartTick, pdMS_TO_TICKS(delay));
    drawStartTick = xTaskGetTickCount();
    display_draw(pix, animation.canvas_width, animation.canvas_height, 4, 0, 1,
                 2);
    delay = timestamp - lastTimestamp;
    lastTimestamp = timestamp;
  }
  if (delay > 0) xTaskDelayUntil(&drawStartTick, pdMS_TO_TICKS(delay));

  // // In case of a single frame, sleep for 1s
  // if (animation.frame_count == 1) {
  //   xTaskDelayUntil(&drawStartTick, pdMS_TO_TICKS(1000));
  // }

  WebPAnimDecoderDelete(decoder);
  return animation.frame_count;
}
