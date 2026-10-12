/*
 * The functions of Go's math package the rasterizer draws with, ported so
 * that it draws the same picture as the reference implementation, and the
 * same one on every platform: the C library's sine and arctangent differ
 * from Go's, and from each other, in the last bit.
 *
 * Ported from the Go standard library (src/math: sin.go, tan.go, atan.go,
 * atan2.go, asin.go, hypot.go, trig_reduce.go, dim.go, log2.go... as of Go
 * 1.27), which is
 *
 *   Copyright 2009 The Go Authors. All rights reserved.
 *   Use of this source code is governed by a BSD-style license.
 *
 * Its trigonometric functions come from the Cephes Math Library Release
 * 2.8, Copyright 1984, 1987, 1989, 1992, 2000 by Stephen L. Moshier, "may be
 * used freely but it comes with no support or guarantee". The full notices
 * are in third_party/go-math/LICENSE.
 *
 * The products and sums are those of the Go code in the same order, and the
 * file must be compiled without fused multiply-adds (see raster_internal.h).
 */
#include <math.h>
#include <string.h>

#include "raster_internal.h"

#define BDFI_PI4A 0.78539812564849854   /* 0x3fe921fb40000000: Pi/4 in three parts */
#define BDFI_PI4B 3.7748947079307982e-08 /* 0x3e64442d00000000 */
#define BDFI_PI4C 2.6951514290790595e-15 /* 0x3ce8469898cc5170 */
#define BDFI_4_PI 1.2732395447351628     /* 0x3ff45f306dc9c883: 4/Pi */
#define BDFI_LOG2E 1.4426950408889634    /* 0x3ff71547652b82fe: 1/Ln2 */

static const double bdfi_sin_c[6] = {
    1.58962301576546568060e-10, /* 0x3de5d8fd1fd19ccd */
    -2.50507477628578072866e-8, /* 0xbe5ae5e5a9291f5d */
    2.75573136213857245213e-6,  /* 0x3ec71de3567d48a1 */
    -1.98412698295895385996e-4, /* 0xbf2a01a019bfdf03 */
    8.33333333332211858878e-3,  /* 0x3f8111111110f7d0 */
    -1.66666666666666307295e-1, /* 0xbfc5555555555548 */
};
static const double bdfi_cos_c[6] = {
    -1.13585365213876817300e-11, /* 0xbda8fa49a0861a9b */
    2.08757008419747316778e-9,   /* 0x3e21ee9d7b4e3f05 */
    -2.75573141792967388112e-7,  /* 0xbe927e4f7eac4bc6 */
    2.48015872888517045348e-5,   /* 0x3efa01a019c844f5 */
    -1.38888888888730564116e-3,  /* 0xbf56c16c16c14f91 */
    4.16666666666665929218e-2,   /* 0x3fa555555555554b */
};

int bdfi_isnan(double v) { return v != v; }
int bdfi_isinf(double v) { return v == INFINITY || v == -INFINITY; }
int bdfi_signbit(double v) {
  uint64_t u;
  memcpy(&u, &v, sizeof u);
  return (int)(u >> 63);
}

static double bdfi_from_bits(uint64_t u) {
  double v;
  memcpy(&v, &u, sizeof v);
  return v;
}

static double bdfi_nan(void) { return bdfi_from_bits(0x7FF8000000000001ULL); }

/* The binary digits of 4/pi, 64 at a time. */
static const uint64_t bdfi_mpi4[20] = {
    0x0000000000000001ULL, 0x45f306dc9c882a53ULL, 0xf84eafa3ea69bb81ULL, 0xb6c52b3278872083ULL,
    0xfca2c757bd778ac3ULL, 0x6e48dc74849ba5c0ULL, 0x0c925dd413a32439ULL, 0xfc3bd63962534e7dULL,
    0xd1046bea5d768909ULL, 0xd338e04d68befc82ULL, 0x7323ac7306a673e9ULL, 0x3908bf177bf25076ULL,
    0x3ff12fffbc0b301fULL, 0xde5e2316b414da3eULL, 0xda6cfd9e4f96136eULL, 0x9e8c7ecd3cbfd45aULL,
    0xea4f758fd7cbe2f6ULL, 0x7a0e73ef14a525d4ULL, 0xd7f6bf623f1aba10ULL, 0xac06608df8f6d757ULL,
};

