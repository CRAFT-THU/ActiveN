#pragma once
#include "morrison_phase_probe.h"
#include <ramulator/base/base.h>
#include <ramulator/controller/impl/hbm_controller_base.h>

namespace Ramulator {
// HBM12 itself is a thin final subclass of this same native base. This observer
// repeats its four tick calls exactly and never calls scheduling helpers twice.
class MorrisonHBMProbe final : public HBMControllerBase {
  RAMULATOR_REGISTER_IMPLEMENTATION_DERIVED(IController, MorrisonHBMProbe,
                                           HBMControllerBase, "MorrisonHBMProbe")
  morrison_probe::Counters counts{"hbm-probe.csv"};
  uint32_t identity = 0;
  bool identified = false;

  // Bit 0: bank-local row timing; 1: bank-group column/activation spacing;
  // 2: read/write turnaround; 3: refresh recovery; 4: other column spacing;
  // 5: activation spacing/window outside a bank; 6: command-bus spacing;
  // 7: unclassified deadline (including any unsupported sibling constraint).
  unsigned classify(int level, int from, int to) const {
    const auto &s=*m_device.m_spec;
    const auto &a=s.command_meta[from], &b=s.command_meta[to];
    const auto &an=s.command_names[from], &bn=s.command_names[to];
    if (a.is_refreshing) return 8;
    if (a.is_column_command && b.is_column_command &&
        ((an[0]=='R') != (bn[0]=='R'))) return 4;
    if (level==m_bank_level) return 1;
    if (s.level_names[level]=="BankGroup") return 2;
    if (a.is_column_command && b.is_column_command) return 16;
    if (a.is_opening && b.is_opening && level>0) return 32;
    if (level==0) return 64;
    return 128;
  }
  unsigned reasonsAt(const DRAMNode &node, int cmd, const AddrVec_t &addr) const {
    const auto &s=*m_device.m_spec;
    unsigned mask=0;
    if (node.m_cmd_ready_clk[cmd] > m_clk) {
      // Reconstruct the live constraint, retaining all tied/overlapping causes.
      // HBM2 target has no sibling timing constraints. Detect rather than guess
      // if another configuration introduces one.
      bool found=false;
      for (int from=0;from<s.command_count;++from) {
        const auto &hist=node.m_cmd_history[from];
        for (const auto &t:s.timing_cons[node.m_level][from]) {
          if (t.cmd!=cmd || t.sibling || t.window<1 || size_t(t.window)>hist.size()) continue;
          const auto past=hist[t.window-1];
          if (past>=0 && past+t.val>m_clk) {
            found=true; mask |= classify(node.m_level,from,cmd);
          }
        }
      }
      if (!found) mask |= 128;
    }
    if (!node.m_child_nodes.empty()) {
      const int child=addr[node.m_level+1];
      if (child<0) for (const auto &c:node.m_child_nodes) mask |= reasonsAt(*c,cmd,addr);
      else mask |= reasonsAt(*node.m_child_nodes.at(child),cmd,addr);
    }
    return mask;
  }
  struct Scan {
    unsigned candidates=0, ready=0, prep=0, guard=0, masks=0;
    unsigned common=255;
  };
  void scan(ReqBuffer &buffer, SlotType slot, bool guard, bool headOnly, Scan &out) {
    for (const auto &original:buffer) {
      Request req=original;
      req.command=get_preq_command(req.final_command,req.addr_vec);
      if (guard && would_close_active(req)) ++out.guard;
      else if (slot_matches(req,slot)) {
        ++out.candidates;
        const unsigned reason=reasonsAt(*m_device.m_root,req.command,req.addr_vec);
        const bool ready=check_timing(req.command,req.addr_vec);
        if (ready != (reason==0)) throw std::runtime_error("HBM probe timing reconstruction mismatch");
        out.ready += ready;
        out.masks |= reason; out.common &= reason;
      } else if (slot==SlotType::ColumnBus &&
                 m_device.m_spec->command_meta[req.final_command].is_column_command) ++out.prep;
      if (headOnly) break;
    }
  }
  bool predictedMode() const {
    bool mode=m_is_write_mode;
    if (!mode) {
      if (m_write_buffer.size() > m_wr_high_watermark*m_write_buffer.max_size || m_read_buffer.size()==0) mode=true;
    } else if (m_write_buffer.size() < m_wr_low_watermark*m_write_buffer.max_size && m_read_buffer.size()!=0) mode=false;
    return mode;
  }
  void add(const std::string &key,uint64_t n=1) {
    if(morrison_probe::inPhase()) counts.add(morrison_probe::step,morrison_probe::phase,identity,key,n);
    if(morrison_probe::timerActive) counts.add(0,100,identity,key,n);
  }
  void observeSlot(SlotType slot) {
    const std::string prefix=slot==SlotType::ColumnBus ? "column_" : "row_";
    // Sample probes on an odd cadence; actual command/queue statistics below
    // are counted every cycle. Capture candidates BEFORE the native mutation.
    const bool sample=(morrison_probe::cycle+identity)%17==0;
    Scan selected,opposite;
    if (sample) {
      if (slot==SlotType::ColumnBus) scan(m_active_buffer,slot,false,false,selected);
      if (!selected.ready) {
        if (m_priority_buffer.size()) scan(m_priority_buffer,slot,true,true,selected);
        else {
          const bool mode=predictedMode();
          scan(mode?m_write_buffer:m_read_buffer,slot,true,false,selected);
          scan(mode?m_read_buffer:m_write_buffer,slot,true,false,opposite);
        }
      }
    }
    const auto issued=try_issue_slot(slot);
    if (issued) {
      add(prefix+"command_"+m_device.m_spec->command_names[issued->command]);
      if (m_device.m_spec->bank_targets[issued->command]==BankTarget::Single)
        add(prefix+"bank_"+std::to_string(m_device.get_flat_bank_id(issued->addr_vec)));
    }
    else add(prefix+"unused");
    if (!sample) return;
    add(prefix+"samples");
    if (bool(issued)!=(selected.ready!=0)) throw std::runtime_error("HBM probe candidate reconstruction mismatch");
    if (issued) return;
    add(prefix+"unused_samples");
    add(prefix+"unused_with_priority",m_priority_buffer.size()!=0);
    add(prefix+"unused_opposite_ready",opposite.ready!=0);
    add(prefix+"unused_prep",selected.prep!=0);
    add(prefix+"unused_guard",selected.guard!=0);
    add(prefix+"unused_candidates",selected.candidates!=0);
    add(prefix+"unused_mask_"+std::to_string(selected.masks));
    if (selected.candidates) add(prefix+"unused_common_"+std::to_string(selected.common));
  }
 public:
  bool send(Request &req) override {
    const auto writes=s_num_write_reqs_coalesced,reads=s_num_read_reqs_forwarded;
    const bool result=ControllerBase::send(req);
    add("native_write_coalesced",s_num_write_reqs_coalesced-writes);
    add("native_read_forwarded",s_num_read_reqs_forwarded-reads);
    return result;
  }
  void finalize() override {
    ControllerBase::finalize();
    // System intentionally uses std::exit with parked backend workers, so
    // automatic controller destructors do not run. Its native finalize hook
    // is the supported place to publish these counters.
    counts.flush();
  }
  void tick() override {
    if (!identified) { identity=morrison_probe::mc; counts.name="hbm-probe-mc"+std::to_string(identity)+".csv"; identified=true; }
    hbm_tick_prologue();
    if (!morrison_probe::inPhase() && !morrison_probe::timerActive) {
      try_issue_slot(SlotType::ColumnBus); try_issue_slot(SlotType::RowBus);
    } else {
      const auto hits=s_row_hits,misses=s_row_misses,conflicts=s_row_conflicts;
      const auto rh=s_read_row_hits,rm=s_read_row_misses,rc=s_read_row_conflicts;
      const bool mode=m_is_write_mode;
      add("cycles"); add("read_queue_sum",m_read_buffer.size()); add("write_queue_sum",m_write_buffer.size());
      add("active_queue_sum",m_active_buffer.size()); add("priority_queue_sum",m_priority_buffer.size());
      add("pending_sum",m_pending.size()); add("write_mode",mode);
      add("read_full",m_read_buffer.size()==m_read_buffer.max_size);
      add("write_full",m_write_buffer.size()==m_write_buffer.max_size);
      add("no_command_work",m_read_buffer.size()+m_write_buffer.size()+m_active_buffer.size()+m_priority_buffer.size()==0);
      observeSlot(SlotType::ColumnBus); observeSlot(SlotType::RowBus);
      add("mode_switch",mode!=m_is_write_mode);
      add("row_hit",s_row_hits-hits); add("row_miss",s_row_misses-misses); add("row_conflict",s_row_conflicts-conflicts);
      add("read_row_hit",s_read_row_hits-rh); add("read_row_miss",s_read_row_misses-rm); add("read_row_conflict",s_read_row_conflicts-rc);
    }
    hbm_tick_epilogue();
  }
};
}
