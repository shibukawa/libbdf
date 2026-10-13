/*
 * VP8 key frames, the lossy format of WebP: a port of Go's
 * golang.org/x/image/vp8 (BSD license, see third_party/go-image/LICENSE),
 * operation for operation, so that a frame decodes to the bytes the Go
 * decoder gives and the frames it refuses are refused. The specification of
 * VP8 is RFC 6386; where the Go code departs from it, this follows the Go
 * code.
 *
 * Go's integers wrap and its signed shifts are arithmetic. Where a value can
 * leave the range of its Go type (the products of the inverse DCT, the
 * coefficients stored as int16, the filter levels kept as int8) the helpers
 * below do what Go does without relying on what C leaves to the compiler.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img_webp_internal.h"

/* ---- Go's integer conversions and shifts ---------------------------------- */

/* int32(v) of a uint32. */
static int32_t bdfi_vp8_i32(uint32_t v) {
  return v > 0x7fffffffu ? -(int32_t)(~v) - 1 : (int32_t)v;
}

/* int16(c) of an int32: the low 16 bits. */
static int16_t bdfi_vp8_i16(int32_t c) {
  uint32_t v = (uint32_t)c & 0xffffu;
  return (int16_t)(v > 0x7fffu ? (int32_t)v - 0x10000 : (int32_t)v);
}

/* int8(c) of an int32: the low 8 bits. */
static int32_t bdfi_vp8_i8(int32_t c) {
  uint32_t v = (uint32_t)c & 0xffu;
  return v > 0x7fu ? (int32_t)v - 0x100 : (int32_t)v;
}

/* x >> n of a signed x. */
static int32_t bdfi_vp8_sar(int32_t x, int n) {
  return x < 0 ? ~(~x >> n) : x >> n;
}

/* a * b in int32, wrapping. */
static int32_t bdfi_vp8_mul(int32_t a, int32_t b) {
  return bdfi_vp8_i32((uint32_t)a * (uint32_t)b);
}

/* clip in vp8/quant.go */
static int32_t bdfi_vp8_clip(int32_t x, int32_t lo, int32_t hi) {
  if (x < lo) {
    return lo;
  }
  if (x > hi) {
    return hi;
  }
  return x;
}

/* clip8 in vp8/idct.go and clamp255 in vp8/filter.go */
static uint8_t bdfi_vp8_clip8(int32_t i) {
  if (i < 0) {
    return 0;
  }
  if (i > 255) {
    return 255;
  }
  return (uint8_t)i;
}

/* ---- The tables of the specification -------------------------------------- */

