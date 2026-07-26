  # Verify fixed configuration-ROM addresses across memory-line boundaries.

#ifndef NUM_PU
#define NUM_PU 64
#endif
#ifndef NUM_MC
#define NUM_MC 2
#endif
#ifndef MEM_LINE_BYTES
#define MEM_LINE_BYTES 64
#endif

#if MEM_LINE_BYTES == 32
#define MEM_LINE_SHIFT 5
#elif MEM_LINE_BYTES == 64
#define MEM_LINE_SHIFT 6
#else
#error "config_rom_test supports 32-byte and 64-byte memory lines"
#endif

#define CONFIG_ROM_ADDR 0x68000000
#define RESULT_ADDR     0x4000000C
#define STOP_ADDR       0x40000000

  .section .text
  .globl _start

_start:
  bnez  x10, done
  csrr  t0, 0xF14
  li    t1, 1
  bne   t0, t1, done

  li    t0, CONFIG_ROM_ADDR
  lw    t1, 0(t0)
  li    t2, NUM_PU
  bne   t1, t2, fail_num_pu
  lw    t1, 8(t0)
  li    t2, NUM_MC
  bne   t1, t2, fail_num_mc
  lw    t1, 16(t0)
  li    t2, (NUM_PU / NUM_MC)
  bne   t1, t2, fail_pus_per_mc
  lw    t1, 24(t0)
  li    t2, MEM_LINE_SHIFT
  bne   t1, t2, fail_line_shift

  # The configured per-MC size is 4 GiB, encoded little-endian at 0x20.
  lw    t1, 32(t0)
  bnez  t1, fail_mc_size_low
  lw    t1, 36(t0)
  li    t2, 1
  bne   t1, t2, fail_mc_size_high

  li    t0, STOP_ADDR
  sw    zero, 0(t0)

done:
  wfi

fail_num_pu:
  li    t3, 1
  j     fail
fail_num_mc:
  li    t3, 2
  j     fail
fail_pus_per_mc:
  li    t3, 3
  j     fail
fail_line_shift:
  li    t3, 4
  j     fail
fail_mc_size_low:
  li    t3, 5
  j     fail
fail_mc_size_high:
  li    t3, 6

fail:
  li    t0, RESULT_ADDR
  sw    t3, 0(t0)
  li    t0, STOP_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  j     done
