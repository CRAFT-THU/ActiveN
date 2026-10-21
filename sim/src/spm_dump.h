#pragma once

#include <svdpi.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Calls are made on the frontend thread while RTL evaluation is quiescent.
// An endpoint has no write, clock, or eval operation.
struct SpmEndpoint {
  unsigned pu, sizeBytes, lineBytes;
  std::string scopeName;
  std::function<uint32_t(unsigned)> readWord;

  template<class Model>
  static SpmEndpoint create(Model& model, unsigned pu, const std::string& path) {
    struct Scope {
      svScope previous;
      explicit Scope(svScope scope) : previous(svSetScope(scope)) {}
      ~Scope() { svSetScope(previous); }
    };
    const std::string name = std::string(model.name()) + "." + path + ".spm_dpi";
    const svScope scope = svGetScopeFromName(name.c_str());
    if (!scope) throw std::runtime_error("Missing SPM DPI scope: " + name);
    Scope guard(scope);
    const unsigned size = Model::spmSizeBytes();
    return {pu, size, Model::spmLineBytes(), name,
      [scope, size](unsigned offset) {
        if (offset % 4 || offset >= size || size - offset < 4)
          throw std::out_of_range("SPM word offset outside the aligned memory range");
        Scope guard(scope);
        return uint32_t(Model::spmReadWord(offset));
      }};
  }
};

struct SpmDumpBackend {
  std::string name;
  unsigned numPU, numMC;
  std::vector<SpmEndpoint> spms;
};

struct SpmDumpBoundary {
  uint64_t stopCycle, lastCompletedCycle, timerCycles;
  std::string phase, reason;
  std::optional<uint32_t> result;
};

class SpmDump {
  std::filesystem::path directory;
  std::vector<SpmDumpBackend> backends;

  static std::ofstream output(const std::filesystem::path& path) {
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(path, std::ios::binary);
    return stream;
  }

public:
  // Reserve a fresh directory before running; never mix snapshots or overwrite
  // an existing destination. A manifest is published only after all files close.
  SpmDump(std::filesystem::path path, std::vector<SpmDumpBackend> models,
          unsigned sizeBytes, unsigned lineBytes)
      : directory(std::move(path)), backends(std::move(models)) {
    if (!sizeBytes || !lineBytes || sizeBytes % lineBytes || lineBytes % 4)
      throw std::runtime_error("Invalid SPM dump geometry");
    std::set<std::string> names;
    for (const auto& backend : backends) {
      if ((backend.name != "hard" && backend.name != "soft") ||
          !names.insert(backend.name).second || backend.spms.size() != backend.numPU)
        throw std::runtime_error("Invalid SPM backend enumeration");
      std::set<unsigned> ids;
      for (const auto& spm : backend.spms) {
        if (!spm.pu || spm.pu > backend.numPU || !ids.insert(spm.pu).second ||
            spm.sizeBytes != sizeBytes || spm.lineBytes != lineBytes)
          throw std::runtime_error("SPM identity or geometry differs from configuration");
      }
    }
    if (!directory.parent_path().empty())
      std::filesystem::create_directories(directory.parent_path());
    if (!std::filesystem::create_directory(directory))
      throw std::runtime_error("SPM dump directory already exists: " + directory.string());
  }

  void write(const SpmDumpBoundary& boundary) const {
    for (const auto& backend : backends) {
      const auto backendDir = directory / backend.name;
      std::filesystem::create_directory(backendDir);
      for (const auto& spm : backend.spms) {
        auto stream = output(backendDir / ("pu-" + std::to_string(spm.pu) + ".bin"));
        for (unsigned offset = 0; offset < spm.sizeBytes; offset += 4) {
          const uint32_t word = spm.readWord(offset);
          const char bytes[] = {char(word), char(word >> 8), char(word >> 16), char(word >> 24)};
          stream.write(bytes, sizeof(bytes));
        }
        stream.close();
      }
    }
    auto manifest = output(directory / "manifest.json.tmp");
    manifest << "{\n  \"schema\": \"activen-spm-dump-v1\",\n  \"byte_order\": \"little\",\n"
             << "  \"stop_cycle\": " << boundary.stopCycle << ",\n"
             << "  \"last_completed_cycle\": " << boundary.lastCompletedCycle << ",\n"
             << "  \"stop_phase\": " << std::quoted(boundary.phase) << ",\n"
             << "  \"stop_reason\": " << std::quoted(boundary.reason) << ",\n"
             << "  \"timer_cycles\": " << boundary.timerCycles << ",\n"
             << "  \"result\": ";
    if (boundary.result) manifest << *boundary.result;
    else manifest << "null";
    manifest << ",\n  \"backends\": [";
    bool firstBackend = true;
    for (const auto& backend : backends) {
      if (!firstBackend) manifest << ',';
      firstBackend = false;
      manifest << "\n    {\"name\": " << std::quoted(backend.name)
               << ", \"num_pu\": " << backend.numPU << ", \"num_mc\": " << backend.numMC
               << ", \"spms\": [";
      bool firstSpm = true;
      for (const auto& spm : backend.spms) {
        if (!firstSpm) manifest << ',';
        firstSpm = false;
        manifest << "\n      {\"pu\": " << spm.pu << ", \"size_bytes\": " << spm.sizeBytes
                 << ", \"line_bytes\": " << spm.lineBytes
                 << ", \"scope\": " << std::quoted(spm.scopeName)
                 << ", \"file\": " << std::quoted(backend.name + "/pu-" + std::to_string(spm.pu) + ".bin") << '}';
      }
      manifest << "\n    ]}";
    }
    manifest << "\n  ]\n}\n";
    manifest.close();
    std::filesystem::rename(directory / "manifest.json.tmp", directory / "manifest.json");
  }
};
