#include <iostream>
#include <fstream>
#include <verilated_fst_c.h>
#include "verilated/rtl.h"
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>

#include "dramsim3/dramsim3.h"

#include <deque>

#include "meow.h"

using namespace std;

bool TRACE = false;
bool LOG = false;

size_t CORE_CNT = 99;
size_t MEM_CNT = 2;

size_t noc_tick = 0;
size_t global_tick = 0;
size_t mem_tick = 0;
double core_freq_ratio = 1;
double mem_freq_ratio = 1; // Everybody runs at 1GHz

size_t text_size;
uint32_t *text_aligned;

bool exiting = false;

const size_t SMT_RATIO = 2;

void sighandler(int s) {
  exiting = true;
}

const uint64_t RESET_LENGTH = 10;
const uint32_t TEXT_BASE = 0x80000000ul;
const uint32_t DATA_BASE = 0x90000000ul;

std::unique_ptr<VerilatedFstC> tracer;

uint64_t dram_ticket_gen = 0;
struct ongoing_mem_access {
  uint32_t start;
  uint32_t end;
  uint32_t at;
};

struct core_data_t {
  std::unique_ptr<rtl> core;
  size_t core_id;

  size_t idle_cycle_cnt = 0;
  size_t non_idle_cycle_cnt = 0;
  size_t working_cycle_cnt = 0;

  char name[128];

  std::deque<uint32_t> mem_resps;
  std::deque<raw_in_flit> in_flits;

  std::map<uint64_t, std::vector<raw_in_flit>> incomplete_flits;

  uint8_t *spm;

  int memory_flit_cnt = 0;

  core_data_t(size_t core_id) {
    sprintf(name, "core.%lu", core_id);
    core.reset(new rtl(name));
    this->core_id = core_id;
    if(TRACE) core->trace(tracer.get(), 128);
    core->cfg_hartid = core_id;
  }

  ~core_data_t() {
    core->final();
    if(TRACE) tracer->close();
    delete spm;
  }

  void step_posedge(bool reseting) {
    // System reset
    if(reseting) {
      core->reset = true;
      core->mem_resp_valid = false;
      core->mem_req_ready = true;

      core->ext_out_ready = false;
      core->ext_in_valid = false;
      core->clock = true;
      core->eval();
      return;
    }

    core->reset = false;

    // About to go into posedge, update queues
    if(core->mem_resp_valid) mem_resps.pop_front();
    if(core->mem_req_valid && core->mem_req_ready) {
      if(LOG) cout<<"[Meow] "<<name<<" Req: "<<core->mem_req_bits_addr<<", Burst: "<<(uint64_t) core->mem_req_bits_burst<<endl;
      for(uint32_t i = 0; i < ((uint64_t) 1) << ((uint64_t) core->mem_req_bits_burst); ++i) {
        uint32_t addr = i * 4 + core->mem_req_bits_addr;
        if(addr - TEXT_BASE >= text_size) mem_resps.push_back(0);
        else {
          uint32_t offset = (addr - TEXT_BASE) / 4;
          if(LOG) cout<<dec<<"[Meow] Read at: "<<offset<<" = "<<text_aligned[offset]<<endl;
          mem_resps.push_back(text_aligned[offset]);
        }
      }
    }
    if(core->ext_in_valid && core->ext_in_ready) {
      in_flits.pop_front();
    }

    core->clock = true;
    core->eval();

    if(mem_resps.empty()) {
      core->mem_resp_valid = false;
      core->mem_resp_bits_data = 0xdeadbeef;
    } else {
      core->mem_resp_valid = true;
      core->mem_resp_bits_data = mem_resps.front();
    }

    if(in_flits.empty()) {
      core->ext_in_valid = false;
      core->ext_in_bits_src = 0xdead;
      core->ext_in_bits_data = 0xdeadbeef;
      core->ext_in_bits_tag = 0xbeef;
    } else {
      core->ext_in_valid = true;
      core->ext_in_bits_src = in_flits.front().src;
      core->ext_in_bits_data = in_flits.front().data;
      core->ext_in_bits_tag = in_flits.front().tag;
    }
  }
  
  void step_negedge() {
    core->ext_out_ready = false;
    core->clock = false;
    core->eval();
  }
};

std::vector<core_data_t *> cores;

struct dram_data_t {
  std::unique_ptr<dramsim3::MemorySystem> dram;
  std::deque<raw_out_flit> dram_flits;
  std::map<uint64_t, ongoing_mem_access> dram_pending;

  std::unordered_multimap<uint64_t, uint64_t> dram_read_end;
  std::map<int, uint32_t> dram_half;

