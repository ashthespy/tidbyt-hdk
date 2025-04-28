#include "display.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <esp_log.h>
#include <math.h>

#include "driver/gpio.h"
#include "pinsmap.h"

static MatrixPanel_I2S_DMA *_matrix;
static const char *TAG = "display";
static uint8_t _brightness = DISPLAY_DEFAULT_BRIGHTNESS;
static uint8_t _dsplay_night_state = 0;  // Defaults to on.
static uint8_t _dsplay_state = 1;        // Defaults to on.

// Helper function to toggle the display by toggling the LED matrix MOSFET
void toggle_display_night_mode() {
  uint8_t new_level = !_dsplay_night_state;
  gpio_set_level(LED_MATRIX_MOSFET, new_level);
  ESP_LOGI(TAG, "Display night mode toggled %d --> %d", _dsplay_night_state,
           new_level);
  _dsplay_night_state = new_level;
}

void toggle_display() {
  if (_dsplay_state == 1) {
    ESP_LOGI(TAG, "Display toggled %d --> %d", _dsplay_state, 0);
    display_shutdown();

  } else {
    display_initialize();
  }
  _dsplay_state = !_dsplay_state;
}

int display_initialize() {
  // What is the correct driver chip for the adafruit panels?
// Check if we need to power on the panel first
#ifdef TIXEL
  toggle_display_night_mode();
  HUB75_I2S_CFG::shift_driver driver = HUB75_I2S_CFG::FM6126A;
#elif defined(ESPS3)
  HUB75_I2S_CFG::shift_driver driver = HUB75_I2S_CFG::FM6126A;
#else
  HUB75_I2S_CFG::shift_driver driver = HUB75_I2S_CFG::FM6126A;
#endif
  // Initialize the panel.
  HUB75_I2S_CFG::i2s_pins pins = {R1,   G1,   BL1,  R2,   G2,  BL2, CH_A,
                                  CH_B, CH_C, CH_D, CH_E, LAT, OE,  CLK};

#ifdef TIDBYT_GEN2
  bool invert_clock_phase = false;
#else
  bool invert_clock_phase = true;  // Default
#endif

  HUB75_I2S_CFG mxconfig(
      64,                     // width
      32,                     // height
      1,                      // chain length
      pins,                   // pin mapping
      driver,                 // driver chip
      true,                   // double-buffering
      HUB75_I2S_CFG::HZ_10M,  // clock speed
      4,                      // latch blanking
          // https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA?tab=readme-ov-file#latch-blanking
      invert_clock_phase  // invert clock phase
  );

  _matrix = new MatrixPanel_I2S_DMA(mxconfig);

  // Set brightness and clear the screen.
  // _matrix->setBrightness8(DISPLAY_DEFAULT_BRIGHTNESS);
  display_set_brightness(DISPLAY_DEFAULT_BRIGHTNESS);
  _matrix->clearScreen();
  if (!_matrix->begin()) {
    return 1;
  }
  _matrix->fillScreenRGB888(0, 0, 0);

  return 0;
}

void display_start() {
#ifdef TIXEL
  gpio_set_level(LED_MATRIX_MOSFET, 1);
#endif
  _matrix->clearScreen();
  _matrix->stopDMAoutput();
}

void display_shutdown() {
  display_clear();
  _matrix->stopDMAoutput();
#ifdef TIXEL
  gpio_set_level(LED_MATRIX_MOSFET, 0);
#endif
}

static inline uint8_t brightness_percent_to_8bit(uint8_t pct) {
  if (pct > 100) pct = 100;
  // float linear = (float)pct / 100.0f;
  // float gamma = 2.2f;
  // float gamma_corrected = powf(linear, 1.0f / gamma);
  // return (uint8_t)(gamma_corrected * 255.0f + 0.5f);
  return (uint8_t)(((uint32_t)pct * 255 + 50) / 100);
}

void display_set_brightness(uint8_t brightness_pct) {
  if (brightness_pct != _brightness) {
    uint8_t brightness_8bit = brightness_percent_to_8bit(brightness_pct);
    ESP_LOGI(TAG, "Setting brightness to %d%% (%d)", brightness_pct,
             brightness_8bit);
    _matrix->setBrightness8(brightness_8bit);
    _brightness = brightness_pct;
    _matrix->clearScreen();
  }
}

uint8_t get_brightness() { return _brightness; }

// void display_draw(const uint8_t *pix, int width, int height,
// 		  int channels, int ixR, int ixG, int ixB) {
//   for (unsigned int i = 0; i < height; i++) {
//     for (unsigned int j = 0; j < width; j++) {
//       const uint8_t *p = &pix[(i * width + j) * channels];
//       uint8_t r = p[ixR];
//       uint8_t g = p[ixG];
//       uint8_t b = p[ixB];
//       _matrix->drawPixelRGB888(j, i, r, g, b);
//     }
//   }
//   _matrix->flipDMABuffer();
// }

void display_draw(const uint8_t *pix, int width, int height, int channels,
                  int ixR, int ixG, int ixB) {
  if (!pix) {
    ESP_LOGE(TAG, "Can't draw invalid webP pixels!");
    return;
  }
  const int rowStride = width * channels;
  auto m = _matrix;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = pix + y * rowStride;
    const uint8_t *p = row;
    for (int x = 0; x < width; x++, p += channels) {
      if (channels == 4 && p[3] == 0)
        continue;  // Never going to see them so don't draw it?
      m->drawPixelRGB888(x, y, p[ixR], p[ixG], p[ixB]);
    }
  }
  m->flipDMABuffer();
}

// void display_draw(const uint8_t *pix, int width, int height, int channels,
//                   int ixR, int ixG, int ixB) {
//   // pointer to the panel, so we don’t re‑dereference the member every call
//   auto m = _matrix;

//   for (int y = 0; y < height; y++) {
//     const uint8_t *row = pix + y * width * channels;

//     // start a run at x=0
//     int runX = 0;
//     uint8_t *p0 = (uint8_t *)row;
//     uint8_t runR = p0[ixR], runG = p0[ixG], runB = p0[ixB];
//     int runLen = 1;

//     // scan the rest of the row
//     for (int x = 1; x < width; x++) {
//       const uint8_t *p = row + x * channels;
//       uint8_t r = p[ixR], g = p[ixG], b = p[ixB];

//       if (r == runR && g == runG && b == runB) {
//         // extend current run
//         runLen++;
//       } else {
//         // flush previous run
//         m->drawFastHLine(runX, y, runLen, runR, runG, runB);

//         // start new run
//         runX = x;
//         runLen = 1;
//         runR = r;
//         runG = g;
//         runB = b;
//       }
//     }
//     // flush the last run on this row
//     m->drawFastHLine(runX, y, runLen, runR, runG, runB);
//   }

//   // now show the freshly‑drawn back‑buffer in one go
//   m->flipDMABuffer();
// }

void display_clear() { _matrix->fillScreenRGB888(0, 0, 0); }
