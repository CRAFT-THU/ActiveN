#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace soft_mem {

using MshrSlot = uint16_t;
inline constexpr size_t DRAM_MSHR_COUNT = 256;
inline constexpr size_t DRAM_UNICAST_RESERVE = 32;
inline constexpr size_t MAX_MSHR_COUNT = size_t{std::numeric_limits<MshrSlot>::max()} + 1;

inline size_t checkedMshrCount(size_t count) {
  if (count == 0 || count > MAX_MSHR_COUNT)
    throw std::invalid_argument("MSHR count must be between 1 and 65536");
  return count;
}

enum class AllocationMode { FirstFree, Sequential };

inline const char *allocationName(AllocationMode mode) {
  switch (mode) {
    case AllocationMode::FirstFree: return "first-free";
    case AllocationMode::Sequential: return "sequential";
  }
  throw std::invalid_argument("invalid MemIf allocation mode");
}

struct ExperimentConfig {
  size_t slots = DRAM_MSHR_COUNT;
  bool merge = false;
  AllocationMode allocation = AllocationMode::FirstFree;
  size_t unicastReserve = DRAM_UNICAST_RESERVE;

  void validate() const {
    checkedMshrCount(slots);
    allocationName(allocation);
    if (unicastReserve > slots)
      throw std::invalid_argument("MemIf unicast reservation must not exceed MSHR count");
  }

  bool isBaseline() const {
    return slots == DRAM_MSHR_COUNT && !merge &&
           allocation == AllocationMode::FirstFree &&
           unicastReserve == DRAM_UNICAST_RESERVE;
  }
};

inline ExperimentConfig checkedExperimentConfig(ExperimentConfig config) {
  config.validate();
  return config;
}

struct ExperimentOptions {
  std::optional<std::string> slots, allocation, merge, unicastReserve;

  bool any() const {
    return slots || allocation || merge || unicastReserve;
  }
};

inline size_t parseNonnegative(std::string_view text, const char *name) {
  size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument(std::string(name) + " must be a nonnegative decimal integer");
  return value;
}

// Only the MSHR count retains an environment fallback; merging is CLI-only.
inline ExperimentConfig experimentConfig(const ExperimentOptions &options = {}) {
  ExperimentConfig config;
  if (options.slots) config.slots = parseNonnegative(*options.slots, "--soft-memif-mshrs");
  else if (const char *text = std::getenv("SOFT_MEMIF_MSHRS"))
    config.slots = parseNonnegative(text, "SOFT_MEMIF_MSHRS");

  if (options.merge) {
    const std::string_view text(*options.merge);
    if (text == "on" || text == "true")
      config.merge = true;
    else if (text == "off" || text == "false")
      config.merge = false;
    else
      throw std::invalid_argument("MemIf write merging must be on/off or true/false");
  }
  if (options.allocation) {
    if (*options.allocation == "first-free") config.allocation = AllocationMode::FirstFree;
    else if (*options.allocation == "sequential") config.allocation = AllocationMode::Sequential;
    else throw std::invalid_argument("MemIf allocation must be first-free or sequential");
  }
  if (options.unicastReserve)
    config.unicastReserve = parseNonnegative(*options.unicastReserve, "--soft-memif-unicast-reserve");
  config.validate();
  return config;
}

} // namespace soft_mem
