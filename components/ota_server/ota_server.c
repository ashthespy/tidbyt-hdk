#include "ota_server.h"

#include <cJSON.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

static const char *TAG = "OTA_SERVER";

/* Event‐group and queue */
static EventGroupHandle_t s_ota_events;
static QueueHandle_t s_ota_queue;
static volatile uint8_t s_ota_percent = 0;
static int s_ota_total_bytes = 0;
static int s_ota_bytes_read = 0;

/* Structure to hold one OTA request */
typedef struct {
  char url[OTA_URL_MAX_LEN];
  char md5[OTA_MD5_MAX_LEN];
  char version[OTA_VERSION_MAX_LEN];
} ota_request_t;

EventGroupHandle_t ota_event_group(void) {
  if (s_ota_events == NULL) {
    s_ota_events = xEventGroupCreate();
    if (!s_ota_events) {
      ESP_LOGE(TAG, "Failed to create event group");
      return NULL;
    }
    // start with all bits clear
    xEventGroupClearBits(s_ota_events, OTA_QUEUED_BIT | OTA_IN_PROGRESS_BIT |
                                           OTA_SUCCESS_BIT | OTA_FAILED_BIT |
                                           OTA_PROGRESS_UPDATED_BIT);
    ESP_LOGI(TAG, "OTA event group created");
  }
  return s_ota_events;
}

QueueHandle_t ota_request_queue(void) {
  if (!s_ota_queue) {
    s_ota_queue = xQueueCreate(2, sizeof(ota_request_t));
    if (!s_ota_queue) {
      ESP_LOGE(TAG, "Failed to create OTA request queue");
    }
  }
  return s_ota_queue;
}

// HTTP client event handler: updates s_ota_percent on each data chunk
static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
      if (evt->header_key &&
          strcasecmp(evt->header_key, "Content-Length") == 0) {
        s_ota_total_bytes = atoi(evt->header_value);
        ESP_LOGI(TAG, "OTA image size: %d bytes", s_ota_total_bytes);
        s_ota_bytes_read = 0;
        s_ota_percent = 0;
      }
      break;
    case HTTP_EVENT_ON_DATA:
      if (s_ota_total_bytes > 0) {
        s_ota_bytes_read += evt->data_len;
        int pct = (s_ota_bytes_read * 100) / s_ota_total_bytes;
        if (pct > 100)
          pct = 100;
        else if (pct < 0)
          pct = 0;
        if (pct != s_ota_percent) {
          s_ota_percent = pct;
          xEventGroupSetBits(s_ota_events, OTA_PROGRESS_UPDATED_BIT);
        }
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

bool ota_in_progress(void) {
  EventGroupHandle_t group = ota_event_group();
  if (!group) return false;  // handle uninitialized case gracefully
  return xEventGroupGetBits(group) & OTA_IN_PROGRESS_BIT;
}

uint8_t ota_get_progress(void) { return s_ota_percent; }

esp_err_t ota_status_handler(httpd_req_t *req) {
  EventBits_t bits = xEventGroupGetBits(s_ota_events);
  const char *status;
  uint8_t progress = ota_get_progress();

  if (bits & OTA_QUEUED_BIT) {
    status = "OTA_QUEUED";
  } else if (bits & OTA_IN_PROGRESS_BIT) {
    status = "OTA_INPROGRESS";
  } else if (bits & OTA_SUCCESS_BIT) {
    status = "OTA_SUCCESS";
  } else if (bits & OTA_FAILED_BIT) {
    status = "OTA_FAILED";
  } else {
    status = "IDLE";
  }

  char buf[64];
  int len = snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"progress\":%u}",
                     status, progress);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

esp_err_t ota_server_init(void) {
  // Init event group and Queue to notify our OTA server to pull something
  if (!ota_event_group() || !ota_request_queue()) {
    return ESP_ERR_NO_MEM;
  }

  // Our PUll and Status handlers
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();  // default config
  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return err;
  }

  // Register POST /ota handler
  httpd_uri_t ota_uri = {.uri = "/ota",
                         .method = HTTP_POST,
                         .handler = ota_pull_handler,
                         .user_ctx = NULL};
  err = httpd_register_uri_handler(server, &ota_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Pull: /ota handler failed: %s", esp_err_to_name(err));
    return err;
  }

  // Register a simpler /ota/status handler to relay bits
  httpd_uri_t ota_status_uri = {
      .uri = "/ota/status",
      .method = HTTP_GET,
      .handler = ota_status_handler,
      .user_ctx = NULL,
  };
  err = httpd_register_uri_handler(server, &ota_status_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Status: /ota/status handler failed: %s",
             esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "OTA server initialized");
  return ESP_OK;
}