/* lutShift and lutRangeM1 in vp8/partition.go */
static const uint8_t bdfi_vp8_lut_shift[127] = {
    7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const uint8_t bdfi_vp8_lut_range_m1[127] = {
    127, 127, 191, 127, 159, 191, 223, 127, 143, 159, 175, 191, 207, 223, 239, 127, 135, 143, 151,
    159, 167, 175, 183, 191, 199, 207, 215, 223, 231, 239, 247, 127, 131, 135, 139, 143, 147, 151,
    155, 159, 163, 167, 171, 175, 179, 183, 187, 191, 195, 199, 203, 207, 211, 215, 219, 223, 227,
    231, 235, 239, 243, 247, 251, 127, 129, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149, 151,
    153, 155, 157, 159, 161, 163, 165, 167, 169, 171, 173, 175, 177, 179, 181, 183, 185, 187, 189,
    191, 193, 195, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221, 223, 225, 227,
    229, 231, 233, 235, 237, 239, 241, 243, 245, 247, 249, 251, 253};

/* uniformProb: a bit that is 0 half of the time. */
#define BDFI_VP8_UNIFORM_PROB 128

/* The dequantization tables of section 14.1 (vp8/quant.go). */
static const uint16_t bdfi_vp8_dequant_dc[128] = {
    4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,  17,  18,  19,  20,
    20,  21,  21,  22,  22,  23,  23,  24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
    35,  36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
    71,  72,  73,  74,  75,  76,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,
    89,  91,  93,  95,  96,  98,  100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 122, 124,
    126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157};

static const uint16_t bdfi_vp8_dequant_ac[128] = {
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  62,
    64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  86,  88,  90,  92,  94,  96,  98,  100,
    102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149,
    152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209, 213, 217,
    221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284};

#define BDFI_VP8_N_SEGMENT 4
#define BDFI_VP8_N_PLANE   4
#define BDFI_VP8_N_BAND    8
#define BDFI_VP8_N_CONTEXT 3
#define BDFI_VP8_N_PROB    11

/* The planes of section 13.3 (vp8/token.go). */
#define BDFI_VP8_PLANE_Y1_WITH_Y2 0
#define BDFI_VP8_PLANE_Y2         1
#define BDFI_VP8_PLANE_UV         2
#define BDFI_VP8_PLANE_Y1_SANS_Y2 3

/* tokenProbUpdateProb in vp8/token.go (section 13.4): by plane, band,
 * context and probability, as defaultTokenProb is */
static const uint8_t bdfi_vp8_token_prob_update_prob[4][8][3][11] = {
    {{{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255},
      {249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255},
      {234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255},
      {250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
    {{{217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255},
      {234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255}},
     {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
    {{{186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255},
      {234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255},
      {251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255}},
     {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
    {{{248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255},
      {248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255},
      {248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}}};

/* defaultTokenProb in vp8/token.go (section 13.5) */
static const uint8_t bdfi_vp8_default_token_prob[4][8][3][11] = {
    {{{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128},
      {189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128},
      {106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128}},
     {{1, 98, 248, 255, 236, 226, 255, 255, 128, 128, 128},
      {181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128},
      {78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128}},
     {{1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128},
      {184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128},
      {77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128}},
     {{1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128},
      {170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128},
      {37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128}},
     {{1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128},
      {207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128},
      {102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128}},
     {{1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128},
      {177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128},
      {80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128}},
     {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {246, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}},
    {{{198, 35, 237, 223, 193, 187, 162, 160, 145, 155, 62},
      {131, 45, 198, 221, 172, 176, 220, 157, 252, 221, 1},
      {68, 47, 146, 208, 149, 167, 221, 162, 255, 223, 128}},
     {{1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128},
      {184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128},
      {81, 99, 181, 242, 176, 190, 249, 202, 255, 255, 128}},
     {{1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128},
      {99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128},
      {23, 91, 163, 242, 170, 187, 247, 210, 255, 255, 128}},
     {{1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128},
      {109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128},
      {44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128}},
     {{1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128},
      {94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128},
      {22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128}},
     {{1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128},
      {124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128},
      {35, 77, 181, 251, 193, 211, 255, 205, 128, 128, 128}},
     {{1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128},
      {121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128},
      {45, 99, 188, 251, 195, 217, 255, 224, 128, 128, 128}},
     {{1, 1, 251, 255, 213, 255, 128, 128, 128, 128, 128},
      {203, 1, 248, 255, 255, 128, 128, 128, 128, 128, 128},
      {137, 1, 177, 255, 224, 255, 128, 128, 128, 128, 128}}},
    {{{253, 9, 248, 251, 207, 208, 255, 192, 128, 128, 128},
      {175, 13, 224, 243, 193, 185, 249, 198, 255, 255, 128},
      {73, 17, 171, 221, 161, 179, 236, 167, 255, 234, 128}},
     {{1, 95, 247, 253, 212, 183, 255, 255, 128, 128, 128},
      {239, 90, 244, 250, 211, 209, 255, 255, 128, 128, 128},
      {155, 77, 195, 248, 188, 195, 255, 255, 128, 128, 128}},
     {{1, 24, 239, 251, 218, 219, 255, 205, 128, 128, 128},
      {201, 51, 219, 255, 196, 186, 128, 128, 128, 128, 128},
      {69, 46, 190, 239, 201, 218, 255, 228, 128, 128, 128}},
     {{1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128},
      {223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128},
      {141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128}},
     {{1, 16, 248, 255, 255, 128, 128, 128, 128, 128, 128},
      {190, 36, 230, 255, 236, 255, 128, 128, 128, 128, 128},
      {149, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128},
      {213, 62, 250, 255, 255, 128, 128, 128, 128, 128, 128},
      {55, 93, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}},
    {{{202, 24, 213, 235, 186, 191, 220, 160, 240, 175, 255},
      {126, 38, 182, 232, 169, 184, 228, 174, 255, 187, 128},
      {61, 46, 138, 219, 151, 178, 240, 170, 255, 216, 128}},
     {{1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128},
      {166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128},
      {39, 77, 162, 232, 172, 180, 245, 178, 255, 255, 128}},
     {{1, 52, 220, 246, 198, 199, 249, 220, 255, 255, 128},
      {124, 74, 191, 243, 183, 193, 250, 221, 255, 255, 128},
      {24, 71, 130, 219, 154, 170, 243, 182, 255, 255, 128}},
     {{1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128},
      {149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128},
      {28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128}},
     {{1, 81, 230, 252, 204, 203, 255, 192, 128, 128, 128},
      {123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128},
      {20, 95, 153, 243, 164, 173, 255, 203, 128, 128, 128}},
     {{1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128},
      {168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128},
      {47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128}},
     {{1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128},
      {141, 84, 213, 252, 201, 202, 255, 219, 128, 128, 128},
      {42, 80, 160, 240, 162, 185, 255, 205, 128, 128, 128}},
     {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {244, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {238, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}}}};

/* The predictor modes (vp8/predfunc.go). */
#define BDFI_VP8_N_PRED           10
#define BDFI_VP8_PRED_DC          0
#define BDFI_VP8_PRED_TM          1
#define BDFI_VP8_PRED_VE          2
#define BDFI_VP8_PRED_HE          3
#define BDFI_VP8_PRED_RD          4
#define BDFI_VP8_PRED_VR          5
#define BDFI_VP8_PRED_LD          6
#define BDFI_VP8_PRED_VL          7
#define BDFI_VP8_PRED_HD          8
#define BDFI_VP8_PRED_HU          9
#define BDFI_VP8_PRED_DC_TOP      10
#define BDFI_VP8_PRED_DC_LEFT     11
#define BDFI_VP8_PRED_DC_TOP_LEFT 12

/* predProb in vp8/pred.go (section 11.5) */
static const uint8_t bdfi_vp8_pred_prob[10][10][9] = {{{231, 120, 48, 89, 115, 113, 120, 152, 112},
                                                       {152, 179, 64, 126, 170, 118, 46, 70, 95},
                                                       {175, 69, 143, 80, 85, 82, 72, 155, 103},
                                                       {56, 58, 10, 171, 218, 189, 17, 13, 152},
                                                       {114, 26, 17, 163, 44, 195, 21, 10, 173},
                                                       {121, 24, 80, 195, 26, 62, 44, 64, 85},
                                                       {144, 71, 10, 38, 171, 213, 144, 34, 26},
                                                       {170, 46, 55, 19, 136, 160, 33, 206, 71},
                                                       {63, 20, 8, 114, 114, 208, 12, 9, 226},
                                                       {81, 40, 11, 96, 182, 84, 29, 16, 36}},
                                                      {{134, 183, 89, 137, 98, 101, 106, 165, 148},
                                                       {72, 187, 100, 130, 157, 111, 32, 75, 80},
                                                       {66, 102, 167, 99, 74, 62, 40, 234, 128},
                                                       {41, 53, 9, 178, 241, 141, 26, 8, 107},
                                                       {74, 43, 26, 146, 73, 166, 49, 23, 157},
                                                       {65, 38, 105, 160, 51, 52, 31, 115, 128},
                                                       {104, 79, 12, 27, 217, 255, 87, 17, 7},
                                                       {87, 68, 71, 44, 114, 51, 15, 186, 23},
                                                       {47, 41, 14, 110, 182, 183, 21, 17, 194},
                                                       {66, 45, 25, 102, 197, 189, 23, 18, 22}},
                                                      {{88, 88, 147, 150, 42, 46, 45, 196, 205},
                                                       {43, 97, 183, 117, 85, 38, 35, 179, 61},
                                                       {39, 53, 200, 87, 26, 21, 43, 232, 171},
                                                       {56, 34, 51, 104, 114, 102, 29, 93, 77},
                                                       {39, 28, 85, 171, 58, 165, 90, 98, 64},
                                                       {34, 22, 116, 206, 23, 34, 43, 166, 73},
                                                       {107, 54, 32, 26, 51, 1, 81, 43, 31},
                                                       {68, 25, 106, 22, 64, 171, 36, 225, 114},
                                                       {34, 19, 21, 102, 132, 188, 16, 76, 124},
                                                       {62, 18, 78, 95, 85, 57, 50, 48, 51}},
                                                      {{193, 101, 35, 159, 215, 111, 89, 46, 111},
                                                       {60, 148, 31, 172, 219, 228, 21, 18, 111},
                                                       {112, 113, 77, 85, 179, 255, 38, 120, 114},
                                                       {40, 42, 1, 196, 245, 209, 10, 25, 109},
                                                       {88, 43, 29, 140, 166, 213, 37, 43, 154},
                                                       {61, 63, 30, 155, 67, 45, 68, 1, 209},
                                                       {100, 80, 8, 43, 154, 1, 51, 26, 71},
                                                       {142, 78, 78, 16, 255, 128, 34, 197, 171},
                                                       {41, 40, 5, 102, 211, 183, 4, 1, 221},
                                                       {51, 50, 17, 168, 209, 192, 23, 25, 82}},
                                                      {{138, 31, 36, 171, 27, 166, 38, 44, 229},
                                                       {67, 87, 58, 169, 82, 115, 26, 59, 179},
                                                       {63, 59, 90, 180, 59, 166, 93, 73, 154},
                                                       {40, 40, 21, 116, 143, 209, 34, 39, 175},
                                                       {47, 15, 16, 183, 34, 223, 49, 45, 183},
                                                       {46, 17, 33, 183, 6, 98, 15, 32, 183},
                                                       {57, 46, 22, 24, 128, 1, 54, 17, 37},
                                                       {65, 32, 73, 115, 28, 128, 23, 128, 205},
                                                       {40, 3, 9, 115, 51, 192, 18, 6, 223},
                                                       {87, 37, 9, 115, 59, 77, 64, 21, 47}},
                                                      {{104, 55, 44, 218, 9, 54, 53, 130, 226},
                                                       {64, 90, 70, 205, 40, 41, 23, 26, 57},
                                                       {54, 57, 112, 184, 5, 41, 38, 166, 213},
                                                       {30, 34, 26, 133, 152, 116, 10, 32, 134},
                                                       {39, 19, 53, 221, 26, 114, 32, 73, 255},
                                                       {31, 9, 65, 234, 2, 15, 1, 118, 73},
                                                       {75, 32, 12, 51, 192, 255, 160, 43, 51},
                                                       {88, 31, 35, 67, 102, 85, 55, 186, 85},
                                                       {56, 21, 23, 111, 59, 205, 45, 37, 192},
                                                       {55, 38, 70, 124, 73, 102, 1, 34, 98}},
                                                      {{125, 98, 42, 88, 104, 85, 117, 175, 82},
                                                       {95, 84, 53, 89, 128, 100, 113, 101, 45},
                                                       {75, 79, 123, 47, 51, 128, 81, 171, 1},
                                                       {57, 17, 5, 71, 102, 57, 53, 41, 49},
                                                       {38, 33, 13, 121, 57, 73, 26, 1, 85},
                                                       {41, 10, 67, 138, 77, 110, 90, 47, 114},
                                                       {115, 21, 2, 10, 102, 255, 166, 23, 6},
                                                       {101, 29, 16, 10, 85, 128, 101, 196, 26},
                                                       {57, 18, 10, 102, 102, 213, 34, 20, 43},
                                                       {117, 20, 15, 36, 163, 128, 68, 1, 26}},
                                                      {{102, 61, 71, 37, 34, 53, 31, 243, 192},
                                                       {69, 60, 71, 38, 73, 119, 28, 222, 37},
                                                       {68, 45, 128, 34, 1, 47, 11, 245, 171},
                                                       {62, 17, 19, 70, 146, 85, 55, 62, 70},
                                                       {37, 43, 37, 154, 100, 163, 85, 160, 1},
                                                       {63, 9, 92, 136, 28, 64, 32, 201, 85},
                                                       {75, 15, 9, 9, 64, 255, 184, 119, 16},
                                                       {86, 6, 28, 5, 64, 255, 25, 248, 1},
                                                       {56, 8, 17, 132, 137, 255, 55, 116, 128},
                                                       {58, 15, 20, 82, 135, 57, 26, 121, 40}},
                                                      {{164, 50, 31, 137, 154, 133, 25, 35, 218},
                                                       {51, 103, 44, 131, 131, 123, 31, 6, 158},
                                                       {86, 40, 64, 135, 148, 224, 45, 183, 128},
                                                       {22, 26, 17, 131, 240, 154, 14, 1, 209},
                                                       {45, 16, 21, 91, 64, 222, 7, 1, 197},
                                                       {56, 21, 39, 155, 60, 138, 23, 102, 213},
                                                       {83, 12, 13, 54, 192, 255, 68, 47, 28},
                                                       {85, 26, 85, 85, 128, 128, 32, 146, 171},
                                                       {18, 11, 7, 63, 144, 171, 4, 4, 246},
                                                       {35, 27, 10, 146, 174, 171, 12, 26, 128}},
                                                      {{190, 80, 35, 99, 180, 80, 126, 54, 45},
                                                       {85, 126, 47, 87, 176, 51, 41, 20, 32},
                                                       {101, 75, 128, 139, 118, 146, 116, 128, 85},
                                                       {56, 41, 15, 176, 236, 85, 37, 9, 62},
                                                       {71, 30, 17, 119, 118, 255, 17, 18, 138},
                                                       {101, 38, 60, 138, 55, 70, 43, 26, 142},
                                                       {146, 36, 19, 30, 171, 255, 97, 27, 20},
                                                       {138, 45, 61, 62, 219, 1, 81, 188, 64},
                                                       {32, 41, 20, 117, 151, 142, 20, 21, 163},
                                                       {112, 19, 12, 61, 195, 128, 48, 4, 24}}};

/* bands, cat3456 and zigzag in vp8/reconstruct.go */
static const uint8_t bdfi_vp8_bands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};

static const uint8_t bdfi_vp8_cat3456[4][12] = {
    {173, 148, 140, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {176, 155, 140, 135, 0, 0, 0, 0, 0, 0, 0, 0},
    {180, 157, 141, 134, 130, 0, 0, 0, 0, 0, 0, 0},
    {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0}};

static const uint8_t bdfi_vp8_zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

/* ---- A partition: arithmetic-coded bits (vp8/partition.go) ---------------- */

typedef struct bdfi_vp8_partition {
  const uint8_t *buf;
  size_t len, r;
  uint32_t range_m1; /* the range of the arithmetic coder, minus 1 */
  uint32_t bits, nbits;
  int eof; /* unexpectedEOF: a bit was asked for past the end */
} bdfi_vp8_partition;

/* partition.init */
static void bdfi_vp8_partition_init(bdfi_vp8_partition *p, const uint8_t *buf, size_t len) {
  p->buf = buf;
  p->len = len;
  p->r = 0;
  p->range_m1 = 254;
  p->bits = 0;
  p->nbits = 0;
  p->eof = 0;
}

/* partition.readBit: past the end the bit is 0, nothing moves and eof is set. */
static int bdfi_vp8_read_bit(bdfi_vp8_partition *p, uint32_t prob) {
  if (p->nbits < 8) {
    if (p->r >= p->len) {
      p->eof = 1;
      return 0;
    }
    p->bits |= (uint32_t)p->buf[p->r] << (8 - p->nbits);
    p->r++;
    p->nbits += 8;
  }
  uint32_t split = ((p->range_m1 * prob) >> 8) + 1;
  int bit = p->bits >= (split << 8);
  if (bit) {
    p->range_m1 -= split;
    p->bits -= split << 8;
  } else {
    p->range_m1 = split - 1;
  }
  if (p->range_m1 < 127) {
    uint32_t shift = bdfi_vp8_lut_shift[p->range_m1];
    p->range_m1 = bdfi_vp8_lut_range_m1[p->range_m1];
    p->bits <<= shift;
    p->nbits -= shift;
  }
  return bit;
}

/* partition.readUint */
static uint32_t bdfi_vp8_read_uint(bdfi_vp8_partition *p, uint32_t prob, uint32_t n) {
  uint32_t u = 0;
  while (n > 0) {
    n--;
    if (bdfi_vp8_read_bit(p, prob)) {
      u |= (uint32_t)1 << n;
    }
  }
  return u;
}

/* partition.readInt */
static int32_t bdfi_vp8_read_int(bdfi_vp8_partition *p, uint32_t prob, uint32_t n) {
  uint32_t u = bdfi_vp8_read_uint(p, prob, n);
  int b = bdfi_vp8_read_bit(p, prob);
  return b ? -(int32_t)u : (int32_t)u;
}

/* partition.readOptionalInt */
static int32_t bdfi_vp8_read_optional_int(bdfi_vp8_partition *p, uint32_t prob, uint32_t n) {
  if (!bdfi_vp8_read_bit(p, prob)) {
    return 0;
  }
  return bdfi_vp8_read_int(p, prob, n);
}

/* ---- The decoder (vp8/decode.go) ------------------------------------------ */

/* mb: what a macroblock leaves for the one to its right and the one below. */
typedef struct bdfi_vp8_mb {
  uint8_t pred[4]; /* the predictor modes of its bottom or right 4x4 luma regions */
  uint8_t nz_mask; /* which of its bottom or right regions have coefficients: 4 luma, 2+2 chroma */
  uint8_t nz_y16;  /* 1 if it used Y16 prediction and had coefficients */
} bdfi_vp8_mb;

/* quant: the DC and AC factors of a segment. */
typedef struct bdfi_vp8_quant {
  uint16_t y1[2], y2[2], uv[2];
} bdfi_vp8_quant;

/* filterParam: the loop filter's thresholds for a macroblock. */
typedef struct bdfi_vp8_filter_param {
  uint8_t level, ilevel, hlevel;
  uint8_t inner; /* the inner edges are filtered too */
} bdfi_vp8_filter_param;

/* The workspace of a macroblock (the layout is drawn in vp8/reconstruct.go):
 * 16x16 luma at (1, 8), 8x8 blue at (18, 8) and red at (18, 24), each with
 * the row above and the column to the left. */
#define BDFI_VP8_YBR_YX 8
#define BDFI_VP8_YBR_YY 1
#define BDFI_VP8_YBR_BX 8
#define BDFI_VP8_YBR_BY 18
#define BDFI_VP8_YBR_RX 24
#define BDFI_VP8_YBR_RY 18

/* Where the coefficients of the planes start in coeff. */
#define BDFI_VP8_B_COEFF_BASE   256
#define BDFI_VP8_R_COEFF_BASE   320
#define BDFI_VP8_WHT_COEFF_BASE 384

/* limitReader: the bytes of the chunk that are not read yet, how many of
 * them the file has (avail) and how many the chunk states (n). */
typedef struct bdfi_vp8_reader {
  const uint8_t *p;
  size_t avail;
  int64_t n;
} bdfi_vp8_reader;

/* FrameHeader (section 9.1), what of it is used. */
typedef struct bdfi_vp8_fh {
  int key_frame;
  uint32_t first_partition_len;
  int32_t width, height;
} bdfi_vp8_fh;

typedef struct bdfi_vp8_dec {
  bdfi_vp8_reader r;
  bdfi_vp8_fh fh;
  int32_t mbw, mbh; /* the size in macroblocks */
  /* segmentHeader; the quantizers and strengths are int8 in Go */
  int use_segment, update_map, relative_delta;
  int32_t seg_quantizer[BDFI_VP8_N_SEGMENT];
  int32_t seg_filter_strength[BDFI_VP8_N_SEGMENT];
  uint8_t seg_prob[3];
  /* filterHeader; the level and the deltas are int8 in Go */
  int filter_simple, use_lf_delta;
  int32_t filter_level;
  uint32_t sharpness;
  int32_t ref_lf_delta[4], mode_lf_delta[4];
  /* the first partition and the up to 8 others, of coefficients */
  bdfi_vp8_partition fp;
  bdfi_vp8_partition op[8];
  int nop;
  bdfi_vp8_quant quant[BDFI_VP8_N_SEGMENT];
  uint8_t token_prob[BDFI_VP8_N_PLANE][BDFI_VP8_N_BAND][BDFI_VP8_N_CONTEXT][BDFI_VP8_N_PROB];
  int use_skip_prob;
  uint32_t skip_prob;
  bdfi_vp8_filter_param filter_params[BDFI_VP8_N_SEGMENT][2];
  bdfi_vp8_filter_param *per_mb_filter_params; /* mbw*mbh */
  /* the macroblock being decoded */
  int segment;
  bdfi_vp8_mb left_mb;
  bdfi_vp8_mb *up_mb; /* mbw */
  uint32_t nz_dc_mask, nz_ac_mask;
  int use_pred_y16;
  uint8_t pred_y16, pred_c8;
  uint8_t pred_y4[4][4];
  int16_t coeff[1 * 16 * 16 + 2 * 8 * 8 + 1 * 4 * 4];
  uint8_t ybr[1 + 16 + 1 + 8][32];
  /* the image: whole macroblocks */
  uint8_t *img_y, *img_cb, *img_cr;
  size_t y_stride, c_stride;
} bdfi_vp8_dec;

/* limitReader.ReadFull: the next k bytes of the chunk, or NULL where the
 * chunk or the file has fewer. */
static const uint8_t *bdfi_vp8_read_full(bdfi_vp8_reader *r, int64_t k) {
  if (k > r->n || (uint64_t)k > (uint64_t)r->avail) {
    return NULL;
  }
  const uint8_t *p = r->p;
  r->p += (size_t)k;
  r->avail -= (size_t)k;
  r->n -= k;
  return p;
}

/* Decoder.DecodeFrameHeader, what it reads: 0 where it returns an error. */
static int bdfi_vp8_read_frame_header(bdfi_vp8_reader *r, bdfi_vp8_fh *fh) {
  memset(fh, 0, sizeof *fh);
  /* All frame headers are at least 3 bytes long. */
  const uint8_t *b = bdfi_vp8_read_full(r, 3);
  if (!b) {
    return 0;
  }
  fh->key_frame = (b[0] & 1) == 0;
  fh->first_partition_len = (uint32_t)b[0] >> 5 | (uint32_t)b[1] << 3 | (uint32_t)b[2] << 11;
  if (!fh->key_frame) {
    /* Go returns the header without an error, with the size of a decoder
     * that has seen no key frame: 0 x 0. */
    return 1;
  }
  /* Frame headers for key frames are an additional 7 bytes long. */
  b = bdfi_vp8_read_full(r, 7);
  if (!b) {
    return 0;
  }
  /* Check the magic sync code. */
  if (b[0] != 0x9d || b[1] != 0x01 || b[2] != 0x2a) {
    return 0;
  }
  fh->width = (int32_t)(b[4] & 0x3f) << 8 | (int32_t)b[3];
  fh->height = (int32_t)(b[6] & 0x3f) << 8 | (int32_t)b[5];
  return 1;
}

/* Decoder.parseSegmentHeader (section 9.3) */
static void bdfi_vp8_parse_segment_header(bdfi_vp8_dec *d) {
  d->use_segment = bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  if (!d->use_segment) {
    d->update_map = 0;
    return;
  }
  d->update_map = bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  if (bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB)) {
    d->relative_delta = !bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
    for (int i = 0; i < BDFI_VP8_N_SEGMENT; i++) {
      d->seg_quantizer[i] = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 7);
    }
    for (int i = 0; i < BDFI_VP8_N_SEGMENT; i++) {
      d->seg_filter_strength[i] = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 6);
    }
  }
  if (!d->update_map) {
    return;
  }
  for (int i = 0; i < 3; i++) {
    if (bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB)) {
      d->seg_prob[i] = (uint8_t)bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 8);
    } else {
      d->seg_prob[i] = 0xff;
    }
  }
}

/* Decoder.computeFilterParams in vp8/filter.go (section 15.4). The levels
 * are int8 in Go and their sums wrap: a level that passes 127 is negative
 * and turns the filter off. */
static void bdfi_vp8_compute_filter_params(bdfi_vp8_dec *d) {
  for (int i = 0; i < BDFI_VP8_N_SEGMENT; i++) {
    int32_t base_level = d->filter_level;
    if (d->use_segment) {
      base_level = d->seg_filter_strength[i];
      if (d->relative_delta) {
        base_level = bdfi_vp8_i8(base_level + d->filter_level);
      }
    }
    for (int j = 0; j < 2; j++) {
      bdfi_vp8_filter_param *p = &d->filter_params[i][j];
      p->inner = (uint8_t)(j != 0);
      int32_t level = base_level;
      if (d->use_lf_delta) {
        level = bdfi_vp8_i8(level + d->ref_lf_delta[0]);
        if (j != 0) {
          level = bdfi_vp8_i8(level + d->mode_lf_delta[0]);
        }
      }
      if (level <= 0) {
        p->level = 0;
        continue;
      }
      if (level > 63) {
        level = 63;
      }
      int32_t ilevel = level;
      if (d->sharpness > 0) {
        if (d->sharpness > 4) {
          ilevel >>= 2;
        } else {
          ilevel >>= 1;
        }
        int32_t x = 9 - (int32_t)d->sharpness;
        if (ilevel > x) {
          ilevel = x;
        }
      }
      if (ilevel < 1) {
        ilevel = 1;
      }
      p->ilevel = (uint8_t)ilevel;
      p->level = (uint8_t)(2 * level + ilevel);
      /* the thresholds of a key frame: no other frame gets here */
      if (level < 15) {
        p->hlevel = 0;
      } else if (level < 40) {
        p->hlevel = 1;
      } else {
        p->hlevel = 2;
      }
    }
  }
}

/* Decoder.parseFilterHeader (section 9.4). The per-segment levels Go keeps
 * in the header are not used by anything. */
static void bdfi_vp8_parse_filter_header(bdfi_vp8_dec *d) {
  d->filter_simple = bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  d->filter_level = (int32_t)bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 6);
  d->sharpness = bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 3);
  d->use_lf_delta = bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  if (d->use_lf_delta && bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB)) {
    for (int i = 0; i < 4; i++) {
      d->ref_lf_delta[i] = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 6);
    }
    for (int i = 0; i < 4; i++) {
      d->mode_lf_delta[i] = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 6);
    }
  }
  if (d->filter_level == 0) {
    return;
  }
  bdfi_vp8_compute_filter_params(d);
}

/* Decoder.parseOtherPartitions (section 9.5) */
static int bdfi_vp8_parse_other_partitions(bdfi_vp8_dec *d) {
  int64_t part_lens[8];
  for (int i = 0; i < 8; i++) {
    part_lens[i] = 0;
  }
  d->nop = 1 << bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 2);

  /* The length of the last partition is what remains of the chunk (by its
   * stated length) after the 3 bytes of each of the others. */
  int64_t n = 3 * ((int64_t)d->nop - 1);
  part_lens[d->nop - 1] = d->r.n - n;
  if (part_lens[d->nop - 1] < 0) {
    return 0;
  }
  if (n > 0) {
    const uint8_t *buf = bdfi_vp8_read_full(&d->r, n);
    if (!buf) {
      return 0;
    }
    for (int i = 0; i < d->nop - 1; i++) {
      int64_t pl =
          (int64_t)buf[3 * i + 0] | (int64_t)buf[3 * i + 1] << 8 | (int64_t)buf[3 * i + 2] << 16;
      if (pl > part_lens[d->nop - 1]) {
        return 0;
      }
      part_lens[i] = pl;
      part_lens[d->nop - 1] -= pl;
    }
  }

  /* Go refuses a last partition of 16 MiB or more. */
  if (part_lens[d->nop - 1] >= ((int64_t)1 << 24)) {
    return 0;
  }

  /* Go reads all that remains of the chunk: the file has to have it. */
  const uint8_t *buf = bdfi_vp8_read_full(&d->r, d->r.n);
  if (!buf) {
    return 0;
  }
  for (int i = 0; i < d->nop; i++) {
    bdfi_vp8_partition_init(&d->op[i], buf, (size_t)part_lens[i]);
    buf += (size_t)part_lens[i];
  }
  return 1;
}

/* Decoder.parseQuant in vp8/quant.go (section 9.6) */
static void bdfi_vp8_parse_quant(bdfi_vp8_dec *d) {
  int32_t base_q0 = (int32_t)bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 7);
  int32_t dqy1_dc = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 4);
  int32_t dqy2_dc = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 4);
  int32_t dqy2_ac = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 4);
  int32_t dquv_dc = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 4);
  int32_t dquv_ac = bdfi_vp8_read_optional_int(&d->fp, BDFI_VP8_UNIFORM_PROB, 4);
  for (int i = 0; i < BDFI_VP8_N_SEGMENT; i++) {
    int32_t q = base_q0;
    if (d->use_segment) {
      if (d->relative_delta) {
        q += d->seg_quantizer[i];
      } else {
        q = d->seg_quantizer[i];
      }
    }
    bdfi_vp8_quant *qt = &d->quant[i];
    qt->y1[0] = bdfi_vp8_dequant_dc[bdfi_vp8_clip(q + dqy1_dc, 0, 127)];
    qt->y1[1] = bdfi_vp8_dequant_ac[bdfi_vp8_clip(q, 0, 127)];
    qt->y2[0] = (uint16_t)(bdfi_vp8_dequant_dc[bdfi_vp8_clip(q + dqy2_dc, 0, 127)] * 2);
    qt->y2[1] = (uint16_t)(bdfi_vp8_dequant_ac[bdfi_vp8_clip(q + dqy2_ac, 0, 127)] * 155 / 100);
    if (qt->y2[1] < 8) {
      qt->y2[1] = 8;
    }
    /* The 117 is not a typo: see the Go source. */
    qt->uv[0] = bdfi_vp8_dequant_dc[bdfi_vp8_clip(q + dquv_dc, 0, 117)];
    qt->uv[1] = bdfi_vp8_dequant_ac[bdfi_vp8_clip(q + dquv_ac, 0, 127)];
  }
}

