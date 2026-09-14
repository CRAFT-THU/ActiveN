#ifndef BRUNEL_NUMERIC_H
#define BRUNEL_NUMERIC_H
#include <stdint.h>
typedef uint32_t F;
static __attribute__((always_inline)) inline F bits(float x) {
  union {
    float f;
    uint32_t u;
  } v = {.f = x};
  return v.u;
}
#ifdef __riscv
#define FOP(name, op)                                                          \
  static __attribute__((always_inline)) inline F name(F a, F b) {                                             \
    F r;                                                                       \
    __asm__(op " %0,%1,%2" : "=r"(r) : "r"(a), "r"(b));                        \
    return r;                                                                  \
  }
// clang-format off
FOP(fadd, "fadd.s")
FOP(fsub, "fsub.s")
FOP(fmul, "fmul.s")
FOP(fm_min, "fmin.s")
FOP(fm_max, "fmax.s")
// clang-format on
static __attribute__((always_inline)) inline F from_u(uint32_t a) {
  F r;
  __asm__("fcvt.s.wu %0,%1" : "=r"(r) : "r"(a));
  return r;
}
#else
static __attribute__((always_inline)) inline float value(F x) {
  union {
    float f;
    uint32_t u;
  } v = {.u = x};
  return v.f;
}
static __attribute__((always_inline)) inline F fadd(F a, F b) { return bits(value(a) + value(b)); }
static __attribute__((always_inline)) inline F fsub(F a, F b) { return bits(value(a) - value(b)); }
static __attribute__((always_inline)) inline F fmul(F a, F b) { return bits(value(a) * value(b)); }
static __attribute__((always_inline)) inline F fm_min(F a, F b) { return value(a) < value(b) ? a : b; }
static __attribute__((always_inline)) inline F fm_max(F a, F b) { return value(a) > value(b) ? a : b; }
static __attribute__((always_inline)) inline F from_u(uint32_t a) { return bits((float)a); }
#endif
/* Range-reduced binomial polynomial on m in [1,2): z=(m-1.5)/1.5.
 * Degree eight, |z|<=1/3. No division, iterative root, or DRAM lookup table. */
static __attribute__((always_inline)) inline F pow04(F w) {
  if (!(w & 0x7fffffffu))
    return 0;
  int exponent = (int)(w >> 23) - 127;
  if ((w >> 23) == 0) {
    w = fmul(w, bits(0x1p100f));
    exponent = (int)(w >> 23) - 227;
  }
  F m = (w & 0x7fffffu) | 0x3f800000u;
  F z = fmul(fsub(m, bits(1.5f)), bits(2.0f / 3.0f));
  F p = bits(-0.015155712f);
  p = fadd(bits(0.01837056f), fmul(z, p));
  p = fadd(bits(-0.0229632f), fmul(z, p));
  p = fadd(bits(0.029952f), fmul(z, p));
  p = fadd(bits(-0.0416f), fmul(z, p));
  p = fadd(bits(0.064f), fmul(z, p));
  p = fadd(bits(-0.12f), fmul(z, p));
  p = fadd(bits(0.4f), fmul(z, p));
  p = fadd(bits(1.0f), fmul(z, p));
  p = fmul(p, bits(1.1760790225246736f));
  int q = 0;
  while (exponent < 0) {
    exponent += 5;
    q -= 2;
  }
  while (exponent >= 5) {
    exponent -= 5;
    q += 2;
  }
  F scale = bits(1.0f);
  if (exponent == 1)
    scale = bits(1.3195079107728942f);
  if (exponent == 2)
    scale = bits(1.7411011265922482f);
  if (exponent == 3)
    scale = bits(2.29739670999407f);
  if (exponent == 4)
    scale = bits(3.031433133020796f);
  return fmul(fmul(p, scale), (uint32_t)(q + 127) << 23);
}
#endif
