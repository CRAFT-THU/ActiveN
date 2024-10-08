#ifndef __MEOW_H__
#define __MEOW_H__

#include <cstdint>
#include <optional>
#include <vector>

void meow_setup();
void meow_teardown();
bool meow_noc_tick(int inflight);
bool meow_stopped();

struct raw_out_flit {
    int16_t dst;
    uint32_t data;
    uint16_t tag;
    bool first;
    bool last;
};

struct raw_in_flit {
    int16_t src;
    uint32_t data;
    uint16_t tag;

    uint64_t pid;
    bool last;

    raw_in_flit(raw_out_flit f, uint64_t p, int16_t s)
      : src(s), data(f.data), tag(f.tag), pid(p), last(f.last) {}
};

std::optional<std::vector<raw_out_flit>> meow_accept_flit(int node_id);
void meow_retire_flit(int node_id, raw_in_flit flit);

uint32_t meow_softmem_read(size_t core, uint32_t addr);
void meow_softmem_write(size_t core, uint32_t addr, uint32_t wdata, uint8_t we);

#endif // __MEOW_H__