/* Decoder.parseTokenProb in vp8/token.go */
static void bdfi_vp8_parse_token_prob(bdfi_vp8_dec *d) {
  for (int i = 0; i < BDFI_VP8_N_PLANE; i++) {
    for (int j = 0; j < BDFI_VP8_N_BAND; j++) {
      for (int k = 0; k < BDFI_VP8_N_CONTEXT; k++) {
        for (int l = 0; l < BDFI_VP8_N_PROB; l++) {
          if (bdfi_vp8_read_bit(&d->fp, bdfi_vp8_token_prob_update_prob[i][j][k][l])) {
            d->token_prob[i][j][k][l] =
                (uint8_t)bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 8);
          }
        }
      }
    }
  }
}

/* Decoder.parseOtherHeaders: everything of the headers after the frame
 * header. 0 where Go returns an error. */
static int bdfi_vp8_parse_other_headers(bdfi_vp8_dec *d) {
  /* Initialize and parse the first partition. */
  const uint8_t *first = bdfi_vp8_read_full(&d->r, (int64_t)d->fh.first_partition_len);
  if (!first) {
    return 0;
  }
  bdfi_vp8_partition_init(&d->fp, first, (size_t)d->fh.first_partition_len);
  /* Read and ignore the color space and pixel clamp values. */
  bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  bdfi_vp8_parse_segment_header(d);
  bdfi_vp8_parse_filter_header(d);
  if (!bdfi_vp8_parse_other_partitions(d)) {
    return 0;
  }
  bdfi_vp8_parse_quant(d);
  /* Read and ignore the refreshLastFrameBuffer bit. */
  bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  bdfi_vp8_parse_token_prob(d);
  d->use_skip_prob = bdfi_vp8_read_bit(&d->fp, BDFI_VP8_UNIFORM_PROB);
  if (d->use_skip_prob) {
    d->skip_prob = bdfi_vp8_read_uint(&d->fp, BDFI_VP8_UNIFORM_PROB, 8);
  }
  if (d->fp.eof) {
    return 0;
  }
  return 1;
}

