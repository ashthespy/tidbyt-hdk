#include <assets.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <webp/demux.h>

#include "audio.h"
#include "build_info.h"  // generated
#include "display.h"
#include "driver/gpio.h"
#include "flash.h"
#include "gfx.h"
#include "ota_server.h"
#include "pinsmap.h"
#include "remote.h"
#include "sdkconfig.h"
#include "time_sync.h"
#include "touch.h"
#include "util.h"
#include "wifi.h"

static const char* TAG = "main";
#define MIN_FETCH_INTERVAL 2  // Don't hammer the server.

#ifndef DEFAULT_TIMEZONE
DEFAULT_TIMEZONE = "America/New_York"
#ifdef

#ifdef TIXEL
// Polls button states and calls functions for display toggle and brightness
// adjustment
void process_buttons() {
  // Toggle display with PIN_BUTTON_1 (active low)
  if (gpio_get_level(PIN_BUTTON_1) == 0) {
    toggle_display_night_mode();
    vTaskDelay(pdMS_TO_TICKS(200));  // Debounce delay
  }
  // Toggle display with PIN_BUTTON_1 (active low)
  // if (gpio_get_level(4) == 0) {
  //     toggle_display();
  //     vTaskDelay(pdMS_TO_TICKS(200)); // Debounce delay
  // }

  // Increase brightness
  if (gpio_get_level(PIN_BUTTON_2) == 0) {
    uint8_t _brightness_pct = get_brightness();
    if (_brightness_pct < 100)
      _brightness_pct += 5;
    else
      _brightness_pct = 100;

    display_set_brightness(_brightness_pct);
    vTaskDelay(pdMS_TO_TICKS(200));  // debounce
  }

  // Decrease brightness
  if (gpio_get_level(PIN_BUTTON_3) == 0) {
    uint8_t _brightness_pct = get_brightness();
    if (_brightness_pct >= 5)
      _brightness_pct -= 5;
    else
      _brightness_pct = 0;

    display_set_brightness(_brightness_pct);
    vTaskDelay(pdMS_TO_TICKS(200));  // debounce
  }

  if (gpio_get_level(PIN_BUTTON_4) == 0) {
    cycle_display_palette();
    vTaskDelay(pdMS_TO_TICKS(200));  // Debounce delay
  }
}

void setupGPIOS() {
  // Initialize the LED matrix MOSFET GPIO
  gpio_reset_pin(LED_MATRIX_MOSFET);
  gpio_set_direction(LED_MATRIX_MOSFET, GPIO_MODE_OUTPUT);
  // gpio_set_level(LED_MATRIX_MOSFET_PIN, 1); // Start with display ON

  // Configure buttons as inputs with pull-ups enabled (active low)
  gpio_reset_pin(PIN_BUTTON_1);
  gpio_set_direction(PIN_BUTTON_1, GPIO_MODE_INPUT);
  gpio_pullup_en(PIN_BUTTON_1);

  gpio_reset_pin(PIN_BUTTON_2);
  gpio_set_direction(PIN_BUTTON_2, GPIO_MODE_INPUT);
  gpio_pullup_en(PIN_BUTTON_2);

  gpio_reset_pin(PIN_BUTTON_3);
  gpio_set_direction(PIN_BUTTON_3, GPIO_MODE_INPUT);
  gpio_pullup_en(PIN_BUTTON_3);
  gpio_reset_pin(PIN_BUTTON_4);
  gpio_set_direction(PIN_BUTTON_4, GPIO_MODE_INPUT);
  gpio_pullup_en(PIN_BUTTON_4);
}

// A FreeRTOS task that periodically checks for button presses
void button_task(void* pvParameter) {
  while (1) {
    process_buttons();
    vTaskDelay(pdMS_TO_TICKS(100));  // Polling interval
  }
}
#endif
void _on_touch() {
  ESP_LOGI(TAG, "Touch detected");
  audio_play(ASSET_LAZY_DADDY_MP3, ASSET_LAZY_DADDY_MP3_LEN);
}

void app_main(void) {
  ESP_LOGI(TAG, "Hello world!");
#ifdef TIXEL
  setupGPIOS();
#endif

  ESP_LOGI(TAG, "Fw: %s", BUILD_VERSION);
  ESP_LOGI(TAG, "Built: %s", BUILD_TIMESTAMP);
  // Setup the device flash storage.
  if (flash_initialize()) {
    ESP_LOGE(TAG, "failed to initialize flash");
    return;
  }
  esp_register_shutdown_handler(&flash_shutdown);

  // Setup the display.
  if (gfx_initialize(ASSET_NOAPPS_WEBP, ASSET_NOAPPS_WEBP_LEN)) {
    ESP_LOGE(TAG, "failed to initialize gfx");
    return;
  }
  esp_register_shutdown_handler(&display_shutdown);

  // Setup WiFi.
  if (wifi_initialize(TIDBYT_WIFI_SSID, TIDBYT_WIFI_PASSWORD)) {
    ESP_LOGE(TAG, "failed to initialize WiFi");
    return;
  }
  esp_register_shutdown_handler(&wifi_shutdown);

  if (ota_server_init()) {
    ESP_LOGE(TAG, "failed to initialize OTA");
  }

  // Spawn an OTA task, we don't do much with the handle at this stage
  xTaskCreate(ota_server_task, "OTA", 8 * 1024, NULL, tskIDLE_PRIORITY + 2,
              NULL);

  // Setup audio.
  if (audio_initialize() != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize audio");
    return;
  }

  // Setup touch.
  if (touch_initialize(_on_touch) != ESP_OK) {
    ESP_LOGE(TAG, "failed to initialize touch");
    return;
  }

  uint8_t mac[6];
  if (!wifi_get_mac(mac)) {
    ESP_LOGI(TAG, "WiFi MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
  }

  time_start_sync_task(DEFAULT_TIMEZONE);

  // Play a sample. This will only have an effect on Gen 2 devices.
  audio_play(ASSET_LAZY_DADDY_MP3, ASSET_LAZY_DADDY_MP3_LEN);
#ifdef TIXEL
  // Create a separate task for handling button inputs
  xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
#endif
  for (;;) {
    if (ota_in_progress()) {
      ESP_LOGW(TAG, "OTA in progress — skipping remote fetch");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    uint8_t* webp = NULL;
    size_t len = 0;
    static uint8_t brightness = DISPLAY_DEFAULT_BRIGHTNESS;
    uint8_t dwell_secs = 1;
    uint8_t palette_mode = 0;

    if (remote_get(TIDBYT_REMOTE_URL, &webp, &len, &brightness, &dwell_secs,
                   &palette_mode) == 0) {
      display_set_brightness(brightness);
      if (webp && len && brightness) {
        ESP_LOGI(TAG, "Updated webp (%d bytes)", len);
        gfx_update(webp, len, dwell_secs, palette_mode);
      } else {
        ESP_LOGI(TAG, "Skipping draw of webp (%d bytes) brightness: %d", len,
                 brightness);
      }

      free(webp);  // caller must free
    } else {
      ESP_LOGE(TAG, "Failed to fetch WebP from remote.");
      dwell_secs = MIN_FETCH_INTERVAL;  // Retry again
    }
    uint32_t sleep_ms = MAX(dwell_secs, MIN_FETCH_INTERVAL) * 1000;
    ESP_LOGI(TAG, "waiting %lu ms before next fetch", sleep_ms);
    vTaskDelay(pdMS_TO_TICKS(sleep_ms));
  }
}