/* The product of two 64-bit numbers, in two halves. */
static void bdfi_mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
  uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
  uint64_t w0 = a0 * b0;
  uint64_t t = a1 * b0 + (w0 >> 32);
  uint64_t w1 = (t & 0xFFFFFFFFu) + a0 * b1;
  *hi = a1 * b1 + (t >> 32) + (w1 >> 32);
  *lo = a * b;
}

#define BDFI_REDUCE_THRESHOLD 536870912.0 /* 1 << 29 */

/* bdfi_trig_reduce is the reduction of a large x by Pi/4 (Payne and Hanek):
 * the octant and what is left. */
static uint64_t bdfi_trig_reduce(double x, double *z) {
  if (x < BDFI_PI_4) {
    *z = x;
    return 0;
  }
  uint64_t ix;
  memcpy(&ix, &x, sizeof ix);
  int exp = (int)((ix >> 52) & 0x7FF) - 1023 - 52;
  ix &= ~((uint64_t)0x7FF << 52);
  ix |= (uint64_t)1 << 52;
  unsigned digit = (unsigned)(exp + 61) / 64, bitshift = (unsigned)(exp + 61) % 64;
  /* a shift by 64 gives 0 in Go */
#define BDFI_SHR(v, n) ((n) >= 64 ? 0 : (v) >> (n))
  uint64_t z0 = (bdfi_mpi4[digit] << bitshift) | BDFI_SHR(bdfi_mpi4[digit + 1], 64 - bitshift);
  uint64_t z1 = (bdfi_mpi4[digit + 1] << bitshift) | BDFI_SHR(bdfi_mpi4[digit + 2], 64 - bitshift);
  uint64_t z2 = (bdfi_mpi4[digit + 2] << bitshift) | BDFI_SHR(bdfi_mpi4[digit + 3], 64 - bitshift);
  uint64_t z2hi, z2lo, z1hi, z1lo;
  bdfi_mul64(z2, ix, &z2hi, &z2lo);
  (void)z2lo;
  bdfi_mul64(z1, ix, &z1hi, &z1lo);
  uint64_t z0lo = z0 * ix;
  uint64_t lo = z1lo + z2hi;
  uint64_t carry = lo < z1lo;
  uint64_t hi = z0lo + z1hi + carry;
  uint64_t j = hi >> 61;
  hi = hi << 3 | lo >> 61;
  unsigned lz = 0;
  if (hi == 0) {
    lz = 64;
  } else {
    for (uint64_t probe = hi; !(probe >> 63); probe <<= 1) {
      lz++;
    }
  }
  uint64_t e = (uint64_t)(1023 - (lz + 1));
  hi = (lz + 1 >= 64 ? 0 : hi << (lz + 1)) | BDFI_SHR(lo, 64 - (lz + 1));
#undef BDFI_SHR
  hi >>= 64 - 52;
  hi |= e << 52;
  double f = bdfi_from_bits(hi);
  if (j & 1) {
    j++;
    j &= 7;
    f--;
  }
  *z = f * BDFI_PI_4;
  return j;
}

/* The octant of a positive x and what is left of it. */
static uint64_t bdfi_octant(double x, double *z, int wrap) {
  if (x >= BDFI_REDUCE_THRESHOLD) {
    return bdfi_trig_reduce(x, z);
  }
  uint64_t j = (uint64_t)(x * BDFI_4_PI);
  double y = (double)j;
  if (j & 1) {
    j++;
    y++;
  }
  if (wrap) {
    j &= 7;
  }
  *z = ((x - y * BDFI_PI4A) - y * BDFI_PI4B) - y * BDFI_PI4C;
  return j;
}