/* ---- The predictor modes (vp8/pred.go, chapter 11) ------------------------ */

/* Decoder.parsePredModeY16 */
static void bdfi_vp8_parse_pred_mode_y16(bdfi_vp8_dec *d, int32_t mbx) {
  uint8_t p;
  if (!bdfi_vp8_read_bit(&d->fp, 156)) {
    if (!bdfi_vp8_read_bit(&d->fp, 163)) {
      p = BDFI_VP8_PRED_DC;
    } else {
      p = BDFI_VP8_PRED_VE;
    }
  } else if (!bdfi_vp8_read_bit(&d->fp, 128)) {
    p = BDFI_VP8_PRED_HE;
  } else {
    p = BDFI_VP8_PRED_TM;
  }
  for (int i = 0; i < 4; i++) {
    d->up_mb[mbx].pred[i] = p;
    d->left_mb.pred[i] = p;
  }
  d->pred_y16 = p;
}

/* Decoder.parsePredModeC8 */
static void bdfi_vp8_parse_pred_mode_c8(bdfi_vp8_dec *d) {
  if (!bdfi_vp8_read_bit(&d->fp, 142)) {
    d->pred_c8 = BDFI_VP8_PRED_DC;
  } else if (!bdfi_vp8_read_bit(&d->fp, 114)) {
    d->pred_c8 = BDFI_VP8_PRED_VE;
  } else if (!bdfi_vp8_read_bit(&d->fp, 183)) {
    d->pred_c8 = BDFI_VP8_PRED_HE;
  } else {
    d->pred_c8 = BDFI_VP8_PRED_TM;
  }
}

/* Decoder.parsePredModeY4 */
static void bdfi_vp8_parse_pred_mode_y4(bdfi_vp8_dec *d, int32_t mbx) {
  for (int j = 0; j < 4; j++) {
    uint8_t p = d->left_mb.pred[j];
    for (int i = 0; i < 4; i++) {
      const uint8_t *prob = bdfi_vp8_pred_prob[d->up_mb[mbx].pred[i]][p];
      if (!bdfi_vp8_read_bit(&d->fp, prob[0])) {
        p = BDFI_VP8_PRED_DC;
      } else if (!bdfi_vp8_read_bit(&d->fp, prob[1])) {
        p = BDFI_VP8_PRED_TM;
      } else if (!bdfi_vp8_read_bit(&d->fp, prob[2])) {
        p = BDFI_VP8_PRED_VE;
      } else if (!bdfi_vp8_read_bit(&d->fp, prob[3])) {
        if (!bdfi_vp8_read_bit(&d->fp, prob[4])) {
          p = BDFI_VP8_PRED_HE;
        } else if (!bdfi_vp8_read_bit(&d->fp, prob[5])) {
          p = BDFI_VP8_PRED_RD;
        } else {
          p = BDFI_VP8_PRED_VR;
        }
      } else if (!bdfi_vp8_read_bit(&d->fp, prob[6])) {
        p = BDFI_VP8_PRED_LD;
      } else if (!bdfi_vp8_read_bit(&d->fp, prob[7])) {
        p = BDFI_VP8_PRED_VL;
      } else if (!bdfi_vp8_read_bit(&d->fp, prob[8])) {
        p = BDFI_VP8_PRED_HD;
      } else {
        p = BDFI_VP8_PRED_HU;
      }
      d->pred_y4[j][i] = p;
      d->up_mb[mbx].pred[i] = p;
    }
    d->left_mb.pred[j] = p;
  }
}

/* ---- The prediction functions (vp8/predfunc.go, chapter 12) --------------- */

/* checkTopLeftPred: the DC mode has variants for the top row and the left
 * column. */
static uint8_t bdfi_vp8_check_top_left_pred(int32_t mbx, int32_t mby, uint8_t p) {
  if (p != BDFI_VP8_PRED_DC) {
    return p;
  }
  if (mbx == 0) {
    if (mby == 0) {
      return BDFI_VP8_PRED_DC_TOP_LEFT;
    }
    return BDFI_VP8_PRED_DC_LEFT;
  }
  if (mby == 0) {
    return BDFI_VP8_PRED_DC_TOP;
  }
  return BDFI_VP8_PRED_DC;
}

/* (a + 2b + c + 2) / 4 and (a + b + 1) / 2 of the diagonal modes. */
static uint8_t bdfi_vp8_avg3(int32_t a, int32_t b, int32_t c) {
  return (uint8_t)((a + 2 * b + c + 2) / 4);
}

static uint8_t bdfi_vp8_avg2(int32_t a, int32_t b) {
  return (uint8_t)((a + b + 1) / 2);
}

/* predFunc4*: predicts the 4x4 region at (y, x) of the workspace from the
 * row above it (with four more values to the right) and the column to its
 * left. */
