// Exercise the real bound SPM reader, then observe the same storage through its
// normal synchronous RTL read port. No generated private members are used.
#include "spm_rtl.h"
#include "spm_dump.h"
#include <iostream>
#include <memory>
#include <type_traits>

#ifndef TEST_LINE_BYTES
#error TEST_LINE_BYTES is required
#endif
static constexpr unsigned lineBytes = TEST_LINE_BYTES;
static constexpr unsigned sizeBytes = 16384;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static uint32_t expected(unsigned word) { return 0xc639a572u ^ (word * 0x01020305u); }

static void edge(spm_rtl& dut) {
  dut.clock = 0;
  dut.eval();
  dut.clock = 1;
  dut.eval();
}

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto dut = std::make_unique<spm_rtl>("probe");
  dut->clock = 0;
  dut->io_addr = 0;
  dut->io_we = 0;
  dut->eval();
  auto spm = SpmEndpoint::create(*dut, 1, "SPM");
  require(spm.sizeBytes == sizeBytes && spm.lineBytes == lineBytes, "geometry mismatch");
  for (unsigned offset = 0; offset < sizeBytes; offset += lineBytes) {
    dut->io_addr = offset;
    for (unsigned i = 0; i < lineBytes / 4; ++i)
      dut->io_wdata[i] = expected(offset / 4 + i);
    dut->io_we = ~std::remove_reference_t<decltype(dut->io_we)>{0};
    edge(*dut);
  }
  dut->io_we = 0;
  edge(*dut);
  const auto address = dut->io_addr;
  const auto clock = dut->clock;
  // Repeated and descending reads catch stale copies and offset aliasing.
  for (unsigned offset = sizeBytes; offset; offset -= 4)
    require(spm.readWord(offset - 4) == expected((offset - 4) / 4), "DPI contents mismatch");
  for (unsigned offset = 0; offset < sizeBytes; offset += 4)
    require(spm.readWord(offset) == expected(offset / 4), "DPI repeat mismatch");
  require(dut->clock == clock && dut->io_addr == address && dut->io_we == 0,
          "DPI reader changed the interface");
  for (unsigned offset : {1u, sizeBytes, UINT32_MAX}) {
    bool rejected = false;
    try { (void)spm.readWord(offset); }
    catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "invalid offset was accepted");
  }
  // Observe every byte again through the ordinary clocked SPM read path.
  for (unsigned offset = 0; offset < sizeBytes; offset += lineBytes) {
    dut->io_addr = offset;
    edge(*dut);
    for (unsigned i = 0; i < lineBytes / 4; ++i)
      require(dut->io_data[i] == expected(offset / 4 + i), "DPI reads changed SPM state");
  }
  dut->final();
  std::cout << "SPM DPI: " << sizeBytes << " bytes, " << lineBytes
            << "-byte lines; repeated reads, bounds, interface and stored state checked\n";
}