static double bdfi_sin_poly(double z, double zz) {
  return z + z * zz *
                 ((((((bdfi_sin_c[0] * zz) + bdfi_sin_c[1]) * zz + bdfi_sin_c[2]) * zz +
                    bdfi_sin_c[3]) * zz + bdfi_sin_c[4]) * zz + bdfi_sin_c[5]);
}

static double bdfi_cos_poly(double zz) {
  return 1.0 - 0.5 * zz +
         zz * zz *
             ((((((bdfi_cos_c[0] * zz) + bdfi_cos_c[1]) * zz + bdfi_cos_c[2]) * zz +
                bdfi_cos_c[3]) * zz + bdfi_cos_c[4]) * zz + bdfi_cos_c[5]);
}

void bdfi_sincos(double x, double *sn, double *cs) {
  if (x == 0) {
    *sn = x;
    *cs = 1;
    return;
  }
  if (bdfi_isnan(x) || bdfi_isinf(x)) {
    *sn = *cs = bdfi_nan();
    return;
  }
  int sin_sign = 0, cos_sign = 0;
  if (x < 0) {
    x = -x;
    sin_sign = 1;
  }
  double z;
  uint64_t j = bdfi_octant(x, &z, 1);
  if (j > 3) {
    j -= 4;
    sin_sign = !sin_sign;
    cos_sign = !cos_sign;
  }
  if (j > 1) {
    cos_sign = !cos_sign;
  }
  double zz = z * z;
  double c = bdfi_cos_poly(zz);
  double s = bdfi_sin_poly(z, zz);
  if (j == 1 || j == 2) {
    double t = s;
    s = c;
    c = t;
  }
  *cs = cos_sign ? -c : c;
  *sn = sin_sign ? -s : s;
}

double bdfi_sin(double x) {
  if (x == 0 || bdfi_isnan(x)) {
    return x;
  }
  if (bdfi_isinf(x)) {
    return bdfi_nan();
  }
  int sign = 0;
  if (x < 0) {
    x = -x;
    sign = 1;
  }
  double z;
  uint64_t j = bdfi_octant(x, &z, 1);
  if (j > 3) {
    sign = !sign;
    j -= 4;
  }
  double zz = z * z;
  double y = j == 1 || j == 2 ? bdfi_cos_poly(zz) : bdfi_sin_poly(z, zz);
  return sign ? -y : y;
}

double bdfi_cos(double x) {
  if (bdfi_isnan(x) || bdfi_isinf(x)) {
    return bdfi_nan();
  }
  int sign = 0;
  x = x < 0 ? -x : x == 0 ? 0.0 : x;
  double z;
  uint64_t j = bdfi_octant(x, &z, 1);
  if (j > 3) {
    j -= 4;
    sign = !sign;
  }
  if (j > 1) {
    sign = !sign;
  }
  double zz = z * z;
  double y = j == 1 || j == 2 ? bdfi_sin_poly(z, zz) : bdfi_cos_poly(zz);
  return sign ? -y : y;
}

double bdfi_tan(double x) {
  static const double p[3] = {
      -1.30936939181383777646e4, /* 0xc0c992d8d24f3f38 */
      1.15351664838587416140e6,  /* 0x413199eca5fc9ddd */
      -1.79565251976484877988e7, /* 0xc1711fead3299176 */
  };
  static const double q[5] = {
      1.00000000000000000000e0,  1.36812963470692954678e4, /* 0x40cab8a5eeb36572 */
      -1.32089234440210967447e6, /* 0xc13427bc582abc96 */
      2.50083801823357915839e7,  /* 0x4177d98fc2ead8ef */
      -5.38695755929454629881e7, /* 0xc189afe03cbe5a31 */
  };
  if (x == 0 || bdfi_isnan(x)) {
    return x;
  }
  if (bdfi_isinf(x)) {
    return bdfi_nan();
  }
  int sign = 0;
  if (x < 0) {
    x = -x;
    sign = 1;
  }
  double z;
  uint64_t j = bdfi_octant(x, &z, 0);
  double zz = z * z;
  double y;
  if (zz > 1e-14) {
    y = z + z * (zz * (((p[0] * zz) + p[1]) * zz + p[2]) /
                 ((((zz + q[1]) * zz + q[2]) * zz + q[3]) * zz + q[4]));
  } else {
    y = z;
  }
  if ((j & 2) == 2) {
    y = -1 / y;
  }
  return sign ? -y : y;
}