static void bdfi_vp8_pred4(bdfi_vp8_dec *z, uint8_t mode, int y, int x) {
  uint8_t (*w)[32] = z->ybr;
  switch (mode) {
  case BDFI_VP8_PRED_DC: {
    uint32_t sum = 4;
    for (int i = 0; i < 4; i++) {
      sum += w[y - 1][x + i];
    }
    for (int j = 0; j < 4; j++) {
      sum += w[y + j][x - 1];
    }
    uint8_t avg = (uint8_t)(sum / 8);
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 4; i++) {
        w[y + j][x + i] = avg;
      }
    }
    break;
  }
  case BDFI_VP8_PRED_TM: {
    int32_t delta0 = -(int32_t)w[y - 1][x - 1];
    for (int j = 0; j < 4; j++) {
      int32_t delta1 = delta0 + w[y + j][x - 1];
      for (int i = 0; i < 4; i++) {
        int32_t delta2 = delta1 + w[y - 1][x + i];
        w[y + j][x + i] = bdfi_vp8_clip8(delta2);
      }
    }
    break;
  }
  case BDFI_VP8_PRED_VE: {
    int32_t a = w[y - 1][x - 1];
    int32_t b = w[y - 1][x + 0];
    int32_t c = w[y - 1][x + 1];
    int32_t d = w[y - 1][x + 2];
    int32_t e = w[y - 1][x + 3];
    int32_t f = w[y - 1][x + 4];
    uint8_t abc = bdfi_vp8_avg3(a, b, c);
    uint8_t bcd = bdfi_vp8_avg3(b, c, d);
    uint8_t cde = bdfi_vp8_avg3(c, d, e);
    uint8_t def = bdfi_vp8_avg3(d, e, f);
    for (int j = 0; j < 4; j++) {
      w[y + j][x + 0] = abc;
      w[y + j][x + 1] = bcd;
      w[y + j][x + 2] = cde;
      w[y + j][x + 3] = def;
    }
    break;
  }
  case BDFI_VP8_PRED_HE: {
    int32_t s = w[y + 3][x - 1];
    int32_t r = w[y + 2][x - 1];
    int32_t q = w[y + 1][x - 1];
    int32_t p = w[y + 0][x - 1];
    int32_t a = w[y - 1][x - 1];
    uint8_t ssr = bdfi_vp8_avg3(s, s, r);
    uint8_t srq = bdfi_vp8_avg3(s, r, q);
    uint8_t rqp = bdfi_vp8_avg3(r, q, p);
    uint8_t apq = bdfi_vp8_avg3(a, p, q);
    for (int i = 0; i < 4; i++) {
      w[y + 0][x + i] = apq;
      w[y + 1][x + i] = rqp;
      w[y + 2][x + i] = srq;
      w[y + 3][x + i] = ssr;
    }
    break;
  }
  case BDFI_VP8_PRED_RD: {
    int32_t s = w[y + 3][x - 1];
    int32_t r = w[y + 2][x - 1];
    int32_t q = w[y + 1][x - 1];
    int32_t p = w[y + 0][x - 1];
    int32_t a = w[y - 1][x - 1];
    int32_t b = w[y - 1][x + 0];
    int32_t c = w[y - 1][x + 1];
    int32_t d = w[y - 1][x + 2];
    int32_t e = w[y - 1][x + 3];
    uint8_t srq = bdfi_vp8_avg3(s, r, q);
    uint8_t rqp = bdfi_vp8_avg3(r, q, p);
    uint8_t qpa = bdfi_vp8_avg3(q, p, a);
    uint8_t pab = bdfi_vp8_avg3(p, a, b);
    uint8_t abc = bdfi_vp8_avg3(a, b, c);
    uint8_t bcd = bdfi_vp8_avg3(b, c, d);
    uint8_t cde = bdfi_vp8_avg3(c, d, e);
    w[y + 0][x + 0] = pab;
    w[y + 0][x + 1] = abc;
    w[y + 0][x + 2] = bcd;
    w[y + 0][x + 3] = cde;
    w[y + 1][x + 0] = qpa;
    w[y + 1][x + 1] = pab;
    w[y + 1][x + 2] = abc;
    w[y + 1][x + 3] = bcd;
    w[y + 2][x + 0] = rqp;
    w[y + 2][x + 1] = qpa;
    w[y + 2][x + 2] = pab;
    w[y + 2][x + 3] = abc;
    w[y + 3][x + 0] = srq;
    w[y + 3][x + 1] = rqp;
    w[y + 3][x + 2] = qpa;
    w[y + 3][x + 3] = pab;
    break;
  }
  case BDFI_VP8_PRED_VR: {
    int32_t r = w[y + 2][x - 1];
    int32_t q = w[y + 1][x - 1];
    int32_t p = w[y + 0][x - 1];
    int32_t a = w[y - 1][x - 1];
    int32_t b = w[y - 1][x + 0];
    int32_t c = w[y - 1][x + 1];
    int32_t d = w[y - 1][x + 2];
    int32_t e = w[y - 1][x + 3];
    uint8_t ab = bdfi_vp8_avg2(a, b);
    uint8_t bc = bdfi_vp8_avg2(b, c);
    uint8_t cd = bdfi_vp8_avg2(c, d);
    uint8_t de = bdfi_vp8_avg2(d, e);
    uint8_t rqp = bdfi_vp8_avg3(r, q, p);
    uint8_t qpa = bdfi_vp8_avg3(q, p, a);
    uint8_t pab = bdfi_vp8_avg3(p, a, b);
    uint8_t abc = bdfi_vp8_avg3(a, b, c);
    uint8_t bcd = bdfi_vp8_avg3(b, c, d);
    uint8_t cde = bdfi_vp8_avg3(c, d, e);
    w[y + 0][x + 0] = ab;
    w[y + 0][x + 1] = bc;
    w[y + 0][x + 2] = cd;
    w[y + 0][x + 3] = de;
    w[y + 1][x + 0] = pab;
    w[y + 1][x + 1] = abc;
    w[y + 1][x + 2] = bcd;
    w[y + 1][x + 3] = cde;
    w[y + 2][x + 0] = qpa;
    w[y + 2][x + 1] = ab;
    w[y + 2][x + 2] = bc;
    w[y + 2][x + 3] = cd;
    w[y + 3][x + 0] = rqp;
    w[y + 3][x + 1] = pab;
    w[y + 3][x + 2] = abc;
    w[y + 3][x + 3] = bcd;
    break;
  }
  case BDFI_VP8_PRED_LD: {
    int32_t a = w[y - 1][x + 0];
    int32_t b = w[y - 1][x + 1];
    int32_t c = w[y - 1][x + 2];
    int32_t d = w[y - 1][x + 3];
    int32_t e = w[y - 1][x + 4];
    int32_t f = w[y - 1][x + 5];
    int32_t g = w[y - 1][x + 6];
    int32_t h = w[y - 1][x + 7];
    uint8_t abc = bdfi_vp8_avg3(a, b, c);
    uint8_t bcd = bdfi_vp8_avg3(b, c, d);
    uint8_t cde = bdfi_vp8_avg3(c, d, e);
    uint8_t def = bdfi_vp8_avg3(d, e, f);
    uint8_t efg = bdfi_vp8_avg3(e, f, g);
    uint8_t fgh = bdfi_vp8_avg3(f, g, h);
    uint8_t ghh = bdfi_vp8_avg3(g, h, h);
    w[y + 0][x + 0] = abc;
    w[y + 0][x + 1] = bcd;
    w[y + 0][x + 2] = cde;
    w[y + 0][x + 3] = def;
    w[y + 1][x + 0] = bcd;
    w[y + 1][x + 1] = cde;
    w[y + 1][x + 2] = def;
    w[y + 1][x + 3] = efg;
    w[y + 2][x + 0] = cde;
    w[y + 2][x + 1] = def;
    w[y + 2][x + 2] = efg;
    w[y + 2][x + 3] = fgh;
    w[y + 3][x + 0] = def;
    w[y + 3][x + 1] = efg;
    w[y + 3][x + 2] = fgh;
    w[y + 3][x + 3] = ghh;
    break;
  }
  case BDFI_VP8_PRED_VL: {
    int32_t a = w[y - 1][x + 0];
    int32_t b = w[y - 1][x + 1];
    int32_t c = w[y - 1][x + 2];
    int32_t d = w[y - 1][x + 3];
    int32_t e = w[y - 1][x + 4];
    int32_t f = w[y - 1][x + 5];
    int32_t g = w[y - 1][x + 6];
    int32_t h = w[y - 1][x + 7];
    uint8_t ab = bdfi_vp8_avg2(a, b);
    uint8_t bc = bdfi_vp8_avg2(b, c);
    uint8_t cd = bdfi_vp8_avg2(c, d);
    uint8_t de = bdfi_vp8_avg2(d, e);
    uint8_t abc = bdfi_vp8_avg3(a, b, c);
    uint8_t bcd = bdfi_vp8_avg3(b, c, d);
    uint8_t cde = bdfi_vp8_avg3(c, d, e);
    uint8_t def = bdfi_vp8_avg3(d, e, f);
    uint8_t efg = bdfi_vp8_avg3(e, f, g);
    uint8_t fgh = bdfi_vp8_avg3(f, g, h);
    w[y + 0][x + 0] = ab;
    w[y + 0][x + 1] = bc;
    w[y + 0][x + 2] = cd;
    w[y + 0][x + 3] = de;
    w[y + 1][x + 0] = abc;
    w[y + 1][x + 1] = bcd;
    w[y + 1][x + 2] = cde;
    w[y + 1][x + 3] = def;
    w[y + 2][x + 0] = bc;
    w[y + 2][x + 1] = cd;
    w[y + 2][x + 2] = de;
    w[y + 2][x + 3] = efg;
    w[y + 3][x + 0] = bcd;
    w[y + 3][x + 1] = cde;
    w[y + 3][x + 2] = def;
    w[y + 3][x + 3] = fgh;
    break;
  }
  case BDFI_VP8_PRED_HD: {
    int32_t s = w[y + 3][x - 1];
    int32_t r = w[y + 2][x - 1];
    int32_t q = w[y + 1][x - 1];
    int32_t p = w[y + 0][x - 1];
    int32_t a = w[y - 1][x - 1];
    int32_t b = w[y - 1][x + 0];
    int32_t c = w[y - 1][x + 1];
    int32_t d = w[y - 1][x + 2];
    uint8_t sr = bdfi_vp8_avg2(s, r);
    uint8_t rq = bdfi_vp8_avg2(r, q);
    uint8_t qp = bdfi_vp8_avg2(q, p);
    uint8_t pa = bdfi_vp8_avg2(p, a);
    uint8_t srq = bdfi_vp8_avg3(s, r, q);
    uint8_t rqp = bdfi_vp8_avg3(r, q, p);
    uint8_t qpa = bdfi_vp8_avg3(q, p, a);
    uint8_t pab = bdfi_vp8_avg3(p, a, b);
    uint8_t abc = bdfi_vp8_avg3(a, b, c);
    uint8_t bcd = bdfi_vp8_avg3(b, c, d);
    w[y + 0][x + 0] = pa;
    w[y + 0][x + 1] = pab;
    w[y + 0][x + 2] = abc;
    w[y + 0][x + 3] = bcd;
    w[y + 1][x + 0] = qp;
    w[y + 1][x + 1] = qpa;
    w[y + 1][x + 2] = pa;
    w[y + 1][x + 3] = pab;
    w[y + 2][x + 0] = rq;
    w[y + 2][x + 1] = rqp;
    w[y + 2][x + 2] = qp;
    w[y + 2][x + 3] = qpa;
    w[y + 3][x + 0] = sr;
    w[y + 3][x + 1] = srq;
    w[y + 3][x + 2] = rq;
    w[y + 3][x + 3] = rqp;
    break;
  }
  default: { /* BDFI_VP8_PRED_HU: a 4x4 mode is one of the ten */
    int32_t s = w[y + 3][x - 1];
    int32_t r = w[y + 2][x - 1];
    int32_t q = w[y + 1][x - 1];
    int32_t p = w[y + 0][x - 1];
    uint8_t pq = bdfi_vp8_avg2(p, q);
    uint8_t qr = bdfi_vp8_avg2(q, r);
    uint8_t rs = bdfi_vp8_avg2(r, s);
    uint8_t pqr = bdfi_vp8_avg3(p, q, r);
    uint8_t qrs = bdfi_vp8_avg3(q, r, s);
    uint8_t rss = bdfi_vp8_avg3(r, s, s);
    uint8_t sss = (uint8_t)s;
    w[y + 0][x + 0] = pq;
    w[y + 0][x + 1] = pqr;
    w[y + 0][x + 2] = qr;
    w[y + 0][x + 3] = qrs;
    w[y + 1][x + 0] = qr;
    w[y + 1][x + 1] = qrs;
    w[y + 1][x + 2] = rs;
    w[y + 1][x + 3] = rss;
    w[y + 2][x + 0] = rs;
    w[y + 2][x + 1] = rss;
    w[y + 2][x + 2] = sss;
    w[y + 2][x + 3] = sss;
    w[y + 3][x + 0] = sss;
    w[y + 3][x + 1] = sss;
    w[y + 3][x + 2] = sss;
    w[y + 3][x + 3] = sss;
    break;
  }
  }
}

/* predFunc8* and predFunc16*: predicts the n x n region (n is 8 or 16) at
 * (y, x) of the workspace. The modes are DC (with its variants for the top
 * row and the left column: Go names them after the border that is missing),
 * TM, VE and HE. */
static void bdfi_vp8_pred_block(bdfi_vp8_dec *z, uint8_t mode, int n, int y, int x) {
  uint8_t (*w)[32] = z->ybr;
  switch (mode) {
  case BDFI_VP8_PRED_TM: {
    int32_t delta0 = -(int32_t)w[y - 1][x - 1];
    for (int j = 0; j < n; j++) {
      int32_t delta1 = delta0 + w[y + j][x - 1];
      for (int i = 0; i < n; i++) {
        int32_t delta2 = delta1 + w[y - 1][x + i];
        w[y + j][x + i] = bdfi_vp8_clip8(delta2);
      }
    }
    return;
  }
  case BDFI_VP8_PRED_VE:
    for (int j = 0; j < n; j++) {
      for (int i = 0; i < n; i++) {
        w[y + j][x + i] = w[y - 1][x + i];
      }
    }
    return;
  case BDFI_VP8_PRED_HE:
    for (int j = 0; j < n; j++) {
      for (int i = 0; i < n; i++) {
        w[y + j][x + i] = w[y + j][x - 1];
      }
    }
    return;
  default:
    break;
  }
  uint8_t avg;
  if (mode == BDFI_VP8_PRED_DC) {
    uint32_t sum = (uint32_t)n;
    for (int i = 0; i < n; i++) {
      sum += w[y - 1][x + i];
    }
    for (int j = 0; j < n; j++) {
      sum += w[y + j][x - 1];
    }
    avg = (uint8_t)(sum / (2 * (uint32_t)n));
  } else if (mode == BDFI_VP8_PRED_DC_TOP) {
    uint32_t sum = (uint32_t)n / 2;
    for (int j = 0; j < n; j++) {
      sum += w[y + j][x - 1];
    }
    avg = (uint8_t)(sum / (uint32_t)n);
  } else if (mode == BDFI_VP8_PRED_DC_LEFT) {
    uint32_t sum = (uint32_t)n / 2;
    for (int i = 0; i < n; i++) {
      sum += w[y - 1][x + i];
    }
    avg = (uint8_t)(sum / (uint32_t)n);
  } else { /* BDFI_VP8_PRED_DC_TOP_LEFT */
    avg = 0x80;
  }
  for (int j = 0; j < n; j++) {
    for (int i = 0; i < n; i++) {
      w[y + j][x + i] = avg;
    }
  }
}

/* ---- The inverse transforms (vp8/idct.go, sections 14.3 and 14.4) --------- */

#define BDFI_VP8_C1 85627 /* 65536 * cos(pi/8) * sqrt(2) */
#define BDFI_VP8_C2 35468 /* 65536 * sin(pi/8) * sqrt(2) */

/* Decoder.inverseDCT4. The products are int32 in Go and wrap. */
static void bdfi_vp8_inverse_dct4(bdfi_vp8_dec *z, int y, int x, int coeff_base) {
  int32_t m[4][4];
  const int16_t *co = z->coeff + coeff_base;
  for (int i = 0; i < 4; i++) {
    int32_t a = (int32_t)co[i + 0] + (int32_t)co[i + 8];
    int32_t b = (int32_t)co[i + 0] - (int32_t)co[i + 8];
    int32_t c = bdfi_vp8_sar(bdfi_vp8_mul(co[i + 4], BDFI_VP8_C2), 16) -
                bdfi_vp8_sar(bdfi_vp8_mul(co[i + 12], BDFI_VP8_C1), 16);
    int32_t d = bdfi_vp8_sar(bdfi_vp8_mul(co[i + 4], BDFI_VP8_C1), 16) +
                bdfi_vp8_sar(bdfi_vp8_mul(co[i + 12], BDFI_VP8_C2), 16);
    m[i][0] = a + d;
    m[i][1] = b + c;
    m[i][2] = b - c;
    m[i][3] = a - d;
  }
  for (int j = 0; j < 4; j++) {
    int32_t dc = m[0][j] + 4;
    int32_t a = dc + m[2][j];
    int32_t b = dc - m[2][j];
    int32_t c = bdfi_vp8_sar(bdfi_vp8_mul(m[1][j], BDFI_VP8_C2), 16) -
                bdfi_vp8_sar(bdfi_vp8_mul(m[3][j], BDFI_VP8_C1), 16);
    int32_t d = bdfi_vp8_sar(bdfi_vp8_mul(m[1][j], BDFI_VP8_C1), 16) +
                bdfi_vp8_sar(bdfi_vp8_mul(m[3][j], BDFI_VP8_C2), 16);
    uint8_t *row = z->ybr[y + j] + x;
    row[0] = bdfi_vp8_clip8((int32_t)row[0] + bdfi_vp8_sar(a + d, 3));
    row[1] = bdfi_vp8_clip8((int32_t)row[1] + bdfi_vp8_sar(b + c, 3));
    row[2] = bdfi_vp8_clip8((int32_t)row[2] + bdfi_vp8_sar(b - c, 3));
    row[3] = bdfi_vp8_clip8((int32_t)row[3] + bdfi_vp8_sar(a - d, 3));
  }
}