  dram_data_t(dramsim3::MemorySystem *_dram) : dram(_dram) {}
};

std::vector<dram_data_t> mem;

uint32_t *dram_content;

void mem_read_cb(int idx, uint64_t addr) {
  auto mem_line = mem[idx].dram->GetBurstLength() * mem[idx].dram->GetBusBits() / 8;
  auto len = mem_line / 8;
  auto aligned = addr - (addr % mem_line); // TODO: fix head / tail
  std::optional<uint64_t> end = {};
  auto end_lookup = mem[idx].dram_read_end.find(addr);
  if(end_lookup != mem[idx].dram_read_end.end()) {
    end = { end_lookup->second / 4 };
    mem[idx].dram_read_end.erase(end_lookup);
  }

  addr /= 4;
  aligned /= 4;

  for(int i = 0; i < len; i ++) {
    uint32_t base = aligned + i * 2;
    if(base < addr) continue;
    if(end && base >= *end) break;

    uint32_t col = dram_content[base];
    uint32_t data = dram_content[base + 1];
    int16_t dst = std::bit_cast<int16_t>((uint16_t)(col >> 16));
    uint32_t neuron = col & 0xFF;

    mem[idx].dram_flits.push_back(raw_out_flit {
      .dst = dst,
      .data = neuron,
      .tag = 0,
      .first = true,
      .last = false,
    });

    mem[idx].dram_flits.push_back(raw_out_flit {
      .dst = dst,
      .data = data,
      .tag = 0,
      .first = false,
      .last = true,
    });

    cores[dst]->memory_flit_cnt++;
  }
}

void mem_write_cb(int idx, uint64_t addr) {
  throw std::runtime_error("Global memory cannot be read");
}

void meow_setup() {
  auto trace_cfg = std::getenv("MEOW_TRACE");
  if(trace_cfg && trace_cfg[0] != '\0') {
    cout<<"[Meow] Enable tracing"<<endl;
    TRACE = true;
  }
  auto log_cfg = std::getenv("MEOW_LOG");
  if(log_cfg && log_cfg[0] != '\0') {
    cout<<"[Meow] Enable logging"<<endl;
    LOG = true;
  }

  auto text_input_path = std::getenv("MEOW_TEXT");
  cout<<"[Meow] loading "<<text_input_path<<endl;
  std::ifstream text_input(text_input_path);
  text_input.seekg(0, std::ios::end);
  text_size = text_input.tellg();
  cout<<"[Meow] Total text input size: "<<text_size<<endl;
  cout<<"[Meow] Total text input size: "<<text_size<<endl;
  if(text_size % 4 != 0)
    throw std::invalid_argument("Input not a 4-byte aligned file");

  char *text_mem = new (std::align_val_t(4)) char[text_size];
  text_input.seekg(0);
  text_input.read(text_mem, text_size);
  text_aligned = (uint32_t *) text_mem;

  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, NULL);

  if(TRACE) {
    Verilated::traceEverOn(true);
    tracer.reset(new VerilatedFstC);
  }

  auto core_cfg = std::getenv("MEOW_CORE_CNT");
  if(core_cfg && core_cfg[0] != '\0') CORE_CNT = std::atoi(core_cfg);

  cout<<"Using core cnt: "<<CORE_CNT<<endl;

  // Init
  for(int i = 0; i < CORE_CNT; ++i) {
    cores.push_back(new core_data_t(i));
  }

  if(TRACE) tracer->open("./trace.fst");

  auto mem_cfg = std::getenv("MEOW_MEM");
  auto mem_log = std::getenv("MEOW_MEM_LOG");
  MEM_CNT = std::atoi(std::getenv("MEOW_MEM_CNT"));
  for(int i = 0; i < MEM_CNT; ++i) {
    auto log = std::string(mem_log) + "/" + std::to_string(i);
    std::filesystem::create_directory(log);
    auto m = new dramsim3::MemorySystem(
      mem_cfg,
      log.c_str(),
      [=](uint64_t addr){ mem_read_cb(i, addr); },
      [=](uint64_t addr){ mem_write_cb(i, addr); }
    );
    mem.emplace_back(m);
  }

  std::string data_cfg = std::getenv("MEOW_DATA");
  auto dram_path = data_cfg + "/dram.bin";
  auto dram_fd = open(dram_path.c_str(), O_RDONLY);
  if(dram_fd < 0) throw std::runtime_error("Unable to stat dram content");
  struct stat dram_stat;
  if(fstat(dram_fd, &dram_stat) == -1) throw std::runtime_error("Unable to stat dram content");
  dram_content = static_cast<uint32_t *>(mmap(NULL, dram_stat.st_size, PROT_READ, MAP_SHARED, dram_fd, 0));

  // Stat dram content
  int *dst_stat = new int[CORE_CNT];
  for(int i = 0; i < CORE_CNT; ++i) dst_stat[i] = 0;
  for(size_t i = 0; i < dram_stat.st_size / 8; ++i) {
    uint32_t spec = dram_content[i * 2];
    uint16_t dst = spec >> 16;
    if(dst >= CORE_CNT) std::cout<<"Invalid dst: "<<dst<<" at "<<i * 8<<std::endl;
    else dst_stat[dst] ++;
  }
  std::cout<<"Stat: "<<std::endl;
  for(int i = 0; i < CORE_CNT; ++i)
    std::cout<<"> Core "<<i<<": "<<dst_stat[i]<<std::endl;

  for(int i = 0; i < CORE_CNT; ++i) {
    auto c = cores[i];
    auto spm_path = data_cfg + "/spm." + std::to_string(i) + ".bin";
    auto spm_fd = open(spm_path.c_str(), O_RDONLY);
    if(spm_fd < 0) throw std::runtime_error("Unable to stat spm content");
    c->spm = static_cast<uint8_t *>(mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE, spm_fd, 0));
  }
}

