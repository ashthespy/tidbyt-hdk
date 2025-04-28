#include "remote.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_tls.h>

#include "util.h"

static const char* TAG = "remote";

struct remote_state {
  void* buf;
  size_t len;
  size_t size;
  size_t max;
  uint8_t brightness;
  uint8_t dwell_secs;
  uint8_t palette_mode;
  esp_err_t err;
};

#if !defined(HTTP_BUFFER_SIZE_MAX)
#define HTTP_BUFFER_SIZE_MAX 512 * 1024
#endif  // HTTP_BUFFER_SIZE_MAX

#ifndef HTTP_BUFFER_SIZE_DEFAULT
#define HTTP_BUFFER_SIZE_DEFAULT 32 * 1024
#endif

static esp_err_t _httpCallback(esp_http_client_event_t* event) {
  struct remote_state* state = (struct remote_state*)event->user_data;
  // Bail on errors
  if (state->err != ESP_OK) {
    return state->err;
  }
  switch (event->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
      break;

    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;

    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;

    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", event->header_key,
               event->header_value);
      // Failsafe if we can't fit on memory
      // size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      // size_t content_length = strtoul(event->header_value, NULL, 10);
      // if (content_length > free_heap - SAFETY_MARGIN) {
      if (strcmp(event->header_key, "Content-Length") == 0) {
        size_t content_length = (size_t)atoi(event->header_value);
        if (content_length > state->max) {
          ESP_LOGE(TAG,
                   "Content-Length (%zu bytes) exceeds allowed max (%zu bytes)",
                   content_length, state->max);
          free(state->buf);
          state->buf = NULL;
          state->err = ESP_ERR_NO_MEM;
          state->size = 0;
          state->len = 0;
          // esp_http_client_close(event->client);  // Abort the HTTP request
          return state->err;  // Cleanup done by esp_http lib
        } else {
          ESP_LOGI(TAG, "Content-Length Header:%zu bytes", content_length);
        }
        // re-allocate a single time, we know our buffer size..
        free(state->buf);
        state->buf = malloc(content_length);
        if (state->buf == NULL) {
          ESP_LOGE(TAG, "Failed malloc(%zu) for Content-Length",
                   content_length);
          state->size = 0;
          state->len = 0;
          state->err = ESP_ERR_NO_MEM;
          esp_http_client_close(event->client);
          return state->err;
        }
        state->size = content_length;
        ESP_LOGI(TAG, "Resized buffer to Content-Length: %zu bytes",
                 content_length);
      }

      // Check for our Tronbyt-* Headers
      if (strcmp(event->header_key, "Tronbyt-Brightness") == 0) {
        int brightness_pct = atoi(event->header_value);
        state->brightness = (uint8_t)(MIN(MAX(brightness_pct, 0), 100));
        ESP_LOGI(TAG, "Brightness: %s%% --> %d%%", event->header_value,
                 state->brightness);
      } else if (strcmp(event->header_key, "Tronbyt-Dwell-Secs") == 0) {
        state->dwell_secs = (uint8_t)atoi(event->header_value);
        ESP_LOGI(TAG, "Dwell-Secs: %d", state->dwell_secs);
      } else if (strcmp(event->header_key, "Tronbyt-Palette") == 0) {
        state->palette_mode = (uint8_t)atoi(event->header_value);
        ESP_LOGI(TAG, "Palette: %d", state->palette_mode);
      } else {
        ESP_LOGD(TAG, "Unhandled Header: %s", event->header_key);
      }
      break;

    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      break;

    case HTTP_EVENT_ON_DATA:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", event->data_len);
      if (state->buf == NULL || state->err != ESP_OK) {
        break;
      }
      memcpy((uint8_t*)state->buf + state->len, event->data, event->data_len);
      state->len += event->data_len;
      break;

    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;

    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");

      int mbedtlsErr = 0;
      esp_err_t err =
          esp_tls_get_and_clear_last_error(event->data, &mbedtlsErr, NULL);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP error - %s (mbedtls: 0x%x)", esp_err_to_name(err),
                 mbedtlsErr);
      }
      break;
  }

  return state->err;
}

int remote_get(const char* url, uint8_t** buf, size_t* len,
               uint8_t* brightness_pct, uint8_t* dwell_secs,
               uint8_t* palette_mode) {
  // State for processing the response
  struct remote_state state = {
      .buf = malloc(HTTP_BUFFER_SIZE_DEFAULT),
      .len = 0,
      .size = HTTP_BUFFER_SIZE_DEFAULT,
      .max = HTTP_BUFFER_SIZE_MAX,
      .brightness = 0,
      .err = ESP_OK,
  };

  if (state.buf == NULL) {
    ESP_LOGE(TAG, "couldn't allocate HTTP receive buffer");
    return 1;
  }

  // Set up http client
  esp_http_client_config_t config = {
      .url = url,
      .event_handler = _httpCallback,
      .user_data = &state,
      .timeout_ms = 10e3,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t http = esp_http_client_init(&config);

  // Do the request
  esp_err_t err = esp_http_client_perform(http);
  if (err != ESP_OK || state.err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP fetch failed %s: (%s / %s) ", url, esp_err_to_name(err),
             esp_err_to_name(state.err));
    if (state.buf != NULL) {
      free(state.buf);
    }
    esp_http_client_cleanup(http);
    return 1;
  }

  // Write back the results.
  *buf = state.buf;
  *len = state.len;
  // Assumes API provides 0–100 as spec'd, but clamp it just in case
  *brightness_pct = MIN(state.brightness, 100);
  *dwell_secs = state.dwell_secs;
  *palette_mode = state.palette_mode;

  esp_http_client_cleanup(http);

  return 0;
}
