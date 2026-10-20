#pragma once

#include <svdpi.h>
#include <verilated.h>
#include <cstdint>
#include <algorithm>
#include <optional>
#include <unordered_set>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// Each endpoint names one cache, even when a model contains many PUs.
// All calls are made by the driver while model evaluation is quiescent.
struct ICacheEndpoint {
  std::string name;
  unsigned lineBytes, sets, ways;
  const void* model;
  std::function<bool()> ready;
  std::function<void(uint32_t, unsigned, const svBitVecVal*)> insert;
  std::function<void()> settle;

  struct Scope {
    svScope previous;
    explicit Scope(svScope scope) : previous(svSetScope(scope)) {}
    ~Scope() { svSetScope(previous); }
  };

  template<class Model>
  static ICacheEndpoint create(Model& model, const std::string& path) {
    const std::string name = std::string(model.name()) + "." + path + ".icache_dpi";
    const svScope scope = svGetScopeFromName(name.c_str());
    if (!scope) throw std::runtime_error("Missing ICache DPI scope: " + name);
    Scope guard(scope);
    return {name, Model::icacheLineBytes(), Model::icacheSets(), Model::icacheWays(), &model,
      [scope] { Scope guard(scope); return bool(Model::icacheInitReady()); },
      [scope](uint32_t addr, unsigned way, const svBitVecVal* data) {
        Scope guard(scope); Model::insertLine(addr, way, data);
      },
      [&model] { model.eval(); }};
  }
};

// argparse treats optional values as positional filenames. Normalize a bare
// flag before parsing so `--init-icache program.bin` keeps its usual meaning.
// Both --init-icache=ADDR and --init-icache ADDR accept an explicit address.
inline std::vector<std::string> icacheCommandArgs(int argc, char** argv) {
  std::vector<std::string> args;
  bool options = true;
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
    if (args.back() == "--") options = false;
    if (options && args.back() == "--init-icache") {
      const std::string next = i + 1 < argc ? argv[i + 1] : "";
      const bool address = next.starts_with("0x") || next.starts_with("0X") ||
        (!next.empty() && std::all_of(next.begin(), next.end(),
                                    [](char c) { return c >= '0' && c <= '9'; }));
      if (!address) args.emplace_back("0x80000000");
    }
  }
  return args;
}

struct ICachePreload {
  struct Line {
    uint32_t address;
    unsigned way;
    std::vector<svBitVecVal> words;
  };
  unsigned lineBytes, sets, ways;
  std::vector<Line> lines;

  // The reader observes backing bytes directly, with no memory transactions,
  // MMIO side effects, or Ramulator statistics.
  template<class Reader>
  ICachePreload(const ICacheEndpoint& cache, uint32_t start, Reader readByte)
      : lineBytes(cache.lineBytes), sets(cache.sets), ways(cache.ways) {
    const uint64_t size = uint64_t(lineBytes) * sets * ways;
    if (start % lineBytes != 0 || uint64_t(start) + size > (uint64_t{1} << 32))
      throw std::runtime_error("--init-icache requires a cache-line-aligned, non-wrapping region");
    for (unsigned i = 0; i < sets * ways; ++i) {
      Line line{start + i * lineBytes, i / sets, std::vector<svBitVecVal>(lineBytes / 4)};
      for (unsigned byte = 0; byte < lineBytes; ++byte)
        line.words[byte / 4] |= uint32_t(readByte(line.address + byte)) << (8 * (byte % 4));
      lines.push_back(std::move(line));
    }
  }

  void apply(const ICacheEndpoint& cache) const {
    if (cache.lineBytes != lineBytes || cache.sets != sets || cache.ways != ways)
      throw std::runtime_error("ICache preload geometry differs between backends");
    // Insert the beginning last, making early instructions most recently used.
    // The normal replacement policy can then evict later lines for the boot ROM.
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
      cache.insert(it->address, it->way, it->words.data());
  }
};

// Call after step(), before the next fetch edge, with all worker threads idle.
inline void serviceICachePreload(const ICachePreload& preload,
                                std::vector<ICacheEndpoint*>& pending) {
  std::vector<ICacheEndpoint*> inserted;
  std::erase_if(pending, [&](ICacheEndpoint* cache) {
    if (!cache->ready()) return false;
    preload.apply(*cache);
    inserted.push_back(cache);
    return true;
  });
  std::unordered_set<const void*> settled;
  for (auto* cache : inserted)
    if (settled.insert(cache->model).second) cache->settle();
}

