#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sim_model_common.h"
#include "system.h"

class VerilatedFstC;

bool loadSoftSystemModelImage(const ModelImageConfig &config, std::string *error = nullptr);
void unloadSoftSystemModelImage();
bool openSoftSystemModelMemTrace(const std::optional<std::string> &path, std::string *error = nullptr);
void setSoftSystemModelLogging(bool enabled);

class SoftSystemModel : public SystemBackend {
 public:
  SoftSystemModel(int pu, int mc);
  ~SoftSystemModel() override;
  SoftSystemModel(SoftSystemModel &&) noexcept;
  SoftSystemModel &operator=(SoftSystemModel &&) noexcept;
  SoftSystemModel(const SoftSystemModel &) = delete;
  SoftSystemModel &operator=(const SoftSystemModel &) = delete;

  void attachTrace(VerilatedFstC *tracer, int depth);

  // SystemBackend interface
  SystemConfig config() const override;
  void step() override;
  void mem(const MemBusIn *, MemBusOut *) override;
  bool printStats(uint64_t cycles, bool final) override;

  // Legacy accessors (used by cosim / run helpers)
  void stepPosedge();
  void stepNegedge();
  uint64_t cycle() const;
  bool finished() const;
  uint32_t result() const;
  uint64_t timerCount() const;

  const std::vector<MemTraceEvent> &lastMemTrace() const;
  const std::vector<PeriphTraceEvent> &lastPeriphTrace() const;
  void printFinalStats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

int runSoftSystemModelFromEnv();