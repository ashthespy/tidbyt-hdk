#include "display.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "driver/gpio.h"

#ifdef TIDBYT_GEN2
#define R1 5
#define G1 23
#define BL1 4
#define R2 2
#define G2 22
#define BL2 32

#define CH_A 25
#define CH_B 21
#define CH_C 26
#define CH_D 19
#define CH_E -1  // assign to pin 14 if using more than two panels

#define LAT 18
#define OE 27
#define CLK 15
#elif defined(TIXEL)
#pragma message "Compiling for TIXEL board pins"
#define LED_MATRIX_MOSFET GPIO_NUM_4
#define R1 22
#define G1 32
#define BL1 21
#define R2 19
#define G2 33
#define BL2 17

#define CH_A 16
#define CH_B 25
#define CH_C 27
#define CH_D 26
#define CH_E -1

#define LAT 14
#define OE 13
#define CLK 12
#elif defined(ESPS3)
#pragma message "Compiling for ESPS3 board pins"
// #define LED_MATRIX_MOSFET GPIO_NUM_4
#define R1 4          // R1_PIN_DEFAULT -> R1
#define G1 5          // G1_PIN_DEFAULT -> G1
#define BL1 6         // B1_PIN_DEFAULT -> BL1
#define R2 7          // R2_PIN_DEFAULT -> R2
#define G2 15         // G2_PIN_DEFAULT -> G2
#define BL2 16        // B2_PIN_DEFAULT -> BL2 (Note: You had 17, but default is 16)

#define CH_A 18       // A_PIN_DEFAULT -> CH_A
#define CH_B 8        // B_PIN_DEFAULT -> CH_B
#define CH_C 3        // C_PIN_DEFAULT -> CH_C
#define CH_D 42       // D_PIN_DEFAULT -> CH_D
#define CH_E -1       // E_PIN_DEFAULT -> CH_E (kept as -1 since it's required for 1/32 panels)

#define LAT 40        // LAT_PIN_DEFAULT -> LAT
#define OE 2          // OE_PIN_DEFAULT -> OE
#define CLK 41        // CLK_PIN_DEFAULT -> CLK
#else
#define R1 21
#define G1 2
#define BL1 22
#define R2 23
#define G2 4
#define BL2 27

#define CH_A 26
#define CH_B 5
#define CH_C 25
#define CH_D 18
#define CH_E -1  // assign to pin 14 if using more than two panels

#define LAT 19
#define OE 32
#define CLK 33
#endif

static MatrixPanel_I2S_DMA *_matrix;

int display_initialize() {
  // Check if we need to power on the panel first
  #ifdef TIXEL   
    gpio_reset_pin(LED_MATRIX_MOSFET);
    gpio_set_direction(LED_MATRIX_MOSFET, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_MATRIX_MOSFET, 1);
    HUB75_I2S_CFG::shift_driver driver = HUB75_I2S_CFG::SHIFTREG; // HUB75_I2S_CFG::SHIFTREG;
    #elif defined(ESPS3)
    HUB75_I2S_CFG::shift_driver driver = HUB75_I2S_CFG::SHIFTREG;
    #else
    HUB75_I2S_CFG::shift_driver driver = HUB75_I2S_CFG::FM6126A;
  #endif
  // Initialize the panel.
  HUB75_I2S_CFG::i2s_pins pins = {R1,   G1,   BL1,  R2,   G2,  BL2, CH_A,
                                  CH_B, CH_C, CH_D, CH_E, LAT, OE,  CLK};

#ifdef TIDBYT_GEN2
  bool invert_clock_phase = false;
#else
  bool invert_clock_phase = true;  
#endif



  HUB75_I2S_CFG mxconfig(64,                      // width
                         32,                      // height
                         1,                       // chain length
                         pins,                    // pin mapping                         
                         driver,                  // driver chip                         
                         true,                    // double-buffering
                         HUB75_I2S_CFG::HZ_10M,   // clock speed
                         1,                       // latch blanking
                         invert_clock_phase       // invert clock phase
  );

  _matrix = new MatrixPanel_I2S_DMA(mxconfig);

  // Set brightness and clear the screen.
  _matrix->setBrightness8(DISPLAY_DEFAULT_BRIGHTNESS);
  if (!_matrix->begin()) {
    return 1;
  }
  _matrix->fillScreenRGB888(0, 0, 0);

  return 0;
}

void display_shutdown() {
  display_clear();
  _matrix->stopDMAoutput();
}

void display_draw(const uint8_t *pix, int width, int height,
		  int channels, int ixR, int ixG, int ixB) {
  for (unsigned int i = 0; i < height; i++) {
    for (unsigned int j = 0; j < width; j++) {
      const uint8_t *p = &pix[(i * width + j) * channels];
      uint8_t r = p[ixR];
      uint8_t g = p[ixG];
      uint8_t b = p[ixB];

      _matrix->drawPixelRGB888(j, i, r, g, b);
    }
  }
  _matrix->flipDMABuffer();
}

void display_clear() { _matrix->fillScreenRGB888(0, 0, 0); }
