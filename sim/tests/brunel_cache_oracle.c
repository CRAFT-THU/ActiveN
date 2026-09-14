/* ctypes oracle using the actual bare-metal C, with ordered, immediately
 * completed memory effects. The existing brunel_cache.c regression separately
 * tests multiple outstanding requests and reordered completion notifications. */
#define BR_HOST_TEST
#include "../payloads/sys/brunel.c"
#include <assert.h>
#include <sys/mman.h>
static uint32_t *oracle_dram;
void br_test_progress(void) { __builtin_trap(); }
void br_send(uint32_t dst, uint32_t tag, uint32_t addr, uint32_t id,
             uint32_t carried, uint32_t data) {
  (void)dst;
  if (tag == 0x401) {
    oracle_dram[addr / 4] = data;
    br_completion(carried & 65535, data);
  } else {
    assert(tag == 0x800 && (id & 65535) == 0x8002);
    for (unsigned lane = 0; lane < 8; ++lane)
      ((uint32_t *)(uintptr_t)(BR_BASE + carried * 32))[lane] = oracle_dram[addr / 4 + lane];
    br_completion(carried & 65535, data);
  }
}
void oracle_reset(uint32_t *dram, unsigned rule, uint32_t wmax) {
  assert(mmap((void *)BR_BASE, 16384, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)BR_BASE);
  oracle_dram = dram; H->rule = rule; H->wmax = wmax;
}
void oracle_apply(uint32_t addr, F c, uint32_t pot) { apply(addr, c, pot); }
void oracle_forward(uint32_t addr, unsigned count, F *out) {
  forward_weights(addr, count, out);
}
void oracle_export(uint32_t *spm) {
  for (unsigned i = 0; i < BR_DATA_END / 4; ++i)
    spm[i] = ((uint32_t *)BR_BASE)[i];
}