/* Decoder.inverseDCT4DCOnly */
static void bdfi_vp8_inverse_dct4_dc_only(bdfi_vp8_dec *z, int y, int x, int coeff_base) {
  int32_t dc = bdfi_vp8_sar((int32_t)z->coeff[coeff_base + 0] + 4, 3);
  for (int j = 0; j < 4; j++) {
    uint8_t *row = z->ybr[y + j] + x;
    for (int i = 0; i < 4; i++) {
      row[i] = bdfi_vp8_clip8((int32_t)row[i] + dc);
    }
  }
}

/* Decoder.inverseDCT8 */
static void bdfi_vp8_inverse_dct8(bdfi_vp8_dec *z, int y, int x, int coeff_base) {
  bdfi_vp8_inverse_dct4(z, y + 0, x + 0, coeff_base + 0 * 16);
  bdfi_vp8_inverse_dct4(z, y + 0, x + 4, coeff_base + 1 * 16);
  bdfi_vp8_inverse_dct4(z, y + 4, x + 0, coeff_base + 2 * 16);
  bdfi_vp8_inverse_dct4(z, y + 4, x + 4, coeff_base + 3 * 16);
}

/* Decoder.inverseDCT8DCOnly */
static void bdfi_vp8_inverse_dct8_dc_only(bdfi_vp8_dec *z, int y, int x, int coeff_base) {
  bdfi_vp8_inverse_dct4_dc_only(z, y + 0, x + 0, coeff_base + 0 * 16);
  bdfi_vp8_inverse_dct4_dc_only(z, y + 0, x + 4, coeff_base + 1 * 16);
  bdfi_vp8_inverse_dct4_dc_only(z, y + 4, x + 0, coeff_base + 2 * 16);
  bdfi_vp8_inverse_dct4_dc_only(z, y + 4, x + 4, coeff_base + 3 * 16);
}

/* Decoder.inverseWHT16: the 16 values go out as int16 and wrap. */
static void bdfi_vp8_inverse_wht16(bdfi_vp8_dec *d) {
  int32_t m[16];
  const int16_t *co = d->coeff + BDFI_VP8_WHT_COEFF_BASE;
  for (int i = 0; i < 4; i++) {
    int32_t a0 = (int32_t)co[0 + i] + (int32_t)co[12 + i];
    int32_t a1 = (int32_t)co[4 + i] + (int32_t)co[8 + i];
    int32_t a2 = (int32_t)co[4 + i] - (int32_t)co[8 + i];
    int32_t a3 = (int32_t)co[0 + i] - (int32_t)co[12 + i];
    m[0 + i] = a0 + a1;
    m[8 + i] = a0 - a1;
    m[4 + i] = a3 + a2;
    m[12 + i] = a3 - a2;
  }
  int out = 0;
  for (int i = 0; i < 4; i++) {
    int32_t dc = m[0 + i * 4] + 3;
    int32_t a0 = dc + m[3 + i * 4];
    int32_t a1 = m[1 + i * 4] + m[2 + i * 4];
    int32_t a2 = m[1 + i * 4] - m[2 + i * 4];
    int32_t a3 = dc - m[3 + i * 4];
    d->coeff[out + 0] = bdfi_vp8_i16(bdfi_vp8_sar(a0 + a1, 3));
    d->coeff[out + 16] = bdfi_vp8_i16(bdfi_vp8_sar(a3 + a2, 3));
    d->coeff[out + 32] = bdfi_vp8_i16(bdfi_vp8_sar(a0 - a1, 3));
    d->coeff[out + 48] = bdfi_vp8_i16(bdfi_vp8_sar(a3 - a2, 3));
    out += 64;
  }
}

/* ---- Residuals and reconstruction (vp8/reconstruct.go) -------------------- */

/* Decoder.prepareYBR: the row above and the column to the left of the
 * macroblock, in the workspace. */
static void bdfi_vp8_prepare_ybr(bdfi_vp8_dec *d, int32_t mbx, int32_t mby) {
  uint8_t (*w)[32] = d->ybr;
  if (mbx == 0) {
    for (int y = 0; y < 17; y++) {
      w[y][7] = 0x81;
    }
    for (int y = 17; y < 26; y++) {
      w[y][7] = 0x81;
      w[y][23] = 0x81;
    }
  } else {
    for (int y = 0; y < 17; y++) {
      w[y][7] = w[y][7 + 16];
    }
    for (int y = 17; y < 26; y++) {
      w[y][7] = w[y][15];
      w[y][23] = w[y][31];
    }
  }
  if (mby == 0) {
    for (int x = 7; x < 28; x++) {
      w[0][x] = 0x7f;
    }
    for (int x = 7; x < 16; x++) {
      w[17][x] = 0x7f;
    }
    for (int x = 23; x < 32; x++) {
      w[17][x] = 0x7f;
    }
  } else {
    const uint8_t *above = d->img_y + (16 * (size_t)mby - 1) * d->y_stride + 16 * (size_t)mbx;
    for (int i = 0; i < 16; i++) {
      w[0][8 + i] = above[i];
    }
    const uint8_t *above_b = d->img_cb + (8 * (size_t)mby - 1) * d->c_stride + 8 * (size_t)mbx;
    for (int i = 0; i < 8; i++) {
      w[17][8 + i] = above_b[i];
    }
    const uint8_t *above_r = d->img_cr + (8 * (size_t)mby - 1) * d->c_stride + 8 * (size_t)mbx;
    for (int i = 0; i < 8; i++) {
      w[17][24 + i] = above_r[i];
    }
    if (mbx == d->mbw - 1) {
      for (int i = 16; i < 20; i++) {
        w[0][8 + i] = above[15];
      }
    } else {
      for (int i = 16; i < 20; i++) {
        w[0][8 + i] = above[i];
      }
    }
  }
  for (int y = 4; y < 16; y += 4) {
    w[y][24] = w[0][24];
    w[y][25] = w[0][25];
    w[y][26] = w[0][26];
    w[y][27] = w[0][27];
  }
}

/* pack: four 0/1 values as four bits. */
static uint32_t bdfi_vp8_pack(const uint8_t x[4], int shift) {
  uint32_t u =
      (uint32_t)x[0] << 0 | (uint32_t)x[1] << 1 | (uint32_t)x[2] << 2 | (uint32_t)x[3] << 3;
  return u << shift;
}

/* unpack: the four bits of v as four 0/1 values. */
static void bdfi_vp8_unpack(uint8_t out[4], uint32_t v) {
  out[0] = (uint8_t)(v & 1);
  out[1] = (uint8_t)(v >> 1 & 1);
  out[2] = (uint8_t)(v >> 2 & 1);
  out[3] = (uint8_t)(v >> 3 & 1);
}

/* Decoder.parseResiduals4 (section 13.3): the coefficients of a 4x4 region,
 * into coeff from coeff_base. Returns 1 if there was at least one. */
static uint8_t bdfi_vp8_parse_residuals4(bdfi_vp8_dec *d, bdfi_vp8_partition *r, int plane,
                                         uint8_t context, const uint16_t quant[2],
                                         int skip_first_coeff, int coeff_base) {
  uint8_t (*prob)[BDFI_VP8_N_CONTEXT][BDFI_VP8_N_PROB] = d->token_prob[plane];
  int n = skip_first_coeff ? 1 : 0;
  const uint8_t *p = prob[bdfi_vp8_bands[n]][context];
  if (!bdfi_vp8_read_bit(r, p[0])) {
    return 0;
  }
  while (n != 16) {
    n++;
    if (!bdfi_vp8_read_bit(r, p[1])) {
      p = prob[bdfi_vp8_bands[n]][0];
      continue;
    }
    uint32_t v;
    if (!bdfi_vp8_read_bit(r, p[2])) {
      v = 1;
      p = prob[bdfi_vp8_bands[n]][1];
    } else {
      if (!bdfi_vp8_read_bit(r, p[3])) {
        if (!bdfi_vp8_read_bit(r, p[4])) {
          v = 2;
        } else {
          v = 3 + bdfi_vp8_read_uint(r, p[5], 1);
        }
      } else if (!bdfi_vp8_read_bit(r, p[6])) {
        if (!bdfi_vp8_read_bit(r, p[7])) {
          /* Category 1. */
          v = 5 + bdfi_vp8_read_uint(r, 159, 1);
        } else {
          /* Category 2. */
          v = 7 + 2 * bdfi_vp8_read_uint(r, 165, 1);
          v += bdfi_vp8_read_uint(r, 145, 1);
        }
      } else {
        /* Categories 3, 4, 5 or 6. */
        uint32_t b1 = bdfi_vp8_read_uint(r, p[8], 1);
        uint32_t b0 = bdfi_vp8_read_uint(r, p[9 + b1], 1);
        uint32_t cat = 2 * b1 + b0;
        const uint8_t *tab = bdfi_vp8_cat3456[cat];
        v = 0;
        for (int i = 0; tab[i] != 0; i++) {
          v *= 2;
          v += bdfi_vp8_read_uint(r, tab[i], 1);
        }
        v += 3 + ((uint32_t)8 << cat);
      }
      p = prob[bdfi_vp8_bands[n]][2];
    }
    uint8_t z = bdfi_vp8_zigzag[n - 1];
    int32_t c = (int32_t)v * (int32_t)quant[z > 0 ? 1 : 0];
    if (bdfi_vp8_read_bit(r, BDFI_VP8_UNIFORM_PROB)) {
      c = -c;
    }
    /* Go stores it as int16: a large coefficient wraps. */
    d->coeff[coeff_base + z] = bdfi_vp8_i16(c);
    if (n == 16 || !bdfi_vp8_read_bit(r, p[0])) {
      return 1;
    }
  }
  return 1;
}

/* Decoder.parseResiduals: the coefficients of a macroblock. Returns whether
 * the inner loop filter is to be skipped for it (it has no coefficient). */
