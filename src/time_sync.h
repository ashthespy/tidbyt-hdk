#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the time synchronization task
 *
 * @param timezone_str Timezone string to use (e.g., "UTC",
 * "EST5EDT,M3.2.0,M11.1.0")
 */
void time_start_sync_task(const char *timezone_str);

/**
 * @brief Initialize and synchronize time via NTP.
 *
 * @return esp_err_t - ESP_OK on success, or appropriate error.
 */
esp_err_t time_sync_initialize(void);

/**
 * @brief Get the current system time as `struct tm`.
 *
 * @param[out] timeinfo Pointer to a `struct tm` to fill.
 * @return true if time is valid, false otherwise.
 */
bool time_get_current(struct tm *timeinfo);

/**
 * @brief Set the timezone for the system.
 *
 * @param tz_string Timezone string (e.g., "CST6CDT,M3.2.0,M11.1.0" or "UTC")
 */
void time_set_timezone(const char *tz_string);

#ifdef __cplusplus
}
#endif
