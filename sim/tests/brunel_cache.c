/* Exercises the actual cache implementation against uncached sequential
 * updates. Memory effects follow issue order; notifications deliberately complete newest first. */
#define BR_HOST_TEST
#include "../payloads/sys/brunel.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static uint32_t dram[65536];
static float expected[65536];
struct Request {
  uint32_t addr, id, carried, data, line[8];
} queue[16];
static unsigned queued, max_queued;
void br_send(uint32_t dst, uint32_t tag, uint32_t addr, uint32_t id,
             uint32_t carried, uint32_t data) {
  (void)dst;
  if (tag == 8) return;
  if (tag == 4) { br_reverse(addr, id, carried, data); return; }
  assert(tag == 0x800 || tag == 0x401);
  assert(queued < 12);
  struct Request *r = &queue[queued++];
  *r = (struct Request){.addr=addr, .id=id, .carried=carried, .data=data};
  if ((id & 65535) == 0x8003) dram[addr / 4] = data;
  else for (unsigned i=0;i<8;i++) r->line[i] = dram[addr / 4+i];
  if (queued > max_queued)
    max_queued = queued;
}
void br_test_progress(void) {
  assert(queued);
  struct Request r = queue[--queued];
  if ((r.id & 65535) == 0x8003) {
    br_completion(r.carried & 65535, r.data);
  } else {
    for (unsigned i = 0; i < 8; i++)
      ((uint32_t *)(uintptr_t)(BR_BASE + (r.carried & 65535) * 32))[i] = r.line[i];
    br_completion(r.carried & 65535, r.data);
  }
}
static unsigned rng = 19260817;
static unsigned random_test(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
static void reset_test(unsigned rule) {
  assert(!queued);
  for (unsigned i = 0; i < 4096; i++)
    ((uint32_t *)BR_BASE)[i] = 0;
  H->rule = rule;
  H->wmax = bits(91.2192f);
  max_queued = 0;
}

static void directed(unsigned rule) {
  reset_test(rule);
  const unsigned address = 0x10000;
  dram[address / 4] = bits(rule ? 1.0f : 90.0f);
  const F plus = bits(rule ? 0.25f : 10.0f);
  const F minus = bits(rule ? 0.5f : 10.0f);
  apply(address, plus, 1);
  BrEntry *e = lookup(address);
  assert((e->tag & BR_STATE) == BR_PENDING_LTP);
  assert(H->issued == 0); /* The first coefficient needs no DRAM read. */
  apply(address, minus, 0);
  e = lookup(address);
  const F want = e->data;
  assert(is_resident(e));
  /* Clip/apply in order; do not combine updates. Allow polynomial roundoff. */
  if (rule)
    assert(value(want) > 0.624998f && value(want) < 0.625002f);
  else
    assert(want == fsub(H->wmax, minus));
  assert(H->forced == 1);

  evict(e);
  e->tag = 0;
  assert(queued && H->issued != H->consumed);
  /* An immediate read observes the issued store without waiting for its ack. */
  e = resident(address);
  assert(e->data == want && dram[address / 4] == want);
  assert(queued && H->issued > H->consumed);
  while (queued) br_test_progress();
  apply(address, bits(200.0f), 0);
  assert(e->data == 0);
  evict(e);
  while (queued)
    br_test_progress();
  assert(dram[address / 4] == 0);
  /* A staged dirty value may be admitted again while its first write is
   * outstanding. A newer write takes effect after the older write at issue. */
  e->tag = 0;
  insert((BrEntry){address | BR_DIRTY, bits(3.0f)});
  e = lookup(address);
  evict(e);
  e->tag = 0;
  insert((BrEntry){address | BR_DIRTY, bits(4.0f)});
  evict(lookup(address));
  while (queued) br_test_progress();
  assert(dram[address / 4] == bits(4.0f));
  printf("rule %u: pending order, clipping, immediate write/read/write: PASS\n",
         rule);
}
static void fill_policy(unsigned rule) {
  reset_test(rule);
  unsigned addr = 0x10000;
  for (unsigned i = 0; i < 16; i++) dram[addr / 4 + i] = bits(10.0f + i);
  F weights[8];
  forward_weights(addr, 8, weights);
  assert(H->reserved[1] == 1); /* Eight misses share one memory transaction. */
  for (unsigned i = 0; i < 8; i++) assert(weights[i] == bits(10.0f + i));
  apply(addr + 4, bits(.5f), 1);
  F want = lookup(addr + 4)->data;
  forward_weights(addr, 8, weights);
  assert(H->reserved[1] == 1 && weights[1] == want);
  assert(dram[addr / 4 + 1] != want); /* Dirty cached data wins over DRAM. */
  apply(addr + 32, bits(.25f), 0);
  assert(H->reserved[1] == 1); /* One pending coefficient, no immediate read. */
  apply(addr + 32, bits(.5f), 1);
  assert(H->reserved[1] == 2);
  for (unsigned i = 9; i < 16; i++) assert(!lookup(addr + 4 * i));
  printf("rule %u: shared forward fill, dirty-hit precedence, single post admission: PASS\n", rule);
}
static void recording(unsigned rule) {
  reset_test(rule);
  H->nn=3; H->pu=1; H->phase=3; H->work_neuron=3;
  H->dep=bits(.1f); H->pot=bits(.1f);
  X->capacity=1; X->spill_base=0x20000;
  N[0].y=bits(1.f); N[1].y=bits(1.f); N[1].fire=1;
  N[2].pre_fire=1; N[2].x=bits(1.f);
  for(unsigned j=0;j<3;j++) dram[0x10000/4+j]=bits(10.f);
  br_spike(BR_NEURONS+68,bits(1.f),0x10000,1|(2u<<16));
  br_spike(BR_NEURONS+68,bits(1.f),0x10004,1|(2u<<16));
  br_spike(BR_NEURONS+80+68,bits(1.f),0x10008,1|(2u<<16));
  assert(X->count==3 && X->spilled==2 && H->received==3);
  while(queued) br_test_progress();
  br_work();
  assert(X->manual_reverse==2 && H->reverse_received==2);
  assert(lookup(0x10000) && lookup(0x10004) && !lookup(0x10008));
  /* The fired post is covered exactly once by its retained scatter. */
  br_reverse(2,0x10008,H->dep,H->pot);
  while(queued) br_test_progress();
  assert(H->reverse_received==3);
  printf("rule %u: recording, spill replay, fired-post deduplication: PASS\n",rule);
}
int main(void) {
  assert(mmap((void *)BR_BASE, 16384, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
              0) == (void *)BR_BASE);
  for (unsigned rule = 0; rule < 2; rule++) {
    directed(rule);
    fill_policy(rule);
    recording(rule);
    reset_test(rule);
    for (unsigned i = 0; i < 65536; i++) {
      expected[i] = 10 + (i % 101) * .3f;
      dram[i] = bits(expected[i]);
    }
    for (unsigned k = 0; k < 20000; k++) {
      unsigned a = 0x10000 + 4 * (random_test() % 8192),
               pot = random_test() & 1;
      float coefficient = (random_test() % 100 + 1) * .00001f;
      F w = bits(expected[a / 4]), c = bits(coefficient);
      /* Independent eager arithmetic; no tags, lane masks, or deferred state.
       */
      if (rule)
        w = pot ? fadd(w, fmul(c, pow04(w))) : fsub(w, fmul(c, w));
      else
        w = pot ? fadd(w, c) : fsub(w, c);
      w = fm_max(0, w);
      if (!rule)
        w = fm_min(w, H->wmax);
      expected[a / 4] = value(w);
      apply(a, c, pot);
      if (k % 7 == 0) {
        F weights[8];
        unsigned issued = H->issued;
        forward_weights(a & ~31u, 8, weights);
        /* One demand fill plus at most one read/write pair per admitted lane. */
        assert(H->issued - issued <= 17);
        for (unsigned lane = 0; lane < 8; lane++)
          assert(weights[lane] == bits(expected[(a & ~31u) / 4 + lane]));
      }
      for (unsigned slot = 0; slot < BR_SLOTS; slot++)
        if (T[slot].tag)
          assert(lookup(address(&T[slot])) == &T[slot]);
    }
    for (unsigned slot = 0; slot < BR_SLOTS; slot++)
      evict(&T[slot]);
    while (queued)
      br_test_progress();
    assert(H->issued == H->consumed);
    for (unsigned i = 0; i < 65536; i++)
      assert(dram[i] == bits(expected[i]));
    printf("rule %u: %u evictions, %u materializations, %u stores, max %u "
           "inflight: PASS\n",
           rule, H->evictions, H->forced, H->write_words, max_queued);
  }
}
