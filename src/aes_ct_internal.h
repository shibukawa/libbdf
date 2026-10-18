/*
 * Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Adapted from BearSSL src/symcipher/aes_ct.c and aes_ct64.c. Only the
 * S-box circuit and bit transpose are used, for words of 32 and of 64 bits;
 * see third_party/bearssl/README.md. */
#ifndef BDF_AES_CT_INTERNAL_H
#define BDF_AES_CT_INTERNAL_H
#include <stdint.h>

/* The circuit for words of 32 bits (bdfi_aes_sbox_bitslice: four bytes of
 * one block) and of 64 bits (bdfi_aes64_sbox: a bit of sixteen bytes of
 * four blocks), defined once. */
#define BDFI_AES_DEFINE_SBOX(name, word)                                                          \
  static void name(word *q) {                                                                     \
    /*                                                                                              \
     * This S-box implementation is a straightforward translation of                                \
     * the circuit described by Boyar and Peralta in "A new                                         \
     * combinational logic minimization technique with applications                                 \
     * to cryptology" (https://eprint.iacr.org/2009/191.pdf).                                       \
     *                                                                                              \
     * Note that variables x* (input) and s* (output) are numbered                                  \
     * in "reverse" order (x0 is the high bit, x7 is the low bit).                                  \
     */                                                                                             \
                                                                                                    \
    word x0, x1, x2, x3, x4, x5, x6, x7;                                                            \
    word y1, y2, y3, y4, y5, y6, y7, y8, y9;                                                        \
    word y10, y11, y12, y13, y14, y15, y16, y17, y18, y19;                                          \
    word y20, y21;                                                                                  \
    word z0, z1, z2, z3, z4, z5, z6, z7, z8, z9;                                                    \
    word z10, z11, z12, z13, z14, z15, z16, z17;                                                    \
    word t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;                                                    \
    word t10, t11, t12, t13, t14, t15, t16, t17, t18, t19;                                          \
    word t20, t21, t22, t23, t24, t25, t26, t27, t28, t29;                                          \
    word t30, t31, t32, t33, t34, t35, t36, t37, t38, t39;                                          \
    word t40, t41, t42, t43, t44, t45, t46, t47, t48, t49;                                          \
    word t50, t51, t52, t53, t54, t55, t56, t57, t58, t59;                                          \
    word t60, t61, t62, t63, t64, t65, t66, t67;                                                    \
    word s0, s1, s2, s3, s4, s5, s6, s7;                                                            \
                                                                                                    \
    x0 = q[7];                                                                                      \
    x1 = q[6];                                                                                      \
    x2 = q[5];                                                                                      \
    x3 = q[4];                                                                                      \
    x4 = q[3];                                                                                      \
    x5 = q[2];                                                                                      \
    x6 = q[1];                                                                                      \
    x7 = q[0];                                                                                      \
                                                                                                    \
    /*                                                                                              \
     * Top linear transformation.                                                                   \
     */                                                                                             \
    y14 = x3 ^ x5;                                                                                  \
    y13 = x0 ^ x6;                                                                                  \
    y9 = x0 ^ x3;                                                                                   \
    y8 = x0 ^ x5;                                                                                   \
    t0 = x1 ^ x2;                                                                                   \
    y1 = t0 ^ x7;                                                                                   \
    y4 = y1 ^ x3;                                                                                   \
    y12 = y13 ^ y14;                                                                                \
    y2 = y1 ^ x0;                                                                                   \
    y5 = y1 ^ x6;                                                                                   \
    y3 = y5 ^ y8;                                                                                   \
    t1 = x4 ^ y12;                                                                                  \
    y15 = t1 ^ x5;                                                                                  \
    y20 = t1 ^ x1;                                                                                  \
    y6 = y15 ^ x7;                                                                                  \
    y10 = y15 ^ t0;                                                                                 \
    y11 = y20 ^ y9;                                                                                 \
    y7 = x7 ^ y11;                                                                                  \
    y17 = y10 ^ y11;                                                                                \
    y19 = y10 ^ y8;                                                                                 \
    y16 = t0 ^ y11;                                                                                 \
    y21 = y13 ^ y16;                                                                                \
    y18 = x0 ^ y16;                                                                                 \
                                                                                                    \
    /*                                                                                              \
     * Non-linear section.                                                                          \
     */                                                                                             \
    t2 = y12 & y15;                                                                                 \
    t3 = y3 & y6;                                                                                   \
    t4 = t3 ^ t2;                                                                                   \
    t5 = y4 & x7;                                                                                   \
    t6 = t5 ^ t2;                                                                                   \
    t7 = y13 & y16;                                                                                 \
    t8 = y5 & y1;                                                                                   \
    t9 = t8 ^ t7;                                                                                   \
    t10 = y2 & y7;                                                                                  \
    t11 = t10 ^ t7;                                                                                 \
    t12 = y9 & y11;                                                                                 \
    t13 = y14 & y17;                                                                                \
    t14 = t13 ^ t12;                                                                                \
    t15 = y8 & y10;                                                                                 \
    t16 = t15 ^ t12;                                                                                \
    t17 = t4 ^ t14;                                                                                 \
    t18 = t6 ^ t16;                                                                                 \
    t19 = t9 ^ t14;                                                                                 \
    t20 = t11 ^ t16;                                                                                \
    t21 = t17 ^ y20;                                                                                \
    t22 = t18 ^ y19;                                                                                \
    t23 = t19 ^ y21;                                                                                \
    t24 = t20 ^ y18;                                                                                \
                                                                                                    \
    t25 = t21 ^ t22;                                                                                \
    t26 = t21 & t23;                                                                                \
    t27 = t24 ^ t26;                                                                                \
    t28 = t25 & t27;                                                                                \
    t29 = t28 ^ t22;                                                                                \
    t30 = t23 ^ t24;                                                                                \
    t31 = t22 ^ t26;                                                                                \
    t32 = t31 & t30;                                                                                \
    t33 = t32 ^ t24;                                                                                \
    t34 = t23 ^ t33;                                                                                \
    t35 = t27 ^ t33;                                                                                \
    t36 = t24 & t35;                                                                                \
    t37 = t36 ^ t34;                                                                                \
    t38 = t27 ^ t36;                                                                                \
    t39 = t29 & t38;                                                                                \
    t40 = t25 ^ t39;                                                                                \
                                                                                                    \
    t41 = t40 ^ t37;                                                                                \
    t42 = t29 ^ t33;                                                                                \
    t43 = t29 ^ t40;                                                                                \
    t44 = t33 ^ t37;                                                                                \
    t45 = t42 ^ t41;                                                                                \
    z0 = t44 & y15;                                                                                 \
    z1 = t37 & y6;                                                                                  \
    z2 = t33 & x7;                                                                                  \
    z3 = t43 & y16;                                                                                 \
    z4 = t40 & y1;                                                                                  \
    z5 = t29 & y7;                                                                                  \
    z6 = t42 & y11;                                                                                 \
    z7 = t45 & y17;                                                                                 \
    z8 = t41 & y10;                                                                                 \
    z9 = t44 & y12;                                                                                 \
    z10 = t37 & y3;                                                                                 \
    z11 = t33 & y4;                                                                                 \
    z12 = t43 & y13;                                                                                \
    z13 = t40 & y5;                                                                                 \
    z14 = t29 & y2;                                                                                 \
    z15 = t42 & y9;                                                                                 \
    z16 = t45 & y14;                                                                                \
    z17 = t41 & y8;                                                                                 \
                                                                                                    \
    /*                                                                                              \
     * Bottom linear transformation.                                                                \
     */                                                                                             \
    t46 = z15 ^ z16;                                                                                \
    t47 = z10 ^ z11;                                                                                \
    t48 = z5 ^ z13;                                                                                 \
    t49 = z9 ^ z10;                                                                                 \
    t50 = z2 ^ z12;                                                                                 \
    t51 = z2 ^ z5;                                                                                  \
    t52 = z7 ^ z8;                                                                                  \
    t53 = z0 ^ z3;                                                                                  \
    t54 = z6 ^ z7;                                                                                  \
    t55 = z16 ^ z17;                                                                                \
    t56 = z12 ^ t48;                                                                                \
    t57 = t50 ^ t53;                                                                                \
    t58 = z4 ^ t46;                                                                                 \
    t59 = z3 ^ t54;                                                                                 \
    t60 = t46 ^ t57;                                                                                \
    t61 = z14 ^ t57;                                                                                \
    t62 = t52 ^ t58;                                                                                \
    t63 = t49 ^ t58;                                                                                \
    t64 = z4 ^ t59;                                                                                 \
    t65 = t61 ^ t62;                                                                                \
    t66 = z1 ^ t63;                                                                                 \
    s0 = t59 ^ t63;                                                                                 \
    s6 = t56 ^ ~t62;                                                                                \
    s7 = t48 ^ ~t60;                                                                                \
    t67 = t64 ^ t65;                                                                                \
    s3 = t53 ^ t66;                                                                                 \
    s4 = t51 ^ t66;                                                                                 \
    s5 = t47 ^ t65;                                                                                 \
    s1 = t64 ^ ~s3;                                                                                 \
    s2 = t55 ^ ~t67;                                                                                \
                                                                                                    \
    q[7] = s0;                                                                                      \
    q[6] = s1;                                                                                      \
    q[5] = s2;                                                                                      \
    q[4] = s3;                                                                                      \
    q[3] = s4;                                                                                      \
    q[2] = s5;                                                                                      \
    q[1] = s6;                                                                                      \
    q[0] = s7;                                                                                      \
  }