void meow_teardown() {
  for(auto &m: mem)
    m.dram->PrintStats();

  cores.clear();
  mem.clear();

  if(TRACE) {
    tracer->close();
    tracer.reset();
  }
}

bool meow_stopped() {
  return exiting;
}

void stat_cores() {
  double total_idle_cnt = 0;
  for(auto &c : cores) total_idle_cnt += c->idle_cycle_cnt;
  cout<<"[Meow] Avg idle cycle: "<<total_idle_cnt / cores.size()<<" // "<<global_tick<<" = "<<total_idle_cnt / cores.size() / global_tick<<endl;

  double total_non_idle_cnt = 0;
  double total_working_cnt = 0;
  for(auto &c : cores) {
    total_non_idle_cnt += c->non_idle_cycle_cnt;
    total_working_cnt += c->working_cycle_cnt;
  }
  cout<<"[Meow] Avg working ratio: "<<total_working_cnt / cores.size()<<" // "<<total_non_idle_cnt / cores.size()<<" = "<<total_working_cnt / total_non_idle_cnt<<endl;
}

static inline void tick() {
  ++global_tick;

  for(auto &c : cores) c->step_posedge(global_tick < RESET_LENGTH);
  if(TRACE) tracer->dump(global_tick * 2);
  for(auto &c : cores) c->step_negedge();
  if(TRACE) tracer->dump(global_tick * 2 + 1);
  for(auto &c : cores) {
    if(c->core->ext_idlings == (1 << SMT_RATIO) - 1)
      c->idle_cycle_cnt += 1;
    else
      c->non_idle_cycle_cnt += 1;
    
    if(c->core->ext_working)
      c->working_cycle_cnt += 1;
  }

  if(global_tick % 1000 == 0) {
      cout<<"[Meow] Core 0 reamining flit cnt: "<<cores[0]->in_flits.size()<<endl;
  }
  if(global_tick % 10000 == 0) {
    cout<<"[Meow] Core tick: "<<global_tick<<endl;
    stat_cores();
  }
}

static inline void dram_tick() {
  ++mem_tick;
  for(auto &m : mem) {
    m.dram->ClockTick();
  }

  auto mem_line = mem[0].dram->GetBurstLength() * mem[0].dram->GetBusBits() / 8;
  bool has_finish = false;

  for(auto &m : mem) {
    for(auto it = m.dram_pending.begin(); it != m.dram_pending.end();) {
      auto &req = it->second;
      auto aligned = req.at - (req.at % mem_line); // TODO: fix head / tail
      if(m.dram->WillAcceptTransaction(req.at, false)) {
        m.dram->AddTransaction(req.at, false);
        uint32_t new_at = aligned + mem_line;
        if(new_at >= req.end) {
          m.dram_read_end.emplace(req.at, req.end);
          m.dram_pending.erase(it++);
          has_finish = true;
        } else {
          req.at = new_at;
          it++;
        }
      } else it++;
    }
  }

  if(has_finish) {
    std::cout<<"[Meow M"<<mem_tick<<"] memory read finished, outstanding: "<<endl;
    for(auto &m : mem) std::cout<<"  "<<m.dram_pending.size()<<endl;
    std::cout<<"[Meow M"<<mem_tick<<"] total: "<<dram_ticket_gen<<endl;
    // std::cout<<"[Meow M"<<mem_tick<<"] dram flits pending: "<<dram_flits.size()<<endl;
  }
}

