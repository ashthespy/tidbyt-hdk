#include "time_sync.h"

#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NTP";
static TaskHandle_t time_task_handle = NULL;

static void time_sync_task(void *param) {
  const char *timezone_str = (const char *)param;

  if (timezone_str != NULL) {
    time_set_timezone(timezone_str);
  } else {
    time_set_timezone("UTC");
  }

  if (time_sync_initialize() == ESP_OK) {
    struct tm now;
    if (time_get_current(&now)) {
      ESP_LOGI(TAG, "Synchronized Time: %s", asctime(&now));
    }
  } else {
    ESP_LOGW(TAG, "NTP sync failed");
  }

  vTaskDelete(NULL);
}

void time_start_sync_task(const char *timezone_str) {
  if (time_task_handle == NULL) {
    ESP_LOGI(TAG, "Starting time sync task...");
    xTaskCreate(time_sync_task, "time_sync_task", 4096, (void *)timezone_str, 5,
                &time_task_handle);
  } else {
    ESP_LOGW(TAG, "Time sync task already running");
  }
}

static void time_sync_notification_cb(struct timeval *tv) {
  ESP_LOGI(TAG, "Time synchronized");
}

esp_err_t time_sync_initialize(void) {
  ESP_LOGI(TAG, "Initializing SNTP");

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to be set
  time_t now = 0;
  struct tm timeinfo = {0};
  int retry = 0;
  const int max_retries = 10;

  while (timeinfo.tm_year < (2016 - 1900) && ++retry < max_retries) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
             max_retries);
    vTaskDelay(pdMS_TO_TICKS(2000));
    time(&now);
    localtime_r(&now, &timeinfo);
  }

  if (retry == max_retries) {
    ESP_LOGW(TAG, "Failed to synchronize time");
    return ESP_FAIL;
  }

  esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
  ESP_LOGI(TAG, "Time synchronized successfully");

  return ESP_OK;
}

bool time_get_current(struct tm *timeinfo) {
  time_t now;
  time(&now);
  localtime_r(&now, timeinfo);

  // Return true if time looks valid
  return (timeinfo->tm_year > (2016 - 1900));
}

void time_set_timezone(const char *tz_string) {
  if (tz_string == NULL) {
    ESP_LOGW(TAG, "TZ string is NULL, using UTC");
    tz_string = "UTC";
  }

  ESP_LOGI(TAG, "Setting timezone to: %s", tz_string);
  setenv("TZ", tz_string, 1);
  tzset();
}