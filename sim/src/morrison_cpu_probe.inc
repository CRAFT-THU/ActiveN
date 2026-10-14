// Included only by soft_backend.cpp, after the native generated root type.
namespace {
const char *morrisonSpatialDirectory() {
  static const char *directory = std::getenv("MORRISON_NOC_SPATIAL_PROBE");
  return directory;
}
struct MorrisonCpuProbe {
  struct State {
    uint32_t phase=0,step=0,valid=0,advance=0,decode=0,wfi=0,busy=0,idle=0,eidle=0,branch=0,wake=0,delayed=0,kind=0;
    bool seen=false,sampled=false;
    std::array<uint32_t,48> raw{};
  };
  struct Totals {
    std::array<uint64_t,16> counts{};
    std::array<uint64_t,1024> emptyReasons{};
  };
  std::vector<State> previous;
  std::map<std::pair<unsigned,unsigned>,std::vector<Totals>> totals;
  std::pair<unsigned,unsigned> lastKey{};
  std::vector<Totals> *current=nullptr;
  std::ofstream samples;
  std::ofstream spatialSamples;
  morrison_probe::Counters memory{"memif-probe.csv"};
  unsigned previousStep=0,previousPhase=0;
  bool previousTimer=false;
  MorrisonCpuProbe() {
    samples.open(std::filesystem::path(morrison_probe::directory())/"cpu-joint.bin",std::ios::binary);
    if (!samples) throw std::runtime_error("Cannot open Morrison CPU probe");
    if (const char *directory = morrisonSpatialDirectory()) {
      spatialSamples.open(std::filesystem::path(directory)/"cpu-spatial.bin", std::ios::binary);
      const std::array<uint32_t,8> header{0x4d534350,1,48,251,SYSTEM_PIPE_CNT,MEM_BUS_WIDTH_B,0,0};
      spatialSamples.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
      if (!spatialSamples) throw std::runtime_error("Cannot open simultaneous CPU samples");
    }
  }
  ~MorrisonCpuProbe() {
    morrison_probe::Counters out{"cpu-probe.csv"};
    static const char *names[]={"cycles","advance0","advance1","stall0","stall1","empty","previous_both_idle","empty_previous_both_idle","empty_previous_one_idle","empty_previous_none_idle","empty_previous_delayed","empty_previous_wakeup","empty_previous_branch","empty_previous_decode_missing","prediction_checks","prediction_mismatch"};
    for (const auto &[key,pus]:totals) for (unsigned pu=1;pu<pus.size();++pu) {
      const auto &t=pus[pu];
      for (unsigned i=0;i<16;++i) out.add(key.first,key.second,pu,names[i],t.counts[i]);
      for (unsigned i=0;i<1024;++i) out.add(key.first,key.second,pu,"empty_reasons_"+std::to_string(i),t.emptyReasons[i]);
    }
  }
};
}

std::pair<uint32_t,uint32_t> SoftSystemBackend::morrisonProbePhase() const {
  const auto &s=cores[1]->rootp->Core__DOT__exec__DOT__lsu__DOT__spm__DOT__scratchpad_ext__DOT__Memory;
  const auto w=[&](unsigned o){return s[o/MEM_BUS_WIDTH_B][o%MEM_BUS_WIDTH_B/4];};
  const auto abi=w(MR_PARAMETERS+16);
  if (w(0)!=MR_MAGIC || (abi!=3 && abi!=4) || w(24)>=w(36)) return {};
  return {w(24)+1,w(28)};
}