esp_err_t ota_pull_handler(httpd_req_t *req) {
  xEventGroupSetBits(s_ota_events, OTA_QUEUED_BIT);
  // Read the full POST body
  int len = req->content_len;
  if (len <= 0 || len >= 512) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    return ESP_FAIL;
  }

  char buf[512] = {0};
  int ret = httpd_req_recv(req, buf, len);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
    return ESP_FAIL;
  }

  // Parse JSON body
  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    return ESP_FAIL;
  }

  const cJSON *j_url = cJSON_GetObjectItem(root, "url");
  const cJSON *j_md5 = cJSON_GetObjectItem(root, "MD5");
  const cJSON *j_version = cJSON_GetObjectItem(root, "version");

  if (!cJSON_IsString(j_url) || !cJSON_IsString(j_md5)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
    return ESP_FAIL;
  }

  ota_request_t req_data = {{0}, {0}, {0}};
  strlcpy(req_data.url, j_url->valuestring, sizeof(req_data.url));
  strlcpy(req_data.md5, j_md5->valuestring, sizeof(req_data.md5));
  if (cJSON_IsString(j_version)) {
    strlcpy(req_data.version, j_version->valuestring, sizeof(req_data.version));
  }

  ESP_LOGI(TAG, "OTA request: URL=%s, MD5=%s, version=%s", req_data.url,
           req_data.md5, req_data.version[0] ? req_data.version : "(none)");
  cJSON_Delete(root);

  if (xQueueSend(s_ota_queue, &req_data, 0) != pdPASS) {
    httpd_resp_send_err(req, 503, "queue full");
    return ESP_FAIL;
  }

  /* Signal OTA in progress */
  xEventGroupSetBits(s_ota_events, OTA_IN_PROGRESS_BIT);
  httpd_resp_sendstr(req, "OTA_QUEUED");
  return ESP_OK;
}

static void reboot(void) {
  for (int i = 5; i > 0; i--) {
    ESP_LOGI(TAG, "Rebooting in %d seconds...", i);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  esp_restart();
}

void ota_server_task(void *pvParameter) {
  ota_request_t req;
  esp_err_t err;
  while (xQueueReceive(ota_request_queue(), &req, portMAX_DELAY) == pdTRUE) {
    ESP_LOGI(TAG, "OTA begin: %s", req.url);

    // clear QUEUED, set IN_PROGRESS
    xEventGroupClearBits(s_ota_events,
                         OTA_QUEUED_BIT | OTA_SUCCESS_BIT | OTA_FAILED_BIT);
    xEventGroupSetBits(s_ota_events, OTA_IN_PROGRESS_BIT);

    // configure OTA
    esp_http_client_config_t http_cfg = {
        .url = req.url,
        .timeout_ms = 120000,
        .event_handler = ota_http_event_handler,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    if (esp_https_ota_begin(&ota_cfg, &handle) != ESP_OK) {
      ESP_LOGE(TAG, "ota_begin failed");
      xEventGroupSetBits(s_ota_events, OTA_FAILED_BIT);
      continue;
    }

    // Track our OTA chunks until otherwise
    do {
      err = esp_https_ota_perform(handle);
      // yield to other tasks so event callback can run
      vTaskDelay(pdMS_TO_TICKS(100));
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    // final cleanup
    xEventGroupClearBits(s_ota_events, OTA_IN_PROGRESS_BIT);
    err = esp_https_ota_finish(handle);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "OTA success, rebooting");
      xEventGroupSetBits(s_ota_events, OTA_SUCCESS_BIT);
      // signal we done
      s_ota_percent = 100;
      xEventGroupSetBits(s_ota_events, OTA_PROGRESS_UPDATED_BIT);
      vTaskDelay(pdMS_TO_TICKS(750));
      reboot();
    } else {
      ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
      xEventGroupSetBits(s_ota_events, OTA_FAILED_BIT);
    }
  }
}
