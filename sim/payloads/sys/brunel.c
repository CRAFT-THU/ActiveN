#include "brunel.h"
#include "brunel_numeric.h"
#define H ((volatile BrHeader *)BR_BASE)
#define N ((volatile BrNeuron *)(BR_BASE + BR_NEURONS))
#define T ((BrEntry *)(BR_BASE + BR_TABLE))
#define X ((volatile BrReplay *)(BR_BASE + BR_SCRATCH))
#define RECORDS ((volatile BrEntry *)(BR_BASE + BR_RECORDS))
#define R(i) ((volatile uint32_t *)(uintptr_t)(BR_BASE + BR_READ + 32 * (i)))
extern void br_send(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#ifdef BR_HOST_TEST
extern void br_test_progress(void);
#define BR_RELAX() br_test_progress()
static void stop(uint32_t code) {
  (void)code;
  __builtin_trap();
}
#else
#define BR_RELAX() __asm__ volatile("" ::: "memory")
static void stop(uint32_t code) {
  *(volatile uint32_t *)0x40000000u = code;
  for (;;)
    __asm__ volatile("wfi");
}
#endif
void *memcpy(void *d, const void *s, unsigned n) {
  unsigned char *a = d;
  const unsigned char *b = s;
  while (n--)
    *a++ = *b++;
  return d;
}
static __attribute__((always_inline)) inline void send(uint32_t dst, uint32_t tag, uint32_t a, uint32_t b, uint32_t c,
                 uint32_t d) {
  br_send(dst, tag, a, b, c, d);
}
static __attribute__((always_inline)) inline void continuation(void) { send(0, 8, 0, 0, 0, 0); }
static __attribute__((always_inline)) inline uint32_t mc_dst(void) { return 0x8001u + H->mc; }
static void credit(void) {
  while (H->issued - H->consumed >= BR_CREDITS)
    BR_RELAX();
  H->issued++;
}
static void load_start(uint32_t addr, uint32_t slot) {
  H->ready[slot] = 0;
  credit();
  send(mc_dst(), 0x800, addr, (5u << 16) | 0x8002u, (BR_READ + slot * 32) >> 5,
       slot);
}
static void load_wait(uint32_t slot) {
  while (!H->ready[slot])
    BR_RELAX();
}
void br_completion(uint32_t ident, uint32_t carried) {
  if (!(ident & 0x8000u)) {
    if (carried > 3) stop(0xb002);
    H->ready[carried] = 1;
  }
  H->consumed++;
}
static void store_word(uint32_t addr, uint32_t value) {
  credit();
  send(mc_dst(), 0x401, addr, (2u << 16) | 0x8003u, 0x8000u, value);
}
/* After the local recording buffer fills, dispatch spikes on SMT0. Its
 * blocking credit waits can then progress through SMT1's completion handler.
 * The outgoing-message quota is two stores even for ordinary spike handlers. */
static void spike_thread(uint32_t config) {
#ifndef BR_HOST_TEST
  __asm__ volatile("csrw 0x711,%0" :: "r"(config) : "memory");
#else
  (void)config;
#endif
}
__attribute__((optimize("Os"))) void br_spike(uint32_t offset, uint32_t weight, uint32_t address,
              uint32_t source) {
  if (offset < BR_NEURONS || offset >= BR_TABLE) stop(0xb003);
  volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(BR_BASE + offset);
  *p = fadd(*p, weight);
  if (address) {
    uint32_t index = X->count;
    uint32_t meta = source | ((uint32_t)(((uint64_t)(offset - BR_NEURONS) * 0xcccccccdu) >> 38) << 24);
    if (index < X->capacity) {
      RECORDS[index] = (BrEntry){address, meta};
    } else {
      if (index >= BR_MAX_RECORDS) stop(0xb00c);
      uint32_t dst = X->spill_base + 8 * (index - X->capacity);
      store_word(dst, address);
      store_word(dst + 4, meta);
      X->spilled++;
    }
    X->count = index + 1;
    if (X->peak < index + 1) X->peak = index + 1;
  }
  H->received++;
  if (address && X->count == X->capacity) spike_thread(12);
}
static F update(F w, F coefficient, uint32_t pot) {
  if (H->rule == 0)
    w = pot ? fadd(w, coefficient) : fsub(w, coefficient);
  else
    w = pot ? fadd(w, fmul(coefficient, pow04(w)))
            : fsub(w, fmul(coefficient, w));
  w = fm_max(0, w);
  if (!H->rule)
    w = fm_min(w, H->wmax);
  return w;
}
static __attribute__((always_inline)) inline uint32_t address(const BrEntry *e) { return e->tag & ~BR_STATE; }
static __attribute__((always_inline)) inline uint32_t is_resident(const BrEntry *e) { return !(e->tag & 2u); }
static void weight_load(uint32_t addr, uint32_t slot) {
  H->reserved[1]++;
  load_start(addr & ~31u, slot);
}
static void materialize(BrEntry *e) {
  if (is_resident(e))
    return;
  uint32_t addr = address(e);
  weight_load(addr, 0);
  load_wait(0);
  e->data = update(R(0)[(addr >> 2) & 7u], e->data,
                   (e->tag & BR_STATE) == BR_PENDING_LTP);
  e->tag = addr | BR_DIRTY;
}
static void evict(BrEntry *e) {
  if (!e->tag)
    return;
  H->evictions++;
  if (!is_resident(e)) {
    H->forced++;
    materialize(e);
  }
  if ((e->tag & BR_STATE) == BR_CLEAN)
    return;
  /* Ordered memory effects are the workload contract. No per-line
   * acknowledgment dependency is imposed between writeback and refill. */
  store_word(address(e), e->data);
  H->write_words++;
}
static __attribute__((always_inline)) inline uint32_t home(uint32_t addr) {
  uint32_t h = (addr >> 2) * 2654435761u;
  return ((h >> 16) * BR_SLOTS) >> 16;
}
static __attribute__((always_inline)) inline uint32_t position(uint32_t addr, uint32_t distance) {
  uint32_t p = home(addr) + distance;
  return p >= BR_SLOTS ? p - BR_SLOTS : p;
}
static __attribute__((always_inline)) inline uint32_t distance(uint32_t addr, uint32_t pos) {
  uint32_t h = home(addr);
  return pos >= h ? pos - h : pos + BR_SLOTS - h;
}
static BrEntry *lookup(uint32_t addr) {
  for (uint32_t d = 0; d < 4; d++) {
    H->probes++;
    BrEntry *e = &T[position(addr, d)];
    if (address(e) == addr)
      return e;
  }
  return 0;
}
/* Four probes per complete admission operation, including all displacements.
 * Derive distance from the address/position to keep entries exactly eight bytes.
 * The caller supplies a complete value; no eviction can see a placeholder. */
static void insert(BrEntry entry) {
  uint32_t pos = home(address(&entry));
  BrEntry held = T[pos];
  T[pos] = entry;
  H->probes++;
  uint32_t d = held.tag ? distance(address(&held), pos) : 0;
  for (uint32_t budget = 1; held.tag && budget < 4; budget++) {
    if (++d == 4)
      break;
    pos = position(address(&held), d);
    BrEntry *at = &T[pos];
    H->probes++;
    if (!at->tag) {
      *at = held;
      held.tag = 0;
      break;
    }
    uint32_t old_distance = distance(address(at), pos);
    if (old_distance <= d) {
      BrEntry old = *at;
      *at = held;
      held = old;
      d = old_distance;
    }
  }
  evict(&held);
}
#ifdef BR_HOST_TEST
static BrEntry *resident(uint32_t addr) {
  BrEntry *e = lookup(addr);
  if (e) {
    materialize(e);
    return e;
  }
  weight_load(addr, 0);
  load_wait(0);
  insert((BrEntry){addr | BR_CLEAN, R(0)[(addr >> 2) & 7u]});
  return lookup(addr);
}
#endif
static void apply(uint32_t addr, F c, uint32_t pot) {
  if (!(c & 0x7fffffffu))
    return;
  BrEntry *e = lookup(addr);
  if (e && is_resident(e)) {
    H->hits++;
    e->data = update(e->data, c, pot);
    e->tag = addr | BR_DIRTY;
    return;
  }
  H->misses++;
  if (e) {
    H->forced++;
    materialize(e);
    e->data = update(e->data, c, pot);
  } else {
    insert((BrEntry){addr | (pot ? BR_PENDING_LTP : BR_PENDING_LTD), c});
    H->pending++;
  }
}
/* Stage a whole firing row segment before admission can evict any of its
 * entries. R(3) is isolated from eviction/materialization's R(0). Resolve all
 * pending entries in place first, so an intervening eviction cannot apply a
 * coefficient twice or make a later lane consume stale fetched data. */
static void forward_weights(uint32_t addr, uint32_t count, F *weights) {
  BrEntry staged[8];
  uint32_t need_load = 0, missing = 0;
  for (uint32_t lane = 0; lane < count; lane++) {
    BrEntry *e = lookup(addr + 4 * lane);
    staged[lane] = e ? *e : (BrEntry){0, 0};
    if (!e) missing |= 1u << lane;
    if (e && is_resident(e))
      H->hits++;
    else {
      H->misses++;
      need_load = 1;
    }
  }
  if (need_load) {
    weight_load(addr, 3);
    load_wait(3);
  }
  for (uint32_t lane = 0; lane < count; lane++) {
    BrEntry *e = &staged[lane];
    if (!e->tag)
      *e = (BrEntry){(addr + 4 * lane) | BR_CLEAN, R(3)[lane]};
    else if (!is_resident(e)) {
      e->data = update(R(3)[lane], e->data,
                       (e->tag & BR_STATE) == BR_PENDING_LTP);
      e->tag = (addr + 4 * lane) | BR_DIRTY;
      *lookup(addr + 4 * lane) = *e;
    }
    weights[lane] = e->data;
  }
  for (uint32_t lane = 0; lane < count; lane++)
    if (missing & (1u << lane))
      insert(staged[lane]);
}
void br_reverse(uint32_t local, uint32_t address, uint32_t minus,
                uint32_t plus) {
  if (local >= H->nn)
    stop(0xb005);
  if (N[local].pre_fire)
    apply(address, minus, 0);
  if (plus)
    apply(address, fmul(plus, N[local].x), 1);
  H->reverse_received++;
  H->reverse_events++;
}
static uint32_t random_word(volatile uint32_t *p) {
  uint32_t x = *p;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *p = x;
  return x;
}
static uint32_t poisson(volatile uint32_t *rng) {
  F product = bits(1.0f);
  uint32_t k = 0;
  do {
    product = fmul(product,
                   fmul(from_u((random_word(rng) >> 8) + 1), bits(0x1p-24f)));
    k++;
  } while (product > H->external_limit);
  return k - 1;
}
static void integrate(volatile BrNeuron *n) {
  if (!n->refrac) {
    F v = fadd(fmul(H->p31, n->de), fmul(H->p32, n->ie));
    v = fadd(v, fmul(H->p31, n->di));
    v = fadd(v, fmul(H->p32, n->ii));
    n->v = fadd(v, fmul(H->p33, n->v));
  } else
    n->refrac--;
  n->ie = fadd(fmul(H->p21, n->de), fmul(H->p11, n->ie));
  n->ii = fadd(fmul(H->p21, n->di), fmul(H->p11, n->ii));
  n->de = fadd(fmul(H->p11, n->de), n->pad[0]);
  n->di = fadd(fmul(H->p11, n->di), n->pad[1]);
  n->pad[0] = n->pad[1] = 0;
  n->de = fadd(
      n->de, fmul(fmul(H->epsc, H->external_weight), from_u(poisson(&n->rng))));
  n->fire = (!n->refrac && !(n->v >> 31) && n->v >= bits(20.0f));
  if (n->fire) {
    n->v = 0;
    n->refrac = 5;
  }
  n->x = fmul(n->x, H->decay_pre);
  n->y = fmul(n->y, H->decay_post);
}
/* phase commands and report types: boot=0, forward=1, integrate=2,
 * reverse=3, query-forward=4, query-reverse=5, finish=6. */
static void report(uint32_t type, uint32_t count, uint32_t active) {
  send(0, 3, type, count, active, H->pu);
}
__attribute__((optimize("Os"))) void br_boot(void) {
  if (H->magic != BR_MAGIC || H->nn > BR_MAX_NEURONS ||
      !X->capacity || X->capacity > BR_RECORD_CAPACITY)
    stop(0xb006);
  if (H->pu == 1) {
    /* End-to-end 32-byte ID-3 store acknowledgement / ID-2 refill regression.
     * This reserved image region is disjoint from code and network data. */
    for (uint32_t line = 0; line < 4; line++) {
      for (uint32_t lane = 0; lane < 8; lane++) {
        BrEntry e = {(0x8000u + line * 32 + lane * 4) | BR_DIRTY,
                     0x19260000u + line * 256 + lane};
        evict(&e);
      }
    }
    for (uint32_t line = 0; line < 4; line++) {
      while (H->issued != H->consumed) BR_RELAX();
      load_start(0x8000u + line * 32, line);
    }
    for (uint32_t line = 0; line < 4; line++) {
      load_wait(line);
      for (uint32_t lane = 0; lane < 8; lane++)
        if (R(line)[lane] != 0x19260000u + line * 256 + lane)
          stop(0xb00b);
    }
    H->evictions = H->write_words = 0;
  }
  report(0, 0, 0);
}
__attribute__((optimize("Os"))) void br_control(uint32_t cmd, uint32_t active, uint32_t unused,
                uint32_t unused2) {
  (void)unused;
  (void)unused2;
  H->report_count = H->report_sum = H->report_mcs = 0;
  uint32_t child = H->pu * 2;
  if (child <= H->num_pu)
    send(child, 2, cmd, active, 0, 0);
  if (child + 1 <= H->num_pu)
    send(child + 1, 2, cmd, active, 0, 0);
  if (cmd == 4) {
    report(4, H->received, 0);
    return;
  }
  if (cmd == 5) {
    report(5, H->reverse_received, 0);
    return;
  }
  H->phase = cmd;
  H->work_neuron = H->work_line = H->work_mc = H->sent = 0;
  H->active_mcs = active;
  continuation();
}
static void broadcast(uint32_t cmd, uint32_t active) {
  /* The frozen simulator logs this existing marker ABI at PU1 injection.
   * Explicit destination 1 takes the NoC path; handler 10 simply returns. */
  if (cmd == 1 || cmd == 2 || cmd == 3 || cmd == 6)
    send(1, 10, cmd == 6 ? 5 : cmd + 1, H->step + 1, 0, 0);
  H->report_count = H->report_sum = H->report_mcs = 0;
  send(0, 2, cmd, active, 0, 0);
}
__attribute__((optimize("Os"))) void br_report(uint32_t type, uint32_t count, uint32_t active,
               uint32_t source) {
  if (!source || source > H->num_pu)
    stop(0xb007);
  H->report_count++;
  H->report_sum += count;
  H->report_mcs |= active;
  uint32_t need = 1 + (H->pu * 2 <= H->num_pu) + (H->pu * 2 + 1 <= H->num_pu);
  if (H->report_count != need)
    return;
  if (H->pu != 1) {
    send(H->pu >> 1, 3, type, H->report_sum, H->report_mcs, H->pu);
    return;
  }
  if (type == 0) {
    broadcast(1, 0);
    return;
  }
  if (type == 1) {
    H->expected_forward += H->report_sum;
    H->active_mcs = H->report_mcs;
    broadcast(4, H->active_mcs);
    return;
  }
  if (type == 4) {
    if (H->report_sum > H->expected_forward)
      stop(0xb008);
    broadcast(H->report_sum == H->expected_forward ? 2 : 4, H->active_mcs);
    return;
  }
  if (type == 2) {
    broadcast(3, H->active_mcs);
    return;
  }
  if (type == 3) {
    H->expected_reverse += H->report_sum;
    broadcast(5, H->active_mcs);
    return;
  }
  if (type == 5) {
    if (H->report_sum > H->expected_reverse)
      stop(0xb009);
    broadcast(H->report_sum == H->expected_reverse ? 6 : 5, H->active_mcs);
    return;
  }
  if (type == 6) {
    H->completed = H->step;
    if (H->step == H->warmup)
      *(volatile uint32_t *)0x40000004u = 1;
    if (H->step == H->steps) {
      *(volatile uint32_t *)0x40000004u = 0;
      stop(0);
    }
    broadcast(1, 0);
    return;
  }
  stop(0xb00a);
}
__attribute__((optimize("Os"))) void br_work(void) {
  uint32_t phase = H->phase, i = H->work_neuron;
  if (phase == 1 && !H->work_line && !H->work_mc) {
    while (i < H->nn && !N[i].fire) {
      N[i].pre_fire = 0;
      i++;
    }
    H->work_neuron = i;
  }
  if (phase == 2) {
    for (uint32_t j = 0; j < H->nn; j++)
      integrate(&N[j]);
    report(2, 0, 0);
    return;
  }
  if (phase == 6) {
    /* Drain writes before publishing a completed timestep; pending-only entries
     * deliberately survive, and therefore contribute to cache occupancy. */
    while (H->issued != H->consumed)
      BR_RELAX();
    for (uint32_t j = 0; j < H->nn; j++) {
      N[j].x = fadd(N[j].x, from_u(N[j].pre_fire));
      N[j].y = fadd(N[j].y, from_u(N[j].fire));
    }
    X->count = X->cursor = 0;
    X->spill_cached = 0;
    spike_thread(20);
    H->step++;
    report(6, 0, 0);
    return;
  }
  if (phase == 3 && i >= H->nn && X->cursor < X->count) {
    /* At most four replay records per invocation, sharing a spill line. */
    for (uint32_t budget = 0; budget < 4 && X->cursor < X->count; budget++) {
      uint32_t index = X->cursor++, addr, meta;
      if (index < X->capacity) {
        addr = RECORDS[index].tag;
        meta = RECORDS[index].data;
      } else {
        uint32_t location = X->spill_base + 8 * (index - X->capacity);
        if (X->spill_cached != (location & ~31u)) {
          while (H->issued != H->consumed) BR_RELAX();
          load_start(location & ~31u, 1);
          load_wait(1);
          X->spill_cached = location & ~31u;
        }
        addr = R(1)[(location & 31u) >> 2];
        meta = R(1)[((location & 31u) >> 2) + 1];
      }
      uint32_t post = meta >> 24;
      if (!N[post].fire) {
        send(meta & 65535u, 4, (meta >> 16) & 255u, addr,
             fmul(H->dep, N[post].y), 0);
        H->sent++;
        X->manual_reverse++;
      }
    }
    continuation();
    return;
  }
  if (i >= H->nn) {
    uint32_t active = 0;
    if (phase == 1)
      for (uint32_t j = 0; j < H->nn; j++)
        if (N[j].gid < 9000 && N[j].pre_fire)
          active = 1u << H->mc;
    report(phase, H->sent, active);
    return;
  }
  volatile BrNeuron *n = &N[i];
  if (phase == 1) {
    if (!H->work_line && !H->work_mc)
      n->pre_fire = n->fire;
    if (!n->pre_fire) {
      H->work_neuron++;
      continuation();
      return;
    }
    uint32_t off = H->work_line;
    if (off < n->count) {
      uint32_t count = n->count - off;
      if (count > 8) count = 8;
      if (!H->reserved[2]) {
        /* Admission can issue two requests for each of eight pending victims.
         * Split forwarding into the next invocation to stay below work quota
         * 24: target + weight + 16 eviction requests + one continuation <= 19. */
        uint32_t issued_before = H->issued;
        load_start(n->targets + off * 4, 1);
        F weights[8];
        forward_weights(n->weights + off * 4, count, weights);
        load_wait(1);
        for (uint32_t lane = 0; lane < count; lane++)
          R(3)[lane] = weights[lane];
        if (H->issued - issued_before + count + 1 > 24) {
          H->reserved[2] = 1;
          continuation();
          return;
        }
      }
      /* R(1)/R(3) survive this local continuation. Any reverse handler uses
       * only R(0), and the phase barrier excludes new learning here. */
      for (uint32_t lane = 0; lane < count; lane++) {
        uint32_t dst = R(1)[lane];
        send(dst & 65535u, 1, dst >> 16,
             fmul(fmul(H->epsc, H->delivery), R(3)[lane]),
             n->weights + 4 * (off + lane), H->pu | (i << 16));
        H->sent++;
      }
      H->reserved[2] = 0;
      H->work_line = off + 8;
      continuation();
      return;
    }
  }
  if (phase == 3 && (n->gid >= 9000 || !n->fire)) {
    H->work_neuron++;
    continuation();
    return;
  }
  uint32_t mc = H->work_mc;
  H->work_mc = mc;
  if (mc < 16) {
    uint32_t dir = (phase == 1 ? n->static_dir : n->reverse_dir) + 8 * mc;
    if (H->reserved[0] != (dir & ~31u)) {
      load_start(dir & ~31u, 2);
      load_wait(2);
      H->reserved[0] = dir & ~31u;
    }
    uint32_t index = (dir & 31u) >> 2, addr = R(2)[index],
             count = R(2)[index + 1];
    if (count) {
      F minus = phase == 3 ? fmul(H->dep, n->y) : 0;
      F plus = phase == 3 && n->fire ? H->pot : 0;
      uint32_t handler = phase == 3 ? 4 : 1;
      send(0x8001u + mc, 0x810, addr, (((count + 3) >> 2) << 16) | handler,
           minus, plus);
      H->sent += count;
      if (phase == 3) X->scatter_reverse += count;
    }
    H->work_mc++;
    continuation();
    return;
  }
  H->work_neuron++;
  H->work_line = H->work_mc = 0;
  continuation();
}