BDFI_AES_DEFINE_SBOX(bdfi_aes_sbox_bitslice, uint32_t)
BDFI_AES_DEFINE_SBOX(bdfi_aes64_sbox, uint64_t)

#define BDFI_AES_SWAPN(word, cl, ch, s, x, y)                                                      \
  do {                                                                                             \
    word a, b;                                                                                     \
    a = (x);                                                                                       \
    b = (y);                                                                                       \
    (x) = (a & (word)(cl)) | ((b & (word)(cl)) << (s));                                            \
    (y) = ((a & (word)(ch)) >> (s)) | (b & (word)(ch));                                            \
  } while (0)

/* The transpose for words of 32 bits (bdfi_aes_ortho) and of 64 bits
 * (bdfi_aes64_ortho), with the masks of each. */
#define BDFI_AES_DEFINE_ORTHO(name, word, m2l, m2h, m4l, m4h, m8l, m8h)                             \
  static void name(word *q) {                                                                      \
    BDFI_AES_SWAPN(word, m2l, m2h, 1, q[0], q[1]);                                                 \
    BDFI_AES_SWAPN(word, m2l, m2h, 1, q[2], q[3]);                                                 \
    BDFI_AES_SWAPN(word, m2l, m2h, 1, q[4], q[5]);                                                 \
    BDFI_AES_SWAPN(word, m2l, m2h, 1, q[6], q[7]);                                                 \
    BDFI_AES_SWAPN(word, m4l, m4h, 2, q[0], q[2]);                                                 \
    BDFI_AES_SWAPN(word, m4l, m4h, 2, q[1], q[3]);                                                 \
    BDFI_AES_SWAPN(word, m4l, m4h, 2, q[4], q[6]);                                                 \
    BDFI_AES_SWAPN(word, m4l, m4h, 2, q[5], q[7]);                                                 \
    BDFI_AES_SWAPN(word, m8l, m8h, 4, q[0], q[4]);                                                 \
    BDFI_AES_SWAPN(word, m8l, m8h, 4, q[1], q[5]);                                                 \
    BDFI_AES_SWAPN(word, m8l, m8h, 4, q[2], q[6]);                                                 \
    BDFI_AES_SWAPN(word, m8l, m8h, 4, q[3], q[7]);                                                 \
  }

BDFI_AES_DEFINE_ORTHO(bdfi_aes_ortho, uint32_t, 0x55555555u, 0xAAAAAAAAu, 0x33333333u, 0xCCCCCCCCu,
                      0x0F0F0F0Fu, 0xF0F0F0F0u)
BDFI_AES_DEFINE_ORTHO(bdfi_aes64_ortho, uint64_t, 0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL,
                      0x3333333333333333ULL, 0xCCCCCCCCCCCCCCCCULL, 0x0F0F0F0F0F0F0F0FULL,
                      0xF0F0F0F0F0F0F0F0ULL)

#endif