void SoftSystemBackend::observeMorrisonPhase(uint64_t cycle) {
  if (!morrison_probe::enabled()) return;
  static MorrisonCpuProbe p;
  if (p.previous.empty()) p.previous.resize(cfg.numPU+1);
  const auto [step,phase]=morrisonProbePhase();
  const bool measuring=step && (phase==2 || phase==3);
  if (measuring && p.lastKey!=std::pair(step,phase)) {
    p.lastKey={step,phase}; auto &v=p.totals[p.lastKey]; v.resize(cfg.numPU+1); p.current=&v;
  }
  for (unsigned id=0;id<cfg.numMC;++id) {
    const auto &d=drams[id];
    // Current occupancy is pre-edge; event booleans are from the preceding
    // committed edge, tagged with its saved phase rather than today's header.
    if (measuring || morrison_probe::timerActive) {
      auto add=[&](const char *k,uint64_t n=1){
        if(measuring)p.memory.add(step,phase,id,k,n);
        if(morrison_probe::timerActive)p.memory.add(0,100,id,k,n);
      };
      if(measuring)p.memory.maximum(step,phase,id,"external_id_max",d.externalIdCount());
      if(morrison_probe::timerActive)p.memory.maximum(0,100,id,"external_id_max",d.externalIdCount());
      add("capacity_sum",d.mshrCapacity()); add("external_id_sum",d.externalIdCount());
      add("cycles"); add("allocated_sum",d.mshrInflightCount()); add("unissued_sum",d.mshrUnissuedCount());
      add("fulfilled_sum",d.mshrFulfilledCount()); add("full",d.mshrInflightCount()==d.mshrCapacity());
      add("bulk_queue_sum",d.bulkQueueOccupancy());
    }
    const bool previousMeasuring=p.previousStep && (p.previousPhase==2 || p.previousPhase==3);
    if (previousMeasuring || p.previousTimer) {
      const auto &e=d.cycleEvents();
      auto add=[&](const char *k,uint64_t n=1){
        if(previousMeasuring)p.memory.add(p.previousStep,p.previousPhase,id,k,n);
        if(p.previousTimer)p.memory.add(0,100,id,k,n);
      };
      add("incoming_reads",e.incomingRead); add("incoming_writes",e.incomingWrite);
      add("merges",e.mergedWrite); add("merges_when_full",e.mergedWhileFull); add("external_id_blocked",e.idBlocked);
      add("event_cycles"); add("bulk_head",e.bulkHeadPresent); add("allocation_full",e.allocationFull);
      add("bulk_full",e.bulkHeadPresent && e.allocationFull); add("bulk_scalar",e.bulkScalarPriority);
      add("bulk_bcst_limit",e.bulkBroadcastLimit); add("bulk_allocated",e.bulkAllocated);
      add("accepted_reads",e.externalRequestAccepted && !e.externalRequestWrite);
      add("accepted_writes",e.externalRequestAccepted && e.externalRequestWrite);
      add("refused_reads",e.externalRequestPresented && !e.externalRequestAccepted && !e.externalRequestWrite);
      add("refused_writes",e.externalRequestPresented && !e.externalRequestAccepted && e.externalRequestWrite);
    }
  }
  p.previousStep=step;p.previousPhase=phase;p.previousTimer=morrison_probe::timerActive;
  for (unsigned pu=1;pu<=cfg.numPU;++pu) {
    const auto &r=*cores[pu]->rootp;
    auto &prev=p.previous[pu];
    MorrisonCpuProbe::State n;
    n.step=step;n.phase=phase;n.seen=!cores[pu]->reset;
    n.valid=r.Core__DOT__exec__DOT__valid; n.advance=r.Core__DOT__exec__DOT__s0step;
    n.decode=r.Core__DOT___fetch_decoded_0_valid | (r.Core__DOT___fetch_decoded_1_valid<<1);
    n.wfi=(r.Core__DOT___fetch_decoded_0_bits_isSystem && !r.Core__DOT___fetch_decoded_0_bits_funct3 && r.Core__DOT___fetch_decoded_0_bits_rs2==5) |
        ((r.Core__DOT___fetch_decoded_1_bits_isSystem && !r.Core__DOT___fetch_decoded_1_bits_funct3 && r.Core__DOT___fetch_decoded_1_bits_rs2==5)<<1);
    n.busy=r.Core__DOT__exec__DOT__busy; n.idle=r.Core__DOT__exec__DOT__idlings;
    n.eidle=r.Core__DOT__exec__DOT__eidlings;
    n.branch=r.Core__DOT___exec_brs_0_valid | (r.Core__DOT___exec_brs_1_valid<<1);
    n.wake=r.Core__DOT__exec__DOT__biu__DOT___sched_wakeup_output;
    n.delayed=n.valid && r.Core__DOT__exec__DOT__delayed;
    n.kind=r.Core__DOT__exec__DOT__uop_isFP | (r.Core__DOT__exec__DOT__uop_isMem<<1) | (r.Core__DOT__exec__DOT__uop_isAM<<2);
    if (measuring) {
      auto &t=(*p.current)[pu]; auto &c=t.counts; ++c[0];
      if (!n.valid) ++c[5];
      else ++c[(n.advance?1:3)+(r.Core__DOT__exec__DOT__uop_smsel==2)];
      if (prev.seen && n.seen) {
        const bool predicted=prev.advance ? bool(prev.decode & ~prev.wfi & ~prev.busy & ~prev.branch & 3) : bool(prev.valid);
        ++c[14]; c[15]+=predicted!=bool(n.valid);
        if (predicted!=bool(n.valid)) throw runtime_error("Morrison CPU probe issue alignment mismatch");
        c[6]+=prev.idle==3;
        if (!n.valid) {
          c[7]+=prev.idle==3;c[8]+=prev.idle==1 || prev.idle==2;c[9]+=prev.idle==0;
          c[10]+=prev.delayed;c[11]+=prev.wake!=0;c[12]+=prev.branch!=0;c[13]+=prev.decode!=3;
          unsigned reason=0;
          for (unsigned smt=0;smt<2;++smt) {
            const unsigned m=1<<smt;
            const unsigned bits=(!(prev.decode&m)) | (bool(prev.wfi&m)<<1) | (bool(prev.busy&m)<<2) | (bool(prev.branch&m)<<3) | (bool(prev.idle&m)<<4);
            reason |= bits<<(5*smt);
          }
          ++t.emptyReasons[reason];
        }
      }
    }
    if (prev.sampled) {
      prev.raw[6]=n.valid; prev.raw[7]=n.advance;
      p.samples.write(reinterpret_cast<const char*>(prev.raw.data()),sizeof(prev.raw));
      if (!p.samples) throw runtime_error("Morrison CPU sample write failed");
    }
    n.sampled=measuring && (cycle+pu+1)%251==0;
    const bool spatialSample=morrisonSpatialDirectory() && step && phase==2 && cycle%251==0;
    if (n.sampled || spatialSample) {
      const auto &s=r.Core__DOT__exec__DOT__lsu__DOT__spm__DOT__scratchpad_ext__DOT__Memory;
      const auto w=[&](unsigned o){return s[o/MEM_BUS_WIDTH_B][o%MEM_BUS_WIDTH_B/4];};
      auto &a=n.raw;
      a[0]=uint32_t(cycle);a[1]=cycle>>32;a[2]=step;a[3]=phase;a[4]=pu;a[5]=n.valid;
      a[8]=r.Core__DOT__exec__DOT__uop_smsel;a[9]=r.Core__DOT__exec__DOT__uop_pc;
      a[10]=n.decode;a[11]=n.busy;a[12]=n.idle;a[13]=n.eidle;a[14]=n.branch;a[15]=n.wfi;
      a[16]=n.wake;a[17]=n.delayed;a[18]=n.kind;a[19]=r.Core__DOT__exec__DOT__biu__DOT__sendQueue__DOT__cnt;
      a[20]=((n.idle&1)?0:r.Core__DOT__exec__DOT__liveQuotas_0)+((n.idle&2)?0:r.Core__DOT__exec__DOT__liveQuotas_1);
#define MP_QUEUE(I) do { \
      unsigned count=(r.Core__DOT__exec__DOT__biu__DOT__evq_##I##__DOT__enq_ptr_value-r.Core__DOT__exec__DOT__biu__DOT__evq_##I##__DOT__deq_ptr_value)&15; \
      if (r.Core__DOT__exec__DOT__biu__DOT__evq_##I##__DOT__full) count=16; \
      a[21+I/4] |= count<<(8*(I%4)); \
      const unsigned mask=r.Core__DOT__exec__DOT__enmasks_##I; \
      a[25] |= mask<<(2*I); \
      const bool margin=a[19]+a[20]+r.Core__DOT__exec__DOT__margins_##I<=32; \
      if (count && mask && margin) a[26] |= 1u<<I; \
      if (count && (mask&n.eidle)) { a[27] |= 1u<<I; if (!margin) a[28] |= 1u<<I; } \
    } while(0)
      MP_QUEUE(0);MP_QUEUE(1);MP_QUEUE(2);MP_QUEUE(3);MP_QUEUE(4);MP_QUEUE(5);MP_QUEUE(6);MP_QUEUE(7);
      MP_QUEUE(8);MP_QUEUE(9);MP_QUEUE(10);MP_QUEUE(11);MP_QUEUE(12);MP_QUEUE(13);MP_QUEUE(14);MP_QUEUE(15);
#undef MP_QUEUE
      if (bool(a[27]&~a[28]) != bool(n.wake)) throw runtime_error("Morrison CPU probe event eligibility mismatch");
      a[29]=w(28);a[30]=w(MB_READY_CONTROL+R_CREDIT);
      a[31]=w(MB_READY_CONTROL+R_TAIL)-w(MB_READY_CONTROL+R_HEAD);a[32]=w(156);
      a[33]=w(MB_READY_CONTROL+R_PENDING);
      const auto slot=w(MB_READY_CONTROL+R_ALLOC)&63;
      a[34]=w((slot<32?MB_SHARED0:MB_SHARED1)+(slot&31)*64+44);
      a[35]=w(MB_READY_CONTROL+R_CLOSING);a[36]=w(MB_READY_CONTROL+R_DIR_ACTIVE);
      a[37]=w(MB_CONTROL+B_BANK0) | (w(MB_CONTROL+B_BANK1)<<8);
      a[38]=w(MB_CONTROL+B_EMITTING);a[39]=w(MB_CONTROL+B_ISSUING);
      a[40]=r.Core__DOT__exec__DOT__lsu__DOT__acRemaining;
      a[41]=w(MB_CONTROL+B_HOME_CLOSED);a[42]=w(64);a[43]=w(68);
      a[44]=w(72);a[45]=w(MB_CONTROL+B_PRODUCED);a[46]=w(MB_CONTROL+B_EXPANDED);
      a[47]=r.Core__DOT__fetch__DOT__icache__DOT__mem_req_valid_0 | (n.advance<<1);
      // Same 48-word schema as cpu-joint, but all PUs on one cycle. Words6/7
      // stay zero here: only cpu-joint fills those with next-cycle validity.
      if (spatialSample) {
        p.spatialSamples.write(reinterpret_cast<const char*>(a.data()), sizeof(a));
        if (!p.spatialSamples) throw runtime_error("Simultaneous CPU sample write failed");
      }
    }
    prev=n;
  }
}
