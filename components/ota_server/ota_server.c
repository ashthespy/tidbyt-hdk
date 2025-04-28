#include "ota_server.h"

#include <cJSON.h>

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
    xEventGroupClearBits(
        s_ota_events, OTA_IN_PROGRESS_BIT | OTA_SUCCESS_BIT | OTA_FAILED_BIT);
    ESP_LOGI(TAG, "OTA event group created");
  }
  return s_ota_events;
}

bool ota_in_progress(void) {
  EventGroupHandle_t group = ota_event_group();
  if (!group) return false;  // handle uninitialized case gracefully
  return xEventGroupGetBits(group) & OTA_IN_PROGRESS_BIT;
}

esp_err_t ota_server_init(void) {
  /* 1) Create event‐group */
  if (ota_event_group() == NULL) {
    return ESP_ERR_NO_MEM;
  }

  /* 2) Create queue for incoming OTA requests */
  if (s_ota_queue == NULL) {
    s_ota_queue = xQueueCreate(2, sizeof(ota_request_t));
    if (!s_ota_queue) {
      ESP_LOGE(TAG, "Failed to create OTA queue");
      return ESP_ERR_NO_MEM;
    }
  }

  /* 3) Start HTTPD */
  httpd_handle_t server = NULL;
  httpd_config_t config =
      HTTPD_DEFAULT_CONFIG();  // default config
                               // :contentReference[oaicite:2]{index=2}
  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return err;
  }

  /* 4) Register POST /ota handler */
  httpd_uri_t ota_uri = {.uri = "/ota",
                         .method = HTTP_POST,
                         .handler = ota_pull_handler,
                         .user_ctx = NULL};
  err = httpd_register_uri_handler(server, &ota_uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "register uri handler failed: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "OTA server initialized");
  return ESP_OK;
}

esp_err_t ota_pull_handler(httpd_req_t *req) {
  xEventGroupSetBits(s_ota_events, OTA_READY);
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
  int attempts = 400;  // 400 x 500ms = 200s
  while (attempts-- > 0) {
    EventBits_t result = xEventGroupGetBits(s_ota_events);
    if (result & OTA_SUCCESS_BIT) {
      httpd_resp_sendstr(req, "OTA success");
      return ESP_OK;
    } else if (result & OTA_FAILED_BIT) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
      return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "OTA timeout");
  return ESP_FAIL;

  /* Signal OTA in progress */
  //   xEventGroupSetBits(s_ota_events, OTA_IN_PROGRESS_BIT);
  //   EventBits_t result =
  //       xEventGroupWaitBits(s_ota_events, OTA_SUCCESS_BIT | OTA_FAILED_BIT,
  //                           pdTRUE,               // clear on exit
  //                           pdFALSE,              // wait for any
  //                           pdMS_TO_TICKS(100000)  // timeout
  //       );

  //   if (result & OTA_SUCCESS_BIT) {
  //     httpd_resp_sendstr(req, "OTA success");
  //   } else if (result & OTA_FAILED_BIT) {
  //     httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA
  //     failed");
  //   } else {
  //     httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "OTA timeout");
  //   }
  //   return ESP_OK;
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
  while (1) {
    /* Wait for a queued OTA request */
    if (xQueueReceive(s_ota_queue, &req, portMAX_DELAY) == pdTRUE) {
      ESP_LOGI(TAG, "Starting OTA from %s (MD5=%s)", req.url, req.md5);
      esp_http_client_config_t http_cfg = {
          .url = req.url,
          .timeout_ms = 120000,
          .transport_type = HTTP_TRANSPORT_OVER_TCP,
          .buffer_size = 16 * 1024,
      };
      esp_https_ota_config_t ota_cfg = {
          .http_config = &http_cfg,
      };

      int64_t t0 = esp_timer_get_time();
      esp_err_t err = esp_https_ota(&ota_cfg);
      int64_t t1 = esp_timer_get_time();
      ESP_LOGI(TAG, "OTA took %.2f s", (t1 - t0) / 1e6);

      /* Clear IN_PROGRESS bit */
      xEventGroupClearBits(s_ota_events, OTA_IN_PROGRESS_BIT);

      if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, rebooting");
        xEventGroupSetBits(s_ota_events, OTA_SUCCESS_BIT);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(s_ota_events, OTA_FAILED_BIT);
      }
    }
  }
}
