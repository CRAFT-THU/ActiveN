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
/* Host mirror of the assembly's positive-finite w^(2/5) operation ordering.
 * One inverse-fifth-root refinement plus a tuned quadratic residual. Compile
 * with FP contraction disabled; all arithmetic rounds separately to FP32. */
static __attribute__((always_inline)) inline F pow04(F w) {
  if (w == 0)
    return 0;
  uint32_t scale = 0;
  if (w < 0x02000000u) {
    w = fmul(w, bits(0x1p100f));
    scale = 40u << 23;
  }
  F r = 0x4c2bac89u - w / 5u;
  F m = fmul(w, bits(0.2f));
  F r2 = fmul(r, r);
  F r4 = fmul(r2, r2);
  r = fsub(fmul(r, bits(1.20148f)), fmul(fmul(m, r2), r4));
  r2 = fmul(r, r);
  F q = fmul(fmul(w, r2), r);
  F d = fsub(bits(1.0f), fmul(q, r2));
  F correction = fadd(fmul(d, bits(0.48f)), bits(0.60002f));
  return fadd(q, fmul(fmul(q, d), correction)) - scale;
}
#endif
