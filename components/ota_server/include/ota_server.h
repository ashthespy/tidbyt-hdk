#ifndef OTA_SERVER_H
#define OTA_SERVER_H

#include "esp_err.h"

/**
 * @brief  Start the HTTP‑POST OTA server (registers /update, etc).
 * @return ESP_OK on success, or an esp_err_t on failure.
 */
esp_err_t start_ota_server(void);

#endif // OTA_SERVER_H