bool meow_noc_tick(int inflight) {
  noc_tick += 1;
  while(global_tick <= noc_tick * core_freq_ratio) tick();
  while(mem_tick <= noc_tick * mem_freq_ratio) dram_tick();

  bool all_idling = true;
  for(auto &m : mem) if(m.dram_pending.size() != 0 || m.dram_flits.size() != 0 || m.dram_read_end.size() != 0) all_idling = false;
  for(auto &c : cores) if(c->core->ext_idlings != (1 << SMT_RATIO) - 1) all_idling = false;

  if(global_tick % 1000 == 0) {
    cout<<"[Meow] noc inflight flits: "<<inflight<<endl;
  }
  bool done = all_idling && inflight == 0;
  if(done) {
    cout<<"[Meow] Done"<<endl;
    cout<<"[Meow] NoC Cycles: "<<noc_tick<<endl;
    cout<<"[Meow] DRAM Cycles: "<<mem_tick<<endl;
    cout<<"[Meow] Core Cycles: "<<global_tick<<endl;
    stat_cores();
    for(auto &c : cores)
      cout<<"[Meow] MFlit at "<<c->core_id<<": "<<c->memory_flit_cnt<<endl;

    exiting = true;
  }
  return done;
}

int total_dram_flits = 0;

std::optional<std::vector<raw_out_flit>> meow_accept_flit_dram(int dram_id) {
  auto &m = mem[dram_id];
  if(m.dram_flits.empty()) return {};
  assert(m.dram_flits.front().first);
  std::vector<raw_out_flit> result;
  while(!m.dram_flits.empty() && result.size() < 64) { // TODO: parameterize this 32
    result.push_back(m.dram_flits.front());
    m.dram_flits.pop_front();
    assert(!m.dram_flits.empty() && m.dram_flits.front().last);
    result.push_back(m.dram_flits.front());
    m.dram_flits.pop_front();
    total_dram_flits += 2;
  }
  if(LOG && total_dram_flits % 1000 == 0) cout<<"[Meow] DRAM total flits = "<<total_dram_flits<<endl;
  return { result };
}

std::optional<std::vector<raw_out_flit>> meow_accept_flit_core(int core_id) {
  if(global_tick < RESET_LENGTH + 5) return {};

  if(core_id >= CORE_CNT) return {};

  auto core = cores[core_id];
  if(core->core->ext_out_ready) { // Already accepted
    return {};
  }
  core->core->ext_out_ready = true;

  if(core->core->ext_out_valid) {
    raw_out_flit flit = {
      .dst = std::bit_cast<int16_t>((uint16_t) core->core->ext_out_bits_dst),
      .data = core->core->ext_out_bits_data,
      .tag = core->core->ext_out_bits_tag,
      .first = true,
      .last = true,
    };

    if(LOG) cout<<"[Meow] Outgoing msg from "<<core->name<<": ("<<flit.dst<<") <= ["<<flit.tag<<"]: 0x"<<hex<<flit.data<<dec<<endl;
    vector<raw_out_flit> result;
    result.push_back(flit);

    return { result };
  }
  return {};
}

void meow_retire_flit_dram(int dram_id, raw_in_flit flit) {
  auto &m = mem[dram_id];
  assert(flit.tag == 0);
  auto src = flit.src;
  if(m.dram_half.contains(src)) {
    auto pending = m.dram_half[src];
    m.dram_half.erase(src);
    int ticket = ++dram_ticket_gen;
    cout<<"[Meow] DRAM "<<dram_id<<" read: core."<<src<<" -> [0x"<<hex<<pending<<", 0x"<<flit.data<<") = "<<dec<<flit.data - pending<<endl;
    m.dram_pending.insert_or_assign(dram_ticket_gen, ongoing_mem_access {
      .start = pending,
      .end = flit.data,
      .at = pending,
    });
  } else {
    m.dram_half.insert_or_assign(src, flit.data);
  }

  return;
}

void meow_retire_flit_core(int core_id, raw_in_flit flit) {
  // if(LOG) cout<<"[Meow "<<noc_tick<<", "<<global_tick<<"] Incoming msg at "<<node_id<<": "<<flit.data<<" @ "<<flit.tag<<endl;
  auto core = cores[core_id];
  if(flit.last) {
    if(core->incomplete_flits.contains(flit.pid));
      for(auto &f : core->incomplete_flits[flit.pid])
        core->in_flits.push_back(f);
    core->in_flits.push_back(flit);
    core->incomplete_flits.erase(flit.pid);
  } else {
    core->incomplete_flits[flit.pid].push_back(flit);
  }
}