/* A series for the arctangent in [0, 0.66]. */
static double bdfi_xatan(double x) {
  const double p0 = -8.750608600031904122785e-01, p1 = -1.615753718733365076637e+01,
               p2 = -7.500855792314704667340e+01, p3 = -1.228866684490136173410e+02,
               p4 = -6.485021904942025371773e+01, q0 = +2.485846490142306297962e+01,
               q1 = +1.650270098316988542046e+02, q2 = +4.328810604912902668951e+02,
               q3 = +4.853903996359136964868e+02, q4 = +1.945506571482613964425e+02;
  double z = x * x;
  z = z * ((((p0 * z + p1) * z + p2) * z + p3) * z + p4) /
      (((((z + q0) * z + q1) * z + q2) * z + q3) * z + q4);
  z = x * z + x;
  return z;
}

/* The arctangent of a positive x. */
static double bdfi_satan(double x) {
  const double morebits = 6.123233995736765886130e-17; /* pi/2 = PIO2 + morebits */
  const double half_morebits = 3.061616997868383e-17;  /* 0x3c81a62633145c07 */
  const double tan3pio8 = 2.41421356237309504880;      /* tan(3*pi/8) */
  if (x <= 0.66) {
    return bdfi_xatan(x);
  }
  if (x > tan3pio8) {
    return BDFI_PI_2 - bdfi_xatan(1 / x) + morebits;
  }
  return BDFI_PI_4 + bdfi_xatan((x - 1) / (x + 1)) + half_morebits;
}

double bdfi_atan(double x) {
  if (x == 0) {
    return x;
  }
  if (x > 0) {
    return bdfi_satan(x);
  }
  return -bdfi_satan(-x);
}

static double bdfi_copysign(double v, double sign) {
  uint64_t a, b;
  memcpy(&a, &v, sizeof a);
  memcpy(&b, &sign, sizeof b);
  return bdfi_from_bits((a & ~((uint64_t)1 << 63)) | (b & ((uint64_t)1 << 63)));
}

double bdfi_atan2(double y, double x) {
  if (bdfi_isnan(y) || bdfi_isnan(x)) {
    return bdfi_nan();
  }
  if (y == 0) {
    if (x >= 0 && !bdfi_signbit(x)) {
      return bdfi_copysign(0, y);
    }
    return bdfi_copysign(BDFI_PI, y);
  }
  if (x == 0) {
    return bdfi_copysign(BDFI_PI_2, y);
  }
  if (bdfi_isinf(x)) {
    if (x > 0) {
      return bdfi_isinf(y) ? bdfi_copysign(BDFI_PI_4, y) : bdfi_copysign(0, y);
    }
    return bdfi_isinf(y) ? bdfi_copysign(BDFI_3PI_4, y) : bdfi_copysign(BDFI_PI, y);
  }
  if (bdfi_isinf(y)) {
    return bdfi_copysign(BDFI_PI_2, y);
  }
  double q = bdfi_atan(y / x);
  if (x < 0) {
    if (q <= 0) {
      return q + BDFI_PI;
    }
    return q - BDFI_PI;
  }
  return q;
}

double bdfi_asin(double x) {
  if (x == 0) {
    return x;
  }
  int sign = 0;
  if (x < 0) {
    x = -x;
    sign = 1;
  }
  if (x > 1) {
    return bdfi_nan();
  }
  double temp = sqrt(1 - x * x);
  if (x > 0.7) {
    temp = BDFI_PI_2 - bdfi_satan(temp / x);
  } else {
    temp = bdfi_satan(x / temp);
  }
  return sign ? -temp : temp;
}

double bdfi_acos(double x) { return BDFI_PI_2 - bdfi_asin(x); }

double bdfi_hypot(double p, double q) {
  p = p < 0 ? -p : p == 0 ? 0.0 : p;
  q = q < 0 ? -q : q == 0 ? 0.0 : q;
  if (p == INFINITY || q == INFINITY) {
    return INFINITY;
  }
  if (bdfi_isnan(p) || bdfi_isnan(q)) {
    return bdfi_nan();
  }
  if (p < q) {
    double t = p;
    p = q;
    q = t;
  }
  if (p == 0) {
    return 0;
  }
  q = q / p;
  return p * sqrt(1 + q * q);
}

/* math.Max and math.Min: an infinity of the right sign wins over a NaN, a
 * NaN over anything else, and +0 is more than -0. */
double bdfi_fmax(double x, double y) {
  if (x == INFINITY || y == INFINITY) {
    return INFINITY;
  }
  if (bdfi_isnan(x) || bdfi_isnan(y)) {
    return bdfi_nan();
  }
  if (x == 0 && x == y) {
    return bdfi_signbit(x) ? y : x;
  }
  return x > y ? x : y;
}

double bdfi_fmin(double x, double y) {
  if (x == -INFINITY || y == -INFINITY) {
    return -INFINITY;
  }
  if (bdfi_isnan(x) || bdfi_isnan(y)) {
    return bdfi_nan();
  }
  if (x == 0 && x == y) {
    return bdfi_signbit(x) ? x : y;
  }
  return x < y ? x : y;
}

/* math.Mod: the C library's fmod is exact, and so the same; Go gives a NaN
 * where C leaves the sign of one to the platform. */
double bdfi_mod(double x, double y) {
  if (y == 0 || bdfi_isinf(x) || bdfi_isnan(x) || bdfi_isnan(y)) {
    return bdfi_nan();
  }
  return fmod(x, y);
}

/* math.Log (log.go; the FreeBSD e_log.c it is made from is Sun's, see the
 * notices): the logarithm of a mantissa in [sqrt(2)/2, sqrt(2)) by a
 * polynomial, and the exponent times ln 2 in two parts. */
static double bdfi_log(double x) {
  const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10;
  const double l1 = 6.666666666666735130e-01, l2 = 3.999999999940941908e-01;
  const double l3 = 2.857142874366239149e-01, l4 = 2.222219843214978396e-01;
  const double l5 = 1.818357216161805012e-01, l6 = 1.531383769920937332e-01;
  const double l7 = 1.479819860511658591e-01;
  if (bdfi_isnan(x) || x == INFINITY) {
    return x;
  }
  if (x < 0) {
    return bdfi_nan();
  }
  if (x == 0) {
    return -INFINITY;
  }
  int ki;
  double f1 = frexp(x, &ki);
  if (f1 < BDFI_SQRT2 / 2) {
    f1 *= 2;
    ki--;
  }
  double f = f1 - 1;
  double k = (double)ki;
  double s = f / (2 + f);
  double s2 = s * s;
  double s4 = s2 * s2;
  double t1 = s2 * (l1 + s4 * (l3 + s4 * (l5 + s4 * l7)));
  double t2 = s4 * (l2 + s4 * (l4 + s4 * l6));
  double r = t1 + t2;
  double hfsq = 0.5 * f * f;
  return k * ln2_hi - ((hfsq - (s * (hfsq + r) + k * ln2_lo)) - f);
}

/* math.Log2 (log10.go): exact for the powers of two. */
double bdfi_log2(double x) {
  int exp = 0;
  double frac = x;
  if (!bdfi_isnan(x) && !bdfi_isinf(x) && x != 0) {
    frac = frexp(x, &exp);
  }
  if (frac == 0.5) {
    return (double)(exp - 1);
  }
  return bdfi_log(frac) * BDFI_LOG2E + (double)exp;
}

#ifndef BDF_IMPLEMENTATION
/* A product and a sum as this file computes them: the tests of the build ask
 * that it is not what a compiler makes of them when it fuses the two (see
 * raster_internal.h). */
double bdfi_r_muladd(double a, double b, double c) { return a * b + c; }
#endif