static int bdfi_vp8_parse_residuals(bdfi_vp8_dec *d, int32_t mbx, int32_t mby) {
  bdfi_vp8_partition *partition = &d->op[mby & (d->nop - 1)];
  int plane = BDFI_VP8_PLANE_Y1_SANS_Y2;
  const bdfi_vp8_quant *quant = &d->quant[d->segment];

  /* Parse the DC coefficient of each 4x4 luma region. */
  if (d->use_pred_y16) {
    uint8_t nz = bdfi_vp8_parse_residuals4(d, partition, BDFI_VP8_PLANE_Y2,
                                           (uint8_t)(d->left_mb.nz_y16 + d->up_mb[mbx].nz_y16),
                                           quant->y2, 0, BDFI_VP8_WHT_COEFF_BASE);
    d->left_mb.nz_y16 = nz;
    d->up_mb[mbx].nz_y16 = nz;
    bdfi_vp8_inverse_wht16(d);
    plane = BDFI_VP8_PLANE_Y1_WITH_Y2;
  }

  uint8_t nz_dc[4] = {0, 0, 0, 0}, nz_ac[4] = {0, 0, 0, 0};
  uint32_t nz_dc_mask = 0, nz_ac_mask = 0;
  int coeff_base = 0;

  /* Parse the luma coefficients. */
  uint8_t lnz[4], unz[4];
  bdfi_vp8_unpack(lnz, d->left_mb.nz_mask & 0x0fu);
  bdfi_vp8_unpack(unz, d->up_mb[mbx].nz_mask & 0x0fu);
  for (int y = 0; y < 4; y++) {
    uint8_t nz = lnz[y];
    for (int x = 0; x < 4; x++) {
      nz = bdfi_vp8_parse_residuals4(d, partition, plane, (uint8_t)(nz + unz[x]), quant->y1,
                                     d->use_pred_y16, coeff_base);
      unz[x] = nz;
      nz_ac[x] = nz;
      nz_dc[x] = (uint8_t)(d->coeff[coeff_base] != 0);
      coeff_base += 16;
    }
    lnz[y] = nz;
    nz_dc_mask |= bdfi_vp8_pack(nz_dc, y * 4);
    nz_ac_mask |= bdfi_vp8_pack(nz_ac, y * 4);
  }
  uint32_t lnz_mask = bdfi_vp8_pack(lnz, 0);
  uint32_t unz_mask = bdfi_vp8_pack(unz, 0);

  /* Parse the chroma coefficients. */
  bdfi_vp8_unpack(lnz, (uint32_t)d->left_mb.nz_mask >> 4);
  bdfi_vp8_unpack(unz, (uint32_t)d->up_mb[mbx].nz_mask >> 4);
  for (int c = 0; c < 4; c += 2) {
    for (int y = 0; y < 2; y++) {
      uint8_t nz = lnz[y + c];
      for (int x = 0; x < 2; x++) {
        nz = bdfi_vp8_parse_residuals4(d, partition, BDFI_VP8_PLANE_UV, (uint8_t)(nz + unz[x + c]),
                                       quant->uv, 0, coeff_base);
        unz[x + c] = nz;
        nz_ac[y * 2 + x] = nz;
        nz_dc[y * 2 + x] = (uint8_t)(d->coeff[coeff_base] != 0);
        coeff_base += 16;
      }
      lnz[y + c] = nz;
    }
    nz_dc_mask |= bdfi_vp8_pack(nz_dc, 16 + c * 2);
    nz_ac_mask |= bdfi_vp8_pack(nz_ac, 16 + c * 2);
  }
  lnz_mask |= bdfi_vp8_pack(lnz, 4);
  unz_mask |= bdfi_vp8_pack(unz, 4);

  /* Save decoder state. */
  d->left_mb.nz_mask = (uint8_t)lnz_mask;
  d->up_mb[mbx].nz_mask = (uint8_t)unz_mask;
  d->nz_dc_mask = nz_dc_mask;
  d->nz_ac_mask = nz_ac_mask;

  return nz_dc_mask == 0 && nz_ac_mask == 0;
}

/* Decoder.reconstructMacroblock: the predictions plus the inverse
 * transforms of the residuals, in the workspace. */
static void bdfi_vp8_reconstruct_macroblock(bdfi_vp8_dec *d, int32_t mbx, int32_t mby) {
  if (d->use_pred_y16) {
    uint8_t p = bdfi_vp8_check_top_left_pred(mbx, mby, d->pred_y16);
    bdfi_vp8_pred_block(d, p, 16, 1, 8);
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 4; i++) {
        int n = 4 * j + i;
        int y = 4 * j + 1;
        int x = 4 * i + 8;
        uint32_t mask = (uint32_t)1 << n;
        if ((d->nz_ac_mask & mask) != 0) {
          bdfi_vp8_inverse_dct4(d, y, x, 16 * n);
        } else if ((d->nz_dc_mask & mask) != 0) {
          bdfi_vp8_inverse_dct4_dc_only(d, y, x, 16 * n);
        }
      }
    }
  } else {
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 4; i++) {
        int n = 4 * j + i;
        int y = 4 * j + 1;
        int x = 4 * i + 8;
        bdfi_vp8_pred4(d, d->pred_y4[j][i], y, x);
        uint32_t mask = (uint32_t)1 << n;
        if ((d->nz_ac_mask & mask) != 0) {
          bdfi_vp8_inverse_dct4(d, y, x, 16 * n);
        } else if ((d->nz_dc_mask & mask) != 0) {
          bdfi_vp8_inverse_dct4_dc_only(d, y, x, 16 * n);
        }
      }
    }
  }
  uint8_t p = bdfi_vp8_check_top_left_pred(mbx, mby, d->pred_c8);
  bdfi_vp8_pred_block(d, p, 8, BDFI_VP8_YBR_BY, BDFI_VP8_YBR_BX);
  if ((d->nz_ac_mask & 0x0f0000u) != 0) {
    bdfi_vp8_inverse_dct8(d, BDFI_VP8_YBR_BY, BDFI_VP8_YBR_BX, BDFI_VP8_B_COEFF_BASE);
  } else if ((d->nz_dc_mask & 0x0f0000u) != 0) {
    bdfi_vp8_inverse_dct8_dc_only(d, BDFI_VP8_YBR_BY, BDFI_VP8_YBR_BX, BDFI_VP8_B_COEFF_BASE);
  }
  bdfi_vp8_pred_block(d, p, 8, BDFI_VP8_YBR_RY, BDFI_VP8_YBR_RX);
  if ((d->nz_ac_mask & 0xf00000u) != 0) {
    bdfi_vp8_inverse_dct8(d, BDFI_VP8_YBR_RY, BDFI_VP8_YBR_RX, BDFI_VP8_R_COEFF_BASE);
  } else if ((d->nz_dc_mask & 0xf00000u) != 0) {
    bdfi_vp8_inverse_dct8_dc_only(d, BDFI_VP8_YBR_RY, BDFI_VP8_YBR_RX, BDFI_VP8_R_COEFF_BASE);
  }
}

/* Decoder.reconstruct: one macroblock, into the image. Returns whether the
 * inner loop filter is to be skipped for it. */
static int bdfi_vp8_reconstruct(bdfi_vp8_dec *d, int32_t mbx, int32_t mby) {
  int skip = 0;
  if (d->update_map) {
    if (!bdfi_vp8_read_bit(&d->fp, d->seg_prob[0])) {
      d->segment = (int)bdfi_vp8_read_uint(&d->fp, d->seg_prob[1], 1);
    } else {
      d->segment = (int)bdfi_vp8_read_uint(&d->fp, d->seg_prob[2], 1) + 2;
    }
  }
  if (d->use_skip_prob) {
    skip = bdfi_vp8_read_bit(&d->fp, d->skip_prob);
  }
  /* Prepare the workspace. */
  memset(d->coeff, 0, sizeof d->coeff);
  bdfi_vp8_prepare_ybr(d, mbx, mby);
  /* Parse the predictor modes. */
  d->use_pred_y16 = bdfi_vp8_read_bit(&d->fp, 145);
  if (d->use_pred_y16) {
    bdfi_vp8_parse_pred_mode_y16(d, mbx);
  } else {
    bdfi_vp8_parse_pred_mode_y4(d, mbx);
  }
  bdfi_vp8_parse_pred_mode_c8(d);
  /* Parse the residuals. */
  if (!skip) {
    skip = bdfi_vp8_parse_residuals(d, mbx, mby);
  } else {
    if (d->use_pred_y16) {
      d->left_mb.nz_y16 = 0;
      d->up_mb[mbx].nz_y16 = 0;
    }
    d->left_mb.nz_mask = 0;
    d->up_mb[mbx].nz_mask = 0;
    d->nz_dc_mask = 0;
    d->nz_ac_mask = 0;
  }
  /* Reconstruct the YCbCr data and copy it to the image. */
  bdfi_vp8_reconstruct_macroblock(d, mbx, mby);
  size_t yi = ((size_t)mby * d->y_stride + (size_t)mbx) * 16;
  for (int y = 0; y < 16; y++) {
    memcpy(d->img_y + yi, d->ybr[BDFI_VP8_YBR_YY + y] + BDFI_VP8_YBR_YX, 16);
    yi += d->y_stride;
  }
  size_t ci = ((size_t)mby * d->c_stride + (size_t)mbx) * 8;
  for (int y = 0; y < 8; y++) {
    memcpy(d->img_cb + ci, d->ybr[BDFI_VP8_YBR_BY + y] + BDFI_VP8_YBR_BX, 8);
    memcpy(d->img_cr + ci, d->ybr[BDFI_VP8_YBR_RY + y] + BDFI_VP8_YBR_RX, 8);
    ci += d->c_stride;
  }
  return skip;
}

/* ---- The loop filter (vp8/filter.go, chapter 15) -------------------------- */

static int bdfi_vp8_abs(int x) {
  return x < 0 ? -x : x;
}

static int bdfi_vp8_clamp15(int x) {
  if (x < -16) {
    return -16;
  }
  if (x > 15) {
    return 15;
  }
  return x;
}

static int bdfi_vp8_clamp127(int x) {
  if (x < -128) {
    return -128;
  }
  if (x > 127) {
    return 127;
  }
  return x;
}

/* filter2: a band 2 pixels wide (or high) along an edge of 16. The pixels
 * of the edge are i_step apart, and j_step apart across it. */
static void bdfi_vp8_filter2(uint8_t *pix, int level, size_t index, size_t i_step, size_t j_step) {
  for (int n = 16; n > 0; n--, index += i_step) {
    int p1 = pix[index - 2 * j_step];
    int p0 = pix[index - 1 * j_step];
    int q0 = pix[index + 0 * j_step];
    int q1 = pix[index + 1 * j_step];
    if ((bdfi_vp8_abs(p0 - q0) << 1) + (bdfi_vp8_abs(p1 - q1) >> 1) > level) {
      continue;
    }
    int a = 3 * (q0 - p0) + bdfi_vp8_clamp127(p1 - q1);
    int a1 = bdfi_vp8_clamp15(bdfi_vp8_sar(a + 4, 3));
    int a2 = bdfi_vp8_clamp15(bdfi_vp8_sar(a + 3, 3));
    pix[index - 1 * j_step] = bdfi_vp8_clip8(p0 + a2);
    pix[index + 0 * j_step] = bdfi_vp8_clip8(q0 - a1);
  }
}

/* filter246: a band 2, 4 or 6 pixels wide (or high) along an edge of n. */
static void bdfi_vp8_filter246(uint8_t *pix, int n, int level, int ilevel, int hlevel, size_t index,
                               size_t i_step, size_t j_step, int four_not_six) {
  for (; n > 0; n--, index += i_step) {
    int p3 = pix[index - 4 * j_step];
    int p2 = pix[index - 3 * j_step];
    int p1 = pix[index - 2 * j_step];
    int p0 = pix[index - 1 * j_step];
    int q0 = pix[index + 0 * j_step];
    int q1 = pix[index + 1 * j_step];
    int q2 = pix[index + 2 * j_step];
    int q3 = pix[index + 3 * j_step];
    if ((bdfi_vp8_abs(p0 - q0) << 1) + (bdfi_vp8_abs(p1 - q1) >> 1) > level) {
      continue;
    }
    if (bdfi_vp8_abs(p3 - p2) > ilevel || bdfi_vp8_abs(p2 - p1) > ilevel ||
        bdfi_vp8_abs(p1 - p0) > ilevel || bdfi_vp8_abs(q1 - q0) > ilevel ||
        bdfi_vp8_abs(q2 - q1) > ilevel || bdfi_vp8_abs(q3 - q2) > ilevel) {
      continue;
    }
    if (bdfi_vp8_abs(p1 - p0) > hlevel || bdfi_vp8_abs(q1 - q0) > hlevel) {
      /* Filter 2 pixels. */
      int a = 3 * (q0 - p0) + bdfi_vp8_clamp127(p1 - q1);
      int a1 = bdfi_vp8_clamp15(bdfi_vp8_sar(a + 4, 3));
      int a2 = bdfi_vp8_clamp15(bdfi_vp8_sar(a + 3, 3));
      pix[index - 1 * j_step] = bdfi_vp8_clip8(p0 + a2);
      pix[index + 0 * j_step] = bdfi_vp8_clip8(q0 - a1);
    } else if (four_not_six) {
      /* Filter 4 pixels. */
      int a = 3 * (q0 - p0);
      int a1 = bdfi_vp8_clamp15(bdfi_vp8_sar(a + 4, 3));
      int a2 = bdfi_vp8_clamp15(bdfi_vp8_sar(a + 3, 3));
      int a3 = bdfi_vp8_sar(a1 + 1, 1);
      pix[index - 2 * j_step] = bdfi_vp8_clip8(p1 + a3);
      pix[index - 1 * j_step] = bdfi_vp8_clip8(p0 + a2);
      pix[index + 0 * j_step] = bdfi_vp8_clip8(q0 - a1);
      pix[index + 1 * j_step] = bdfi_vp8_clip8(q1 - a3);
    } else {
      /* Filter 6 pixels. */
      int a = bdfi_vp8_clamp127(3 * (q0 - p0) + bdfi_vp8_clamp127(p1 - q1));
      int a1 = bdfi_vp8_sar(27 * a + 63, 7);
      int a2 = bdfi_vp8_sar(18 * a + 63, 7);
      int a3 = bdfi_vp8_sar(9 * a + 63, 7);
      pix[index - 3 * j_step] = bdfi_vp8_clip8(p2 + a3);
      pix[index - 2 * j_step] = bdfi_vp8_clip8(p1 + a2);
      pix[index - 1 * j_step] = bdfi_vp8_clip8(p0 + a1);
      pix[index + 0 * j_step] = bdfi_vp8_clip8(q0 - a1);
      pix[index + 1 * j_step] = bdfi_vp8_clip8(q1 - a2);
      pix[index + 2 * j_step] = bdfi_vp8_clip8(q2 - a3);
    }
  }
}

/* Decoder.simpleFilter (section 15.2) */
static void bdfi_vp8_simple_filter(bdfi_vp8_dec *d) {
  size_t ys = d->y_stride;
  for (int32_t mby = 0; mby < d->mbh; mby++) {
    for (int32_t mbx = 0; mbx < d->mbw; mbx++) {
      bdfi_vp8_filter_param f = d->per_mb_filter_params[(size_t)d->mbw * (size_t)mby + (size_t)mbx];
      if (f.level == 0) {
        continue;
      }
      int l = f.level;
      size_t y_index = ((size_t)mby * ys + (size_t)mbx) * 16;
      if (mbx > 0) {
        bdfi_vp8_filter2(d->img_y, l + 4, y_index, ys, 1);
      }
      if (f.inner) {
        bdfi_vp8_filter2(d->img_y, l, y_index + 0x4, ys, 1);
        bdfi_vp8_filter2(d->img_y, l, y_index + 0x8, ys, 1);
        bdfi_vp8_filter2(d->img_y, l, y_index + 0xc, ys, 1);
      }
      if (mby > 0) {
        bdfi_vp8_filter2(d->img_y, l + 4, y_index, 1, ys);
      }
      if (f.inner) {
        bdfi_vp8_filter2(d->img_y, l, y_index + ys * 0x4, 1, ys);
        bdfi_vp8_filter2(d->img_y, l, y_index + ys * 0x8, 1, ys);
        bdfi_vp8_filter2(d->img_y, l, y_index + ys * 0xc, 1, ys);
      }
    }
  }
}

/* Decoder.normalFilter (section 15.3) */
static void bdfi_vp8_normal_filter(bdfi_vp8_dec *d) {
  size_t ys = d->y_stride, cs = d->c_stride;
  for (int32_t mby = 0; mby < d->mbh; mby++) {
    for (int32_t mbx = 0; mbx < d->mbw; mbx++) {
      bdfi_vp8_filter_param f = d->per_mb_filter_params[(size_t)d->mbw * (size_t)mby + (size_t)mbx];
      if (f.level == 0) {
        continue;
      }
      int l = f.level, il = f.ilevel, hl = f.hlevel;
      size_t y_index = ((size_t)mby * ys + (size_t)mbx) * 16;
      size_t c_index = ((size_t)mby * cs + (size_t)mbx) * 8;
      if (mbx > 0) {
        bdfi_vp8_filter246(d->img_y, 16, l + 4, il, hl, y_index, ys, 1, 0);
        bdfi_vp8_filter246(d->img_cb, 8, l + 4, il, hl, c_index, cs, 1, 0);
        bdfi_vp8_filter246(d->img_cr, 8, l + 4, il, hl, c_index, cs, 1, 0);
      }
      if (f.inner) {
        bdfi_vp8_filter246(d->img_y, 16, l, il, hl, y_index + 0x4, ys, 1, 1);
        bdfi_vp8_filter246(d->img_y, 16, l, il, hl, y_index + 0x8, ys, 1, 1);
        bdfi_vp8_filter246(d->img_y, 16, l, il, hl, y_index + 0xc, ys, 1, 1);
        bdfi_vp8_filter246(d->img_cb, 8, l, il, hl, c_index + 0x4, cs, 1, 1);
        bdfi_vp8_filter246(d->img_cr, 8, l, il, hl, c_index + 0x4, cs, 1, 1);
      }
      if (mby > 0) {
        bdfi_vp8_filter246(d->img_y, 16, l + 4, il, hl, y_index, 1, ys, 0);
        bdfi_vp8_filter246(d->img_cb, 8, l + 4, il, hl, c_index, 1, cs, 0);
        bdfi_vp8_filter246(d->img_cr, 8, l + 4, il, hl, c_index, 1, cs, 0);
      }
      if (f.inner) {
        bdfi_vp8_filter246(d->img_y, 16, l, il, hl, y_index + ys * 0x4, 1, ys, 1);
        bdfi_vp8_filter246(d->img_y, 16, l, il, hl, y_index + ys * 0x8, 1, ys, 1);
        bdfi_vp8_filter246(d->img_y, 16, l, il, hl, y_index + ys * 0xc, 1, ys, 1);
        bdfi_vp8_filter246(d->img_cb, 8, l, il, hl, c_index + cs * 0x4, 1, cs, 1);
        bdfi_vp8_filter246(d->img_cr, 8, l, il, hl, c_index + cs * 0x4, 1, cs, 1);
      }
    }
  }
}

/* ---- The entry points ----------------------------------------------------- */

BDF_INTERNAL int bdfi_vp8_frame_header(const uint8_t *data, size_t avail, int64_t n, int32_t *w,
                                       int32_t *h) {
  bdfi_vp8_reader r;
  bdfi_vp8_fh fh;
  r.p = data;
  r.avail = avail;
  r.n = n;
  *w = 0;
  *h = 0;
  if (!bdfi_vp8_read_frame_header(&r, &fh)) {
    return 0;
  }
  *w = fh.width;
  *h = fh.height;
  return 1;
}

/* Decoder.DecodeFrameHeader and Decoder.DecodeFrame. What d has allocated
 * is the caller's to free. */
static bdf_status bdfi_vp8_decode(bdfi_vp8_dec *d, bdfi_vp8_frame *out) {
  if (!bdfi_vp8_read_frame_header(&d->r, &d->fh)) {
    return BDF_ERR_FORMAT;
  }
  /* Go parses the headers of a frame that is not a key frame and then, if
   * it found no error before, returns one ("Golden / AltRef frames are not
   * implemented"). */
  if (!d->fh.key_frame) {
    return BDF_ERR_FORMAT;
  }
  /* A key frame of no width or no height is an empty image in Go. */
  if (d->fh.width <= 0 || d->fh.height <= 0) {
    return BDF_ERR_FORMAT;
  }
  if ((int64_t)d->fh.width * d->fh.height > BDFI_MAX_DECODE_PIXELS) {
    return BDF_ERR_TOO_LARGE;
  }
  d->mbw = (d->fh.width + 0x0f) >> 4;
  d->mbh = (d->fh.height + 0x0f) >> 4;
  d->seg_prob[0] = 0xff;
  d->seg_prob[1] = 0xff;
  d->seg_prob[2] = 0xff;
  memcpy(d->token_prob, bdfi_vp8_default_token_prob, sizeof d->token_prob);
  d->segment = 0;

  /* Go makes the image first (ensureImg). The headers decide nothing about
   * it, so they are read before anything is allocated for it. */
  if (!bdfi_vp8_parse_other_headers(d)) {
    return BDF_ERR_FORMAT;
  }

  /* ensureImg: an image of whole macroblocks, 4:2:0, zeroed. */
  size_t mbw = (size_t)d->mbw, mbh = (size_t)d->mbh;
  size_t y_size = 16 * mbw * 16 * mbh, c_size = 8 * mbw * 8 * mbh;
  out->buf = bdfi_calloc_array(y_size + 2 * c_size, 1);
  d->per_mb_filter_params = bdfi_calloc_array(mbw * mbh, sizeof *d->per_mb_filter_params);
  d->up_mb = bdfi_calloc_array(mbw, sizeof *d->up_mb);
  if (!out->buf || !d->per_mb_filter_params || !d->up_mb) {
    return BDF_ERR_NOMEM;
  }
  d->img_y = out->buf;
  d->img_cb = out->buf + y_size;
  d->img_cr = out->buf + y_size + c_size;
  d->y_stride = 16 * mbw;
  d->c_stride = 8 * mbw;

  /* Reconstruct the rows. */
  for (int32_t mby = 0; mby < d->mbh; mby++) {
    memset(&d->left_mb, 0, sizeof d->left_mb);
    for (int32_t mbx = 0; mbx < d->mbw; mbx++) {
      int skip = bdfi_vp8_reconstruct(d, mbx, mby);
      bdfi_vp8_filter_param fs = d->filter_params[d->segment][d->use_pred_y16 ? 0 : 1];
      fs.inner = (uint8_t)(fs.inner || !skip);
      d->per_mb_filter_params[(size_t)d->mbw * (size_t)mby + (size_t)mbx] = fs;
    }
    /* Past the end of a partition Go reads zeros, goes on to the last
     * macroblock and returns an error then. No row after this one changes
     * that, so the error is returned here. */
    if (d->fp.eof || d->op[mby & (d->nop - 1)].eof) {
      return BDF_ERR_FORMAT;
    }
  }

  /* Apply the loop filter. */
  if (d->filter_level != 0) {
    if (d->filter_simple) {
      bdfi_vp8_simple_filter(d);
    } else {
      bdfi_vp8_normal_filter(d);
    }
  }
  out->width = d->fh.width;
  out->height = d->fh.height;
  out->y = d->img_y;
  out->cb = d->img_cb;
  out->cr = d->img_cr;
  out->y_stride = d->y_stride;
  out->c_stride = d->c_stride;
  return BDF_OK;
}

BDF_INTERNAL bdf_status bdfi_vp8_decode_frame(const uint8_t *data, size_t avail, int64_t n,
                                              bdfi_vp8_frame *out) {
  memset(out, 0, sizeof *out);
  /* a decoder is some kilobytes: not for the stack */
  bdfi_vp8_dec *d = calloc(1, sizeof *d);
  if (!d) {
    return BDF_ERR_NOMEM;
  }
  d->r.p = data;
  d->r.avail = avail;
  d->r.n = n;
  bdf_status st = bdfi_vp8_decode(d, out);
  free(d->per_mb_filter_params);
  free(d->up_mb);
  free(d);
  if (st != BDF_OK) {
    free(out->buf);
    memset(out, 0, sizeof *out);
  }
  return st;
}