uint32_t meow_softmem_read(size_t core, uint32_t addr) {
  if(core >= cores.size()) throw std::runtime_error("Invalid core id");
  if(addr >= 16384 || addr % 4 != 0) {
    // cout<<core<<" <== "<<addr<<endl;
    // throw std::runtime_error("Unaligned spm read");
    return 0xdeadbeef;
  }

  uint32_t result = 0;
  for(int d = 0; d < 4; ++d)
    result = result | (((uint32_t) cores[core]->spm[addr + d]) << (d * 8));
  // if(LOG && addr % 1000 == 0) cout<<"[Meow] Core "<<core<<" reading: 0x"<<hex<<addr<<", content 0x"<<result<<dec<<endl;
  return result;
}

void meow_softmem_write(size_t core, uint32_t addr, uint32_t wdata, uint8_t we) {
  if(we == 0) return;
  if(core >= cores.size()) throw std::runtime_error("Invalid core id");
  if(addr >= 16384 || addr % 4 != 0) {
    cout<<"[Meow] OoB / unaligned spm access: "<<core<<" ==> "<<addr<<", we = "<<(int) we<<endl;
    // throw std::runtime_error("Unaligned spm write");
    return;
  }

  for(int d = 0; d < 4; ++d) {
    if(((we >> d) & 1) != 0) {
      uint8_t written = wdata >> (d * 8);
      // if(LOG && addr % 1000 == 0 && d == 0) cout<<"[Meow] Core "<<core<<" writing: byte "<<addr + d<<", content "<<(int) written<<endl;
      cores[core]->spm[addr + d] = written;
    }
  }
}

int get_dist(int src, int dst) {
  if(src < 0 && dst < 0)
    throw std::runtime_error("Why are dram sending to dram???");
  if(dst < 0) return get_dist(dst, src);
  if(src < 0) {
    int avg = (-src-1) * CORE_CNT / MEM_CNT;
    return get_dist(avg, dst);
  }

  int diff = std::abs(src - dst);
  int rdiff = CORE_CNT - diff;
  return std::min(diff, rdiff);
}

int main(int argc, char **argv) {
  meow_setup();
  // Initialize all tick related structures
  std::vector<std::multimap<uint64_t, raw_in_flit>> core_incoming;
  std::vector<std::multimap<uint64_t, raw_in_flit>> dram_incoming;
  uint64_t tick = 0;
  uint64_t pid_gen = 0;

  core_incoming.resize(cores.size());
  dram_incoming.resize(mem.size());

  while(true) {
    for(int i = 0; i < core_incoming.size(); ++i) {
      auto &q = core_incoming[i];

      auto lb = q.lower_bound(tick);
      auto ub = q.upper_bound(tick);
      for(auto it = lb; it != ub; ++it)
        meow_retire_flit_core(i, it->second);
      q.erase(lb, ub);
    }

    for(int i = 0; i < dram_incoming.size(); ++i) {
      auto &q = dram_incoming[i];

      auto lb = q.lower_bound(tick);
      auto ub = q.upper_bound(tick);
      for(auto it = lb; it != ub; ++it)
        meow_retire_flit_dram(i, it->second);
      q.erase(lb, ub);
    }

    for(int16_t i = 0; i < CORE_CNT; ++i)
      if(auto flits = meow_accept_flit_core(i))
        for(auto &flit : *flits) {
          int dist = get_dist(i, flit.dst);
          auto &tq = flit.dst < 0 ? dram_incoming[-flit.dst-1] : core_incoming[flit.dst];
          int pid = pid_gen;
          if(flit.last) pid_gen++;
          tq.emplace(make_pair(tick + dist + 1, raw_in_flit(flit, pid, i)));
        }

    for(int16_t i = 0; i < MEM_CNT; ++i)
      if(auto flits = meow_accept_flit_dram(i))
        for(auto &flit : *flits) {
          int dist = get_dist(i, flit.dst);
          auto &tq = flit.dst < 0 ? dram_incoming[-flit.dst-1] : core_incoming[flit.dst];
          int pid = pid_gen;
          if(flit.last) pid_gen++;
          tq.emplace(make_pair(tick + dist + 1, raw_in_flit(flit, pid, i)));
        }

    int inflights = 0;
    for(auto &q : core_incoming) inflights += q.size();
    for(auto &q : dram_incoming) inflights += q.size();

    bool done = meow_noc_tick(inflights);
    ++tick;
    if(done && tick > 1000) break;
  }
  meow_teardown();
}
