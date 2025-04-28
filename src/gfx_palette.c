#include "gfx_palette.h"

#include <esp_log.h>
#include <math.h>

#include "dspm_mult.h"
#include "util.h"

static const char *TAG = "gfx_p";

static const float MATRIX_IDENTITY[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

static const float MATRIX_DIMMED[3][3] = {
    {0.25f, 0, 0}, {0, 0.25f, 0}, {0, 0, 0.25f}};

static const float MATRIX_NIGHT[3][3] = {{1.2066f, 0.3380f, 0.0383f},
                                         {-0.0164f, 0.8985f, 0.0098f},
                                         {-0.0156f, -0.0500f, 0.4201f}};

static const float MATRIX_COOL[3][3] = {
    {0.9f, 0.0f, 0.2f}, {0.0f, 1.0f, 0.0f}, {-0.1f, 0.0f, 1.0f}};

static const float MATRIX_WARM[3][3] = {
    {1.0f, 0.0f, -0.1f}, {0.0f, 1.0f, 0.0f}, {0.1f, 0.0f, 0.8f}};

static const float MATRIX_PASTEL[3][3] = {
    {1.2f, 0.1f, 0.1f}, {0.1f, 1.2f, 0.1f}, {0.1f, 0.1f, 1.2f}};

static const float MATRIX_MOONLIGHT[3][3] = {
    {0.6f, 0.2f, 0.4f}, {0.2f, 0.7f, 0.2f}, {0.3f, 0.3f, 0.9f}};

static const float MATRIX_DUSK[3][3] = {
    {1.1f, 0.0f, 0.2f}, {0.0f, 0.8f, 0.1f}, {0.0f, 0.1f, 0.6f}};

static const float MATRIX_VINTAGE[3][3] = {
    {1.1f, 0.3f, 0.0f}, {0.0f, 0.9f, 0.1f}, {0.0f, 0.2f, 0.5f}};

static const float MATRIX_BW[3][3] = {
    {0.3f, 0.59f, 0.11f}, {0.3f, 0.59f, 0.11f}, {0.3f, 0.59f, 0.11f}};

static const float MATRIX_SUNRISE[3][3] = {
    {1.3f, 0.2f, 0.0f}, {0.1f, 1.1f, 0.0f}, {0.0f, 0.1f, 0.6f}};

static const float MATRIX_CYBER[3][3] = {
    {1.0f, 0.0f, 1.2f}, {0.0f, 1.0f, 0.5f}, {0.2f, 0.5f, 1.2f}};

// Expand 3x3 matrix into 4x4 matrix
void matrix_expand_3x3_to_4x4(const float m3[3][3], float m4[4][4]) {
  m4[0][0] = m3[0][0];
  m4[0][1] = m3[0][1];
  m4[0][2] = m3[0][2];
  m4[0][3] = 0.0f;
  m4[1][0] = m3[1][0];
  m4[1][1] = m3[1][1];
  m4[1][2] = m3[1][2];
  m4[1][3] = 0.0f;
  m4[2][0] = m3[2][0];
  m4[2][1] = m3[2][1];
  m4[2][2] = m3[2][2];
  m4[2][3] = 0.0f;
  m4[3][0] = 0.0f;
  m4[3][1] = 0.0f;
  m4[3][2] = 0.0f;
  m4[3][3] = 1.0f;
}

const char *gfx_palette_name(gfx_palette_t mode) {
  switch (mode) {
    case PALETTE_NORMAL:
      return "NORMAL";
    case PALETTE_NIGHT:
      return "NIGHT";
    case PALETTE_DIMMED:
      return "DIMMED";
    case PALETTE_COOL:
      return "COOL";
    case PALETTE_WARM:
      return "WARM";
    case PALETTE_PASTEL:
      return "PASTEL";
    case PALETTE_MOONLIGHT:
      return "MOONLIGHT";
    case PALETTE_DUSK:
      return "DUSK";
    case PALETTE_VINTAGE:
      return "VINTAGE";
    case PALETTE_BW:
      return "BW";
    case PALETTE_SUNRISE:
      return "SUNRISE";
    case PALETTE_CYBER:
      return "CYBER";
    default:

      return "UNKNOWN";
  }
}

const float (*gfx_palette_matrix(gfx_palette_t mode))[3] {
  switch (mode) {
    case PALETTE_NORMAL:
      return MATRIX_IDENTITY;
    case PALETTE_NIGHT:
      return MATRIX_NIGHT;
    case PALETTE_DIMMED:
      return MATRIX_DIMMED;
    case PALETTE_COOL:
      return MATRIX_COOL;
    case PALETTE_WARM:
      return MATRIX_WARM;
    case PALETTE_PASTEL:
      return MATRIX_PASTEL;
    case PALETTE_MOONLIGHT:
      return MATRIX_MOONLIGHT;
    case PALETTE_DUSK:
      return MATRIX_DUSK;
    case PALETTE_VINTAGE:
      return MATRIX_VINTAGE;
    case PALETTE_BW:
      return MATRIX_BW;
    case PALETTE_SUNRISE:
      return MATRIX_SUNRISE;
    case PALETTE_CYBER:
      return MATRIX_CYBER;
    default:
      return MATRIX_IDENTITY;
  }
}

void gfx_palette_apply(uint8_t *pix, int w, int h, const float matrix[3][3]) {
  if (!pix || !matrix) {
    ESP_LOGW(TAG, "gfx_palette_apply: Invalid buffer/matrix passed");
    return;
  }
  size_t npix = (size_t)w * (size_t)h;
  const float *m = (const float *)matrix;  // Flatten matrix pointer for fast
  // const float *m = __builtin_assume_aligned(matrix, 4);

  // access
  uint8_t *p = pix;
  for (size_t i = 0; i < npix; i++, p += 4) {
    float r = p[0];
    float g = p[1];
    float b = p[2];
    // Skip alpha

    float nr = m[0] * r + m[1] * g + m[2] * b;  // row 0: m[0],m[1],m[2]
    float ng = m[3] * r + m[4] * g + m[5] * b;  // row 1: m[3],m[4],m[5]
    float nb = m[6] * r + m[7] * g + m[8] * b;  // row 2: m[6],m[7],m[8]
    // Skip alpha

    // clamp to [0,255]
    p[0] = (uint8_t)MIN(MAX(nr, 0.0f), 255.0f);
    p[1] = (uint8_t)MIN(MAX(ng, 0.0f), 255.0f);
    p[2] = (uint8_t)MIN(MAX(nb, 0.0f), 255.0f);
  }
}

// Apply palette transformation to whole frame row by row
// void gfx_palette_apply_frame(uint8_t *pix, int width, int height,
//                              const float matrix3x3[3][3]) {
//   const int channels = 4;  // RGBA
//   int rowStride = width * channels;

//   float matrix4x4[4][4];
//   matrix_expand_3x3_to_4x4(matrix3x3, matrix4x4);

//   for (int y = 0; y < height; y++) {
//     uint8_t *row = pix + y * rowStride;
//     for (int x = 0; x < width; x++) {
//       float in[4] = {
//           row[x * channels + 0],  // R
//           row[x * channels + 1],  // G
//           row[x * channels + 2],  // B
//           row[x * channels + 3],  // A
//       };
//       float out[4];

//       dspm_mult_4x4x1_f32((float *)matrix4x4, in, out);

//       row[x * channels + 0] = (out[0] > 255.0f) ? 255
//                               : (out[0] < 0.0f) ? 0
//                                                 : (uint8_t)out[0];
//       row[x * channels + 1] = (out[1] > 255.0f) ? 255
//                               : (out[1] < 0.0f) ? 0
//                                                 : (uint8_t)out[1];
//       row[x * channels + 2] = (out[2] > 255.0f) ? 255
//                               : (out[2] < 0.0f) ? 0
//                                                 : (uint8_t)out[2];
//       row[x * channels + 3] = (out[3] > 255.0f) ? 255
//                               : (out[3] < 0.0f) ? 0
//                                                 : (uint8_t)out[3];
//     }
//   }
// }

void gfx_palette_apply_frame_rbg(uint8_t *pix, int w, int h,
                                 const float matrix[3][3]) {
  int npix = w * h;
  for (int i = 0; i < npix; i++) {
    uint8_t *p = pix + 4 * i;
    float in[3] = {p[0], p[1], p[2]};
    float out[3];

    dspm_mult_3x3x1_f32((float *)matrix, in, out);

    p[0] = (out[0] > 255.0f) ? 255 : (out[0] < 0.0f) ? 0 : (uint8_t)out[0];
    p[1] = (out[1] > 255.0f) ? 255 : (out[1] < 0.0f) ? 0 : (uint8_t)out[1];
    p[2] = (out[2] > 255.0f) ? 255 : (out[2] < 0.0f) ? 0 : (uint8_t)out[2];
  }
}

void gfx_palette_apply_frame(uint8_t *pix, int w, int h,
                             const float matrix3x3[3][3]) {
  const int channels = 4;  // RGBA
  const int npix = w * h;

  // Allocate temp float arrays (size 3 x npix)
  float *input = (float *)malloc(sizeof(float) * 3 * npix);
  float *output = (float *)malloc(sizeof(float) * 3 * npix);

  if (!input || !output) {
    ESP_LOGE("gfx", "Failed to allocate memory for batch palette apply!");
    if (input) free(input);
    if (output) free(output);
    return;
  }

  // Load pixel data into input buffer
  for (int i = 0; i < npix; i++) {
    input[i + 0 * npix] = pix[i * channels + 0];  // R
    input[i + 1 * npix] = pix[i * channels + 1];  // G
    input[i + 2 * npix] = pix[i * channels + 2];  // B
                                                  // Skip Alpha (leave it as is)
  }

  // Single DSP matrix multiply: (3x3) × (3 x npix)
  dspm_mult_f32((float *)matrix3x3, input, output, 3, 3, npix);

  // Write results back to pixel buffer
  for (int i = 0; i < npix; i++) {
    pix[i * channels + 0] = (output[i + 0 * npix] > 255.0f) ? 255
                            : (output[i + 0 * npix] < 0.0f)
                                ? 0
                                : (uint8_t)output[i + 0 * npix];
    pix[i * channels + 1] = (output[i + 1 * npix] > 255.0f) ? 255
                            : (output[i + 1 * npix] < 0.0f)
                                ? 0
                                : (uint8_t)output[i + 1 * npix];
    pix[i * channels + 2] = (output[i + 2 * npix] > 255.0f) ? 255
                            : (output[i + 2 * npix] < 0.0f)
                                ? 0
                                : (uint8_t)output[i + 2 * npix];
    // Alpha untouched
  }

  free(input);
  free(output);
}

// static void apply_night_matrix(uint8_t *pix, int w, int h) {
//   // const float M[3][3] = {
//   //   {1.2f, 0.0f, 0.0f},
//   //   {0.0f, 1.0f, 0.0f},
//   //   {0.0f, 0.0f, 0.3f}
//   // }; // Hue shift to more sepiaisque tones

//   // Or colour shift D65→3400K Bradford chromatic‐adaptation matrix
//   const float M[3][3] = {{1.2066f, 0.3380f, 0.0383f},
//                          {-0.0164f, 0.8985f, 0.0098f},
//                          {-0.0156f, -0.0500f, 0.4201f}};
//   int npix = w * h;
//   for (int i = 0; i < npix; i++) {
//     uint8_t *p = pix + 4 * i;
//     float r = p[0], g = p[1], b = p[2];  // a = p[3];
//     // Don't process alpha..
//     float nr = M[0][0] * r + M[0][1] * g + M[0][2] * b;
//     float ng = M[1][0] * r + M[1][1] * g + M[1][2] * b;
//     float nb = M[2][0] * r + M[2][1] * g + M[2][2] * b;
//     // Clamp back if our math got funky
//     p[0] = (nr > 255.0f) ? 255 : (uint8_t)nr;
//     p[1] = (ng > 255.0f) ? 255 : (uint8_t)ng;
//     p[2] = (nb > 255.0f) ? 255 : (uint8_t)nb;
//   }
// }

// void gfx_palette_apply_frame_rbg(uint8_t *pix, int width, int height,
//                                  const float matrix3x3[3][3]) {
//   const int channels = 4;
//   int rowStride = width * channels;

//   float input[3][width];
//   float output[3][width];

//   for (int y = 0; y < height; y++) {
//     uint8_t *row = pix + y * rowStride;

//     for (int x = 0; x < width; x++) {
//       input[0][x] = row[x * channels + 0];  // R
//       input[1][x] = row[x * channels + 1];  // G
//       input[2][x] = row[x * channels + 2];  // B
//                                             // Skip Alpha
//     }

//     // Multiply: 3x3 matrix * 3xwidth vector

//     dspm_mult_f32((float *)matrix3x3, (float *)input, (float *)output, 3,
//     3,
//                   width);

//     for (int x = 0; x < width; x++) {
//       row[x * channels + 0] = (output[0][x] > 255.0f) ? 255
//                               : (output[0][x] < 0.0f) ? 0
//                                                       :
//                                                       (uint8_t)output[0][x];
//       row[x * channels + 1] = (output[1][x] > 255.0f) ? 255
//                               : (output[1][x] < 0.0f) ? 0
//                                                       :
//                                                       (uint8_t)output[1][x];
//       row[x * channels + 2] = (output[2][x] > 255.0f) ? 255
//                               : (output[2][x] < 0.0f) ? 0
//                                                       :
//                                                       (uint8_t)output[2][x];
//       // Alpha unchanged
//       // row[x * channels + 3] stays the same
//     }
//   }
// }