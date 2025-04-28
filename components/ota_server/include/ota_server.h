#ifndef OTA_SERVER_H
#define OTA_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

/* bits for the OTA event‐group */
#define OTA_READY (1 << 0)
#define OTA_IN_PROGRESS_BIT (1 << 1)
#define OTA_SUCCESS_BIT (1 << 2)
#define OTA_FAILED_BIT (1 << 3)

/* maximum lengths for URL and MD5 */
#define OTA_URL_MAX_LEN 256
#define OTA_MD5_MAX_LEN 33
#define OTA_VERSION_MAX_LEN 32

/**
 * @brief  Initialize the OTA server component:
 *         - create event‐group and queue
 *         - start HTTPD and register the POST handler
 * @return ESP_OK or an esp-err code
 */
esp_err_t ota_server_init(void);

/**
 * @brief  HTTP POST handler; parses form body for
 *         host, port, path, MD5 and queues an OTA request
 */
esp_err_t ota_pull_handler(httpd_req_t *req);

/**
 * @brief  Get the OTA event‐group handle (for status bits)
 */
EventGroupHandle_t ota_event_group(void);

/**
 * @brief  FreeRTOS task entry that blocks on the
 *         OTA‐request queue and runs esp_https_ota()
 */
void ota_server_task(void *pvParameter);

/**
 * @brief  Check if an OTA is currently in progress
 */
bool ota_in_progress(void);

#endif  // OTA_SERVER_H
