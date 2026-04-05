#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sim_model_common.h"

class VerilatedFstC;

bool loadSystemModelImage(const ModelImageConfig &config, std::string *error = nullptr);
void unloadSystemModelImage();
bool openSystemModelMemTrace(const std::optional<std::string> &path, std::string *error = nullptr);
void setSystemModelLogging(bool enabled);

class SystemModel {
 public:
  SystemModel(int pu, int mc);
  ~SystemModel();
  SystemModel(SystemModel &&) noexcept;
  SystemModel &operator=(SystemModel &&) noexcept;
  SystemModel(const SystemModel &) = delete;
  SystemModel &operator=(const SystemModel &) = delete;

  void attachTrace(VerilatedFstC *tracer, int depth);
  void initDram(const char *cfg, const char *log_dir);
  void step();
  void stepPosedge();
  void stepNegedge();
  void printDramStats() const;

  uint64_t cycle() const;
  bool finished() const;
  uint32_t result() const;
  uint64_t timerCount() const;
  bool useDram() const;

  const std::vector<MemTraceEvent> &lastMemTrace() const;
  const std::vector<PeriphTraceEvent> &lastPeriphTrace() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

int runSystemModelFromEnv();