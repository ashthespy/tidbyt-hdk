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
#include "touch.h"
#include "wifi.h"

static const char* TAG = "main";

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

  // Increase brightness using PIN_BUTTON_2 (active low)
  if (gpio_get_level(PIN_BUTTON_2) == 0) {
    uint8_t currentBrightness = get_brightness();
    if (currentBrightness <= 245)
      currentBrightness += 10;
    else
      currentBrightness = 255;
    display_set_brightness(currentBrightness);
    ESP_LOGI(TAG, "Brightness increased to %d", currentBrightness);
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  // Decrease brightness using PIN_BUTTON_3 (active low)
  if (gpio_get_level(PIN_BUTTON_3) == 0) {
    uint8_t currentBrightness = get_brightness();
    if (currentBrightness >= 10)
      currentBrightness -= 10;
    else
      currentBrightness = 0;
    display_set_brightness(currentBrightness);
    ESP_LOGI(TAG, "Brightness decreased to %d", currentBrightness);
    vTaskDelay(pdMS_TO_TICKS(200));
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

  if (start_ota_server()) {
    ESP_LOGE(TAG, "failed to initialize OTA");
  }

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

  // Play a sample. This will only have an effect on Gen 2 devices.
  audio_play(ASSET_LAZY_DADDY_MP3, ASSET_LAZY_DADDY_MP3_LEN);
#ifdef TIXEL
  // Create a separate task for handling button inputs
  xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
#endif
  for (;;) {
    uint8_t* webp;
    size_t len;
    static uint8_t brightness = DISPLAY_DEFAULT_BRIGHTNESS;
    if (remote_get(TIDBYT_REMOTE_URL, &webp, &len, &brightness)) {
      ESP_LOGE(TAG, "Failed to get webp");
    } else {
      display_set_brightness(brightness);
      if (len) {
        ESP_LOGI(TAG, "Updated webp (%d bytes)", len);
        gfx_update(webp, len);
        free(webp);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
