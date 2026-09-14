/* Functional Brunel cache model. Memory effects execute in issue order; this
 * models traffic and state, not NoC arrival order or cycle timing. Compile with
 * -ffp-contract=off and without fast-math, as for the bare-metal payload. */
#include "../sim/payloads/sys/brunel.h"
#include "../sim/payloads/sys/brunel_numeric.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

enum Metric {
  Hits, Misses, TagHits, Probes, Inserts, Evictions, CleanEvictions,
  DirtyEvictions, PendingEvictions, WriteWords, WeightLoads, PendingOps,
  Forced, Bypasses, InsertProbes, InsertMax, Forward, Updates, MetricCount
};
struct Cache {
  unsigned policy, shift, rule;
  F wmax;
  uint32_t *dram;
  size_t words;
  std::array<BrEntry, BR_SLOTS> table{};
  std::array<uint8_t, BR_SLOTS> useful{};
  std::array<uint64_t, MetricCount> stats{};
  std::array<uint64_t, 3202> chains{};

  uint32_t home(uint32_t addr) const {
    if (policy == 2) {
      uint32_t h = ((addr >> 2) << shift) & 1023;
      return h >= BR_SLOTS ? h - BR_SLOTS : h;
    }
    uint32_t h = (addr >> 2) * 2654435761u;
    return ((h >> 16) * BR_SLOTS) >> 16;
  }
  uint32_t candidate(uint32_t addr, unsigned d) const {
    if (policy == 3) {
      // Four disjoint 200-slot banks: distinct candidates even for aliases.
      constexpr uint32_t salt[] = {0x9e3779b9, 0x85ebca6b, 0xc2b2ae35, 0x27d4eb2f};
      uint32_t h = (addr >> 2) ^ salt[d];
      h ^= h >> 16; h *= 0x7feb352d; h ^= h >> 15;
      h *= 0x846ca68b; h ^= h >> 16;
      return d * 200 + (uint64_t(h) * 200 >> 32);
    }
    return (home(addr) + d) % BR_SLOTS;
  }
  unsigned distance(uint32_t addr, unsigned pos) const {
    return (pos + BR_SLOTS - home(addr)) % BR_SLOTS;
  }
  static uint32_t address(BrEntry e) { return e.tag & ~3u; }
  static bool resident(BrEntry e) { return !(e.tag & 2); }
  int find(uint32_t addr, bool probe = true) {
    for (unsigned d = 0; d < 4; ++d) {
      if (probe) ++stats[Probes];
      unsigned pos = candidate(addr, d);
      if (address(table[pos]) == addr) return int(pos);
    }
    return -1;
  }
  int access(uint32_t addr) {
    int p = find(addr);
    if (p >= 0) {
      ++stats[TagHits];
      if (policy >= 3) useful[p] = std::min(3, useful[p] + 1);
    }
    ++stats[p >= 0 && resident(table[p]) ? Hits : Misses];
    return p;
  }
  F update(F w, F c, bool pot) const {
    if (!rule) w = pot ? fadd(w, c) : fsub(w, c);
    else w = pot ? fadd(w, fmul(c, pow04(w))) : fsub(w, fmul(c, w));
    w = fm_max(0, w);
    return rule ? w : fm_min(w, wmax);
  }
  F memory(uint32_t addr) const {
    assert(addr && !(addr & 3) && (addr / 4) < words);
    return dram[addr / 4];
  }
  void materialize(BrEntry &e) {
    if (resident(e)) return;
    ++stats[WeightLoads];
    e.data = update(memory(address(e)), e.data, (e.tag & 3) == 3);
    e.tag = address(e) | BR_DIRTY;
  }
  void evict(BrEntry e) {
    if (!e.tag) return;
    ++stats[Evictions];
    if (!resident(e)) {
      ++stats[PendingEvictions]; ++stats[Forced];
      materialize(e);
    } else ++stats[(e.tag & 3) ? DirtyEvictions : CleanEvictions];
    if ((e.tag & 3) == BR_CLEAN) return;
    dram[address(e) / 4] = e.data;
    ++stats[WriteWords];
  }
  void length(unsigned n) {
    ++chains.at(n);
    stats[InsertProbes] += n;
    stats[InsertMax] = std::max(stats[InsertMax], uint64_t(n));
  }
  void insert(BrEntry entry) {
    if (policy == 3) {
      int empty = -1, zero = -1;
      for (unsigned d = 0; d < 4; ++d) {
        unsigned p = candidate(address(entry), d);
        ++stats[Probes];
        if (!table[p].tag) { if (empty < 0) empty = p; }
        else {
          if (useful[p]) --useful[p];
          if (!useful[p] && zero < 0) zero = p;
        }
      }
      length(4);
      int p = empty >= 0 ? empty : zero;
      if (p < 0) {
        ++stats[Bypasses];
        // A bypassed modified value must still reach backing memory.
        if ((entry.tag & 3) != BR_CLEAN) {
          materialize(entry);
          dram[address(entry) / 4] = entry.data;
          ++stats[WriteWords];
        }
        return;
      }
      evict(table[p]); table[p] = entry; useful[p] = 3;
      ++stats[Inserts];
      return;
    }
    unsigned pos = home(address(entry)), n = 1;
    BrEntry held = table[pos];
    uint8_t held_use = useful[pos];
    // Age the encountered occupant, before moving it or declaring a tombstone.
    if (policy >= 4 && held.tag && held_use) --held_use;
    table[pos] = entry; useful[pos] = 3;
    ++stats[Probes]; ++stats[Inserts];
    if (policy >= 4 && held.tag && !held_use) {
      evict(held); length(n); return;
    }
    unsigned d = held.tag ? distance(address(held), pos) : 0;
    while (held.tag && (policy == 5 || n < 4)) {
      if (++d == 4) break;
      pos = candidate(address(held), d);
      ++stats[Probes]; ++n;
      // Every occupied encounter decreases the total usefulness potential.
      // Therefore policy 5 needs at most 3*800 + 1 encounters to terminate.
      assert(n <= 3 * BR_SLOTS + 1);
      if (!table[pos].tag) {
        table[pos] = held; useful[pos] = held_use; held.tag = 0; break;
      }
      if (policy >= 4 && useful[pos]) --useful[pos];
      if (policy >= 4 && !useful[pos]) {
        evict(table[pos]); table[pos] = held; useful[pos] = held_use;
        held.tag = 0; break;
      }
      unsigned old_d = distance(address(table[pos]), pos);
      if (old_d <= d) {
        std::swap(table[pos], held); std::swap(useful[pos], held_use);
        d = old_d;
      }
    }
    evict(held); length(n);
  }
  void apply(uint32_t addr, F c, bool pot) {
    if (!(c & 0x7fffffff)) return;
    ++stats[Updates];
    int p = access(addr);
    if (p >= 0 && resident(table[p])) {
      table[p] = {addr | BR_DIRTY, update(table[p].data, c, pot)};
    } else if (p >= 0) {
      ++stats[Forced]; materialize(table[p]);
      table[p].data = update(table[p].data, c, pot);
    } else if (policy == 3) {
      ++stats[WeightLoads];
      insert({addr | BR_DIRTY, update(memory(addr), c, pot)});
    } else {
      ++stats[PendingOps];
      insert({addr | (pot ? BR_PENDING_LTP : BR_PENDING_LTD), c});
    }
  }
  void forward(uint32_t addr, unsigned count, F *out) {
    assert(!(addr & 31) && count && count <= 8);
    stats[Forward] += count;
    BrEntry staged[8]; F line[8];
    unsigned missing = 0; bool load = false;
    for (unsigned lane = 0; lane < count; ++lane) {
      int p = access(addr + 4 * lane);
      staged[lane] = p < 0 ? BrEntry{0, 0} : table[p];
      if (p < 0) missing |= 1u << lane;
      if (p < 0 || !resident(staged[lane])) load = true;
    }
    if (load) {
      ++stats[WeightLoads];
      for (unsigned lane = 0; lane < count; ++lane) line[lane] = memory(addr + 4 * lane);
    }
    for (unsigned lane = 0; lane < count; ++lane) {
      auto &e = staged[lane];
      if (!e.tag) e = {addr + 4 * lane, line[lane]};
      else if (!resident(e)) {
        e.data = update(line[lane], e.data, (e.tag & 3) == 3);
        e.tag = (addr + 4 * lane) | BR_DIRTY;
        table[find(addr + 4 * lane)] = e;
      }
      out[lane] = e.data;
    }
    for (unsigned lane = 0; lane < count; ++lane)
      if (missing & (1u << lane)) insert(staged[lane]);
  }
  F logical(uint32_t addr) {
    int p = find(addr, false);
    if (p < 0) return memory(addr);
    auto e = table[p];
    return resident(e) ? e.data : update(memory(addr), e.data, (e.tag & 3) == 3);
  }
};
struct Model { std::vector<Cache> pu; };
extern "C" {
void *bc_create(unsigned policy, unsigned shift, unsigned rule, F wmax,
                uint32_t **drams, const uint64_t *sizes, unsigned pus) {
  if (policy < 1 || policy > 5 || shift > 9 || !pus || pus % 16) return nullptr;
  auto *m = new Model;
  for (unsigned p = 0; p < pus; ++p) {
    unsigned mc = p / (pus / 16);
    Cache c{}; c.policy = policy; c.shift = shift; c.rule = rule; c.wmax = wmax;
    c.dram = drams[mc]; c.words = sizes[mc]; m->pu.push_back(c);
  }
  return m;
}
void bc_destroy(Model *m) { delete m; }
// Commands: (zero-based PU, MC-local byte address, count, output offset).
void bc_forward(Model *m, const uint32_t *commands, unsigned n, F *out) {
  for (unsigned i = 0; i < n; ++i, commands += 4)
    m->pu.at(commands[0]).forward(commands[1], commands[2], out + commands[3]);
}
// Commands: (zero-based PU, MC-local byte address, FP32 coefficient bits, LTP).
void bc_apply(Model *m, const uint32_t *commands, unsigned n) {
  for (unsigned i = 0; i < n; ++i, commands += 4)
    m->pu.at(commands[0]).apply(commands[1], commands[2], commands[3]);
}
void bc_logical(Model *m, const uint32_t *pus, const uint32_t *addresses,
                unsigned n, F *out) {
  for (unsigned i = 0; i < n; ++i) out[i] = m->pu.at(pus[i]).logical(addresses[i]);
}
void bc_export(Model *m, uint32_t *entries, uint8_t *useful, uint64_t *stats,
               uint64_t *chains) {
  for (auto &c : m->pu) {
    for (unsigned i = 0; i < BR_SLOTS; ++i) {
      *entries++ = c.table[i].tag; *entries++ = c.table[i].data;
      *useful++ = c.useful[i];
    }
    for (auto v : c.stats) *stats++ = v;
    for (auto v : c.chains) *chains++ = v;
  }
}
}
