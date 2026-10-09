// Passive NoC -> MemIf admission observations after stage, before any commit.
namespace {
struct MorrisonInletProbe {
  std::ofstream stream;

  explicit MorrisonInletProbe(const char *directory) {
    stream.open(std::filesystem::path(directory) / "memif-inlet.bin",
                std::ios::binary);
    // Magic, version, record words, MCs, request ports/MC, PUs, reserved.
    const std::array<uint32_t, 8> header{0x4e4d4931, 1, 16, 16, 2, 512, 0, 0};
    stream.write(reinterpret_cast<const char *>(header.data()), sizeof(header));
    if (!stream) throw std::runtime_error("Cannot open MemIf inlet trace");
  }

  ~MorrisonInletProbe() {
    stream.flush();
    if (!stream) std::abort();
  }
};
}

void SoftSystemBackend::observeMorrisonInlet(uint64_t cycle) {
  static const char *directory = std::getenv("MORRISON_MEMIF_INLET_PROBE");
  if (!directory) return;
  if (!morrison_probe::enabled() || cfg.numMC != 16 || cfg.numPU != 512 ||
      MEM_BUS_WIDTH_B != 32)
    throw std::runtime_error("MemIf inlet trace requires the 512/16/32B phase probe target");
  static MorrisonInletProbe probe(directory);
  const auto [step, phase] = morrisonProbePhase();
  if (!morrison_probe::timerActive && !(step && (phase == 2 || phase == 3))) return;

  // 0..3: cycle low/high, step, phase.
  // 4..6: offered, accepted and selected-before-admission port masks (2/MC).
  // 7..8: full MSHR / full bulk FIFO MC masks.
  // 9..13: selected scalar-read, scalar-write, bulk, idle, other MC masks.
  // 14: timer active. 15: external request presented but Ramulator refused.
  std::array<uint32_t, 16> record{};
  record[0] = cycle;
  record[1] = cycle >> 32;
  record[2] = step;
  record[3] = phase;
  record[14] = morrison_probe::timerActive;

  for (size_t mc = 0; mc < cfg.numMC; ++mc) {
    const size_t endpoint = mc + 1;
    const auto &dram = drams[mc];
    if (memif_eject[endpoint].size() != 2)
      throw std::runtime_error("MemIf inlet trace expected two request ports per MC");
    const auto offered = [&](size_t port) -> const Flit * {
      const auto &token = memif_eject[endpoint][port].token;
      if (!token) return nullptr;
      const uint16_t pu = topo.memif_req_at[endpoint][port];
      return routers[pu].rt.peek(*token);
    };
    for (size_t port = 0; port < 2; ++port)
      if (offered(port)) record[4] |= uint32_t{1} << (2 * mc + port);

    const auto selected = dram.nocSelectedPort(offered);
    const auto accepted = memif_eject_accept[endpoint];
    if (bool(selected) != bool((record[4] >> (2 * mc)) & 3) ||
        (accepted && accepted != selected) || accepted != dram.nocAcceptMultiple(offered))
      throw std::runtime_error("MemIf inlet observation disagrees with resolved admission");
    if (selected) record[6] |= uint32_t{1} << (2 * mc + *selected);
    if (accepted) record[5] |= uint32_t{1} << (2 * mc + *accepted);

    const uint32_t bit = uint32_t{1} << mc;
    const bool full = dram.mshrInflightCount() == dram.mshrCapacity();
    const bool bulkFull = dram.bulkQueueOccupancy() == soft_mem::DRAM_BULK_QUEUE_DEPTH;
    if (full) record[7] |= bit;
    if (bulkFull) record[8] |= bit;
    if (selected) {
      const auto &flit = *offered(*selected);
      if (flit.dst != 0x8001 + mc)
        throw std::runtime_error("MemIf inlet request reached the wrong MC");
      const uint8_t tag = flit.tag;
      const bool scalar = tag == 0 || tag == 1 || tag == 2;
      const bool bulk = tag == 0x10 || tag == 0x11;
      const unsigned category = tag == 0 ? 9 : (tag == 1 || tag == 2) ? 10 :
          bulk ? 11 : (tag == 0xf0 || tag == 0xf1) ? 12 : 13;
      record[category] |= bit;
      if (!accepted && scalar && !full)
        throw std::runtime_error("Scalar inlet refusal without a full MSHR pool");
      if (!accepted && bulk && (!bulkFull || (flit.data[1] >> 16) == 0))
        throw std::runtime_error("Bulk inlet refusal without a full request FIFO");
    }
    if (dram.peekMem() && !memif_ext[endpoint].reqAccepting) record[15] |= bit;
  }
  probe.stream.write(reinterpret_cast<const char *>(record.data()), sizeof(record));
  if (!probe.stream) throw std::runtime_error("Cannot write MemIf inlet trace");
}
