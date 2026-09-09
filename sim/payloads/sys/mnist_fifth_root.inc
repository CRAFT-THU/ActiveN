/* Positive finite FP32 fifth root; input/output a0, return t6.
 * Scratch: a1-a3,t1-t3. All other registers, including the seven phase
 * constants, are preserved. Incoming payloads must already be saved by caller.
 *
 * q=x*r^4, t=q*r; root=q*(B + .72*(t-C)^2).
 * LUT: C=14/9, B=7/9. Bit seed: C/B include the biased refinement correction.
 * Separate FP32 operations; no FMA, division, or stack. Tiny inputs are scaled
 * exactly by 2^100 and the final exponent is reduced by 20.
 */
/* Stable cache-line entry, independent of the selected learning rule. */
  .balign 64
fifth_root:
  blez  a0, fifth_root_zero
  li    a3, 0
  li    t1, 0x02000000
  bgeu  a0, t1, fifth_root_seed
  li    t1, 0x71800000
  fmul.s a0, a0, t1
  li    a3, 0x0a000000
fifth_root_seed:
#if MNIST_FIFTH_ROOT_LUT
  /* 256 exponent factors, followed by 128 midpoint mantissa seeds. */
  srli  t1, a0, 21
  andi  t1, t1, 1020
  add   t1, t1, ROOT_DIVISOR
  lw    a1, 0(t1)
  srli  t1, a0, 14
  andi  t1, t1, 508
  add   t1, t1, ROOT_DIVISOR
  lw    a2, 1024(t1)
  fmul.s a1, a1, a2
#else
  mulhu t1, a0, ROOT_DIVISOR
  sub   a1, ROOT_MAGIC, t1
  fmul.s a2, a0, ROOT_FIFTH
  fmul.s t1, a1, a1
  fmul.s t2, t1, t1
  fmul.s t3, a2, t1
  fmul.s t3, t3, t2
  fmul.s a1, a1, ROOT_NEWTON
  fsub.s a1, a1, t3
#endif
  fmul.s t1, a1, a1
  fmul.s t2, t1, t1
  fmul.s a0, a0, t2
  fmul.s a1, a0, a1
  fsub.s a1, a1, ROOT_SQUARE_CENTER
  fmul.s a1, a1, a1
  fmul.s a1, a1, ROOT_SQUARE_SCALE
  fadd.s a1, a1, ROOT_SQUARE_BASE
  fmul.s a0, a0, a1
  sub   a0, a0, a3
  jr    t6
fifth_root_zero:
  li    a0, 0
  jr    t6
