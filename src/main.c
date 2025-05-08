#include <assets.h>
#include <esp_log.h>
#include <esp_netif.h>
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
#endif

#ifdef TIXEL
    // Polls button states and calls functions for display toggle and brightness
    // adjustment
    void
    process_buttons() {
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
  // audio_play(ASSET_LAZY_DADDY_MP3, ASSET_LAZY_DADDY_MP3_LEN);
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
  if (wifi_initialize(WIFI_SSID, WIFI_PASSWORD)) {
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
  // audio_play(ASSET_LAZY_DADDY_MP3, ASSET_LAZY_DADDY_MP3_LEN);
#ifdef TIXEL
  // Create a separate task for handling button inputs
  xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
#endif

  const char* hostname;
  esp_netif_t* default_netif = esp_netif_get_default_netif();
  esp_netif_get_hostname(default_netif, &hostname);
  ESP_LOGI(TAG, "Hostname: %s", hostname);
  TickType_t nextDelay = 0;
  for (;;) {
    // block until either OTA_IN_PROGRESS_BIT goes high, or our timer expires
    static uint8_t last_ota_step = 255;
    EventBits_t ev = xEventGroupWaitBits(ota_event_group(), OTA_IN_PROGRESS_BIT,
                                         pdFALSE,  // don’t clear the bit
                                         pdFALSE,  // wait for ANY
                                         nextDelay);

    // Show OTA screen and keep waiting for it to finish
    if (ev & OTA_IN_PROGRESS_BIT) {
      if (ev & OTA_PROGRESS_UPDATED_BIT) {
        uint8_t p = ota_get_progress();

        uint8_t step = 0;
        if (p < 25)
          step = 0;
        else if (p < 50)
          step = 25;
        else if (p < 75)
          step = 50;
        else if (p < 100)
          step = 75;
        else
          step = 100;

        if (step != last_ota_step) {
          last_ota_step = step;
          gfx_show_ota(step);
        }
      }
      // when OTA finishes it will clear that bit; and reboot..
      // but incase, fall through to fetch so nextDelay alone
      vTaskDelay(pdMS_TO_TICKS(250));  // feed the dog
      continue;
    }

    // timer expiry: remote fetch -> update buffer dance
    {
      uint8_t* webp = NULL;
      size_t len = 0;
      uint8_t dwell_secs = MIN_FETCH_INTERVAL;
      static uint8_t brightness = DISPLAY_DEFAULT_BRIGHTNESS;
      uint8_t palette = 0;

      if (remote_get(REMOTE_URL, &webp, &len, &brightness, &dwell_secs,
                     &palette) == 0) {
        display_set_brightness(brightness);
        if (webp && len && brightness) {
          webp_meta_t meta = {
              .dwell_secs = dwell_secs,
              .palette_mode = palette,
          };
          ESP_LOGI(TAG, "Updated webp (%zu bytes)", len);
          gfx_update(webp, len, &meta);
        } else {
          ESP_LOGI(TAG, "Skipping draw of webp (%zu bytes) brightness: %d", len,
                   brightness);
        }
        free(webp);
      } else {
        ESP_LOGE(TAG, "Failed to fetch WebP");
        dwell_secs = MIN_FETCH_INTERVAL;
      }

      // schedule next wakeup: max(dwell, MIN_FETCH_INTERVAL)
      uint32_t ms = MAX((uint32_t)dwell_secs, MIN_FETCH_INTERVAL) * 1000;
      nextDelay = pdMS_TO_TICKS(ms);
      ESP_LOGI(TAG, "Next fetch in %lu ms", ms);
    }
  }
}
