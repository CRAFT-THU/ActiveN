// Passive forward-path observations at the resolved pre-edge boundary.
namespace {
struct MorrisonFlowProbe {
  struct StaticItem { uint64_t token; uint32_t offset,weight; };
  struct Context { unsigned handler=99; bool spike=false; std::array<uint32_t,4> args{};uint64_t token=0; };
  std::vector<std::array<Context,2>> contexts;
  std::vector<std::deque<StaticItem>> statics;
  std::vector<std::array<uint32_t,3>> previousBanks;
  std::vector<std::pair<uint32_t,uint32_t>> expectedJobs;
  std::vector<std::array<uint32_t,16>> events;
  std::ofstream samples;
  struct Owner { std::array<uint64_t,16> counts{};std::array<uint64_t,16> instructions{}; };
  std::map<std::pair<unsigned,unsigned>,std::vector<Owner>> owners;
  std::map<std::tuple<unsigned,unsigned,unsigned,unsigned>,uint64_t> network;
  unsigned step=0;
  std::vector<Owner> *current=nullptr;
  explicit MorrisonFlowProbe(unsigned pus):contexts(pus+1),statics(pus+1),previousBanks(pus+1),expectedJobs(pus+1) {
    samples.open(std::filesystem::path(std::getenv("MORRISON_FLOW_PROFILE"))/"flow-state.bin",std::ios::binary);
    if(!samples)throw std::runtime_error("Cannot open forward-flow samples");
  }
  static unsigned traffic(uint16_t tag,uint16_t destination) {
    if(tag==0xc0c)return 0; if(tag==0x405)return 1;
    // Handler numbers may alias memory opcodes (notably completion handler2).
    if(destination<0x8001)return 6;
    if((tag&255)==2)return 2;if((tag&255)==0)return 3;
    if((tag&255)==0x11)return 4;if((tag&255)==0x10)return 5;return 6;
  }
  void event(uint64_t cycle,unsigned s,unsigned pu,unsigned kind,unsigned peer,unsigned tag,
             const std::array<uint32_t,4> &a={},uint64_t token=0,unsigned extra=0) {
    events.push_back({uint32_t(cycle),uint32_t(cycle>>32),s,2,pu,kind,peer,tag,
                     a[0],a[1],a[2],a[3],uint32_t(token),uint32_t(token>>32),extra,0});
  }
  ~MorrisonFlowProbe() {
    const auto d=std::filesystem::path(std::getenv("MORRISON_FLOW_PROFILE"));
    std::ofstream out(d/"flow-events.bin",std::ios::binary);
    if(!events.empty())out.write(reinterpret_cast<const char*>(events.data()),events.size()*sizeof(events[0]));
    std::ofstream counters(d/"flow-owner.csv");
    counters<<"step,pu,key,count\n";
    const char *names[]={"source_pending_cycles","producer_queued","producer_margin_blocked","smt0_active","smt0_update","smt0_producer","instruction_valid","instruction_advance","instruction_stall","empty","emitting","copy_active","next_job_queued","proxy_handler_disabled","emitter_margin_blocked","both_idle"};
    for(const auto &[k,v]:owners)for(unsigned pu=1;pu<v.size();++pu){
      for(unsigned j=0;j<16;++j) if(v[pu].counts[j]) counters<<k.first<<','<<pu<<','<<names[j]<<','<<v[pu].counts[j]<<'\n';
      for(unsigned j=0;j<16;++j) if(v[pu].instructions[j]) counters<<k.first<<','<<pu<<",source_pending_instruction_handler"<<j<<','<<v[pu].instructions[j]<<'\n';
    }
    std::ofstream net(d/"flow-network.csv");net<<"step,router,input,class,blocked_samples\n";
    for(const auto &[k,n]:network){auto [s,r,i,t]=k;net<<s<<','<<r<<','<<i<<','<<t<<','<<n<<'\n';}
    std::ofstream pending(d/"flow-pending.csv");pending<<"pu,static_pending,spike0,spike1\n";
    for(unsigned pu=1;pu<statics.size();++pu)pending<<pu<<','<<statics[pu].size()<<','<<contexts[pu][0].spike<<','<<contexts[pu][1].spike<<'\n';
    if(!out||!counters||!net||!pending)std::abort();
  }
};
}
void SoftSystemBackend::observeMorrisonFlow(uint64_t cycle) {
  static const bool enabled=std::getenv("MORRISON_FLOW_PROFILE")!=nullptr;
  if(!enabled)return;
  if(!morrison_probe::enabled())throw runtime_error("flow probe requires phase probe");
  static MorrisonFlowProbe p(cfg.numPU);
  const auto [step,phase]=morrisonProbePhase();
  const bool forward=step && phase==2;
  if(forward && step!=p.step){p.step=step;auto &v=p.owners[{step,phase}];v.resize(cfg.numPU+1);p.current=&v;}
  for(unsigned pu=1;pu<=cfg.numPU;++pu){
    const auto &c=*cores[pu];const auto &r=*c.rootp;
    const auto &spm=r.Core__DOT__exec__DOT__lsu__DOT__spm__DOT__scratchpad_ext__DOT__Memory;
    const auto w=[&](unsigned a){return spm[a/MEM_BUS_WIDTH_B][a%MEM_BUS_WIDTH_B/4];};
    // H_STEP counts finished steps on every PU; H_COMPLETED is root-only.
    const unsigned currentStep=w(20)+1;
    const unsigned smt=r.Core__DOT__exec__DOT__uop_smsel==2;
    const bool advanced=r.Core__DOT__exec__DOT__valid && r.Core__DOT__exec__DOT__s0step;
    // Completion uses the old context before any same-edge wakeup takes over.
    if(advanced && r.Core__DOT__exec__DOT__uop_isMem && r.Core__DOT__exec__DOT__uop_memIsAtomic &&
       r.Core__DOT__exec__DOT____Vcellinp__lsu__req_bits_addr==BR_BASE+64 && w(28)==2){
      auto &ctx=p.contexts[pu][smt];
      if(!ctx.spike)throw runtime_error("forward completion without captured spike");
      p.event(cycle,currentStep,pu,7,0,5,ctx.args,ctx.token,smt);ctx.spike=false;
    }
    if(forward){
      auto &o=(*p.current)[pu];
      const unsigned idle=r.Core__DOT__exec__DOT__idlings;
      const unsigned eidle=r.Core__DOT__exec__DOT__eidlings;
      const unsigned eg=r.Core__DOT__exec__DOT__biu__DOT__sendQueue__DOT__cnt;
      const unsigned quota=r.Core__DOT__exec__DOT__liveQuotaSum;
      const auto count=[&](unsigned enq,unsigned deq,bool full){return full?16u:(enq-deq)&15u;};
      const unsigned q13=count(r.Core__DOT__exec__DOT__biu__DOT__evq_13__DOT__enq_ptr_value,r.Core__DOT__exec__DOT__biu__DOT__evq_13__DOT__deq_ptr_value,r.Core__DOT__exec__DOT__biu__DOT__evq_13__DOT__full);
      const unsigned q12=count(r.Core__DOT__exec__DOT__biu__DOT__evq_12__DOT__enq_ptr_value,r.Core__DOT__exec__DOT__biu__DOT__evq_12__DOT__deq_ptr_value,r.Core__DOT__exec__DOT__biu__DOT__evq_12__DOT__full);
      const unsigned q15=count(r.Core__DOT__exec__DOT__biu__DOT__evq_15__DOT__enq_ptr_value,r.Core__DOT__exec__DOT__biu__DOT__evq_15__DOT__deq_ptr_value,r.Core__DOT__exec__DOT__biu__DOT__evq_15__DOT__full);
      const unsigned fired=w(212);
      auto &[expectedStep,expected]=p.expectedJobs[pu];
      if(w(28)==2 && expectedStep!=currentStep){
        if(fired>MR_MAX_NEURONS)throw runtime_error("invalid fired list in flow observer");
        expectedStep=currentStep;expected=0;
        const unsigned width=w(MR_PARAMETERS+20);
        const auto jobs=[&](unsigned units,unsigned n){
          if(w(MB_DISPATCH_POLICY)==MB_DISPATCH_BALANCED)return std::max(1u,std::min(units,n));
          const unsigned span=(units+n-1)/n;
          return span ? (units+span-1)/span : 1u;
        };
        for(unsigned j=0;j<fired;++j){
          const unsigned neuron=w(MR_FIRED_LIST+4*j),slot=(neuron-BR_BASE-256)/32;
          expected+=jobs(w(MR_PARAMETERS+28)/32,std::min(width,4u));
          if(neuron<w(44))expected+=jobs((w(MB_ROW_POINTERS+4*slot+4)-w(MB_ROW_POINTERS+4*slot)+1023)/1024,width);
        }
      }
      const bool pending=w(28)==2 && w(MB_CONTROL+B_PRODUCED)<expected;
      if(pending){
        ++o.counts[0];o.counts[1]+=q13!=0;
        o.counts[2]+=q13 && eg+quota+r.Core__DOT__exec__DOT__margins_13>32;
        o.counts[3]+=!(idle&1);o.counts[4]+=!(idle&1)&&p.contexts[pu][0].handler==5;
        o.counts[5]+=!(idle&1)&&p.contexts[pu][0].handler==13;
        o.counts[6]+=r.Core__DOT__exec__DOT__valid;o.counts[7]+=advanced;
        o.counts[8]+=r.Core__DOT__exec__DOT__valid&&!r.Core__DOT__exec__DOT__s0step;
        o.counts[9]+=!r.Core__DOT__exec__DOT__valid;
        if(advanced && p.contexts[pu][smt].handler<16)++o.instructions[p.contexts[pu][smt].handler];
      }
      o.counts[10]+=w(MB_CONTROL+B_EMITTING)!=0;o.counts[11]+=r.Core__DOT__exec__DOT__lsu__DOT__acRemaining!=0;
      o.counts[12]+=q12!=0;o.counts[13]+=q12 && !r.Core__DOT__exec__DOT__enmasks_12;
      o.counts[14]+=q15 && eg+quota+r.Core__DOT__exec__DOT__margins_15>32;
      o.counts[15]+=idle==3;
      if((cycle+pu)%251==0){
        std::array<uint32_t,16> sample{uint32_t(cycle),uint32_t(cycle>>32),step,pu,
          expected,w(MB_CONTROL+B_PRODUCED),q13,q12,q15,idle,eidle,eg,quota,
          p.contexts[pu][0].handler,p.contexts[pu][1].handler,
          uint32_t(pending)|(uint32_t(r.Core__DOT__exec__DOT__valid)<<1)|(uint32_t(advanced)<<2)|
          (uint32_t(r.Core__DOT__exec__DOT__margins_13)<<8)|(uint32_t(r.Core__DOT__exec__DOT__margins_15)<<16)};
        p.samples.write(reinterpret_cast<const char*>(sample.data()),sizeof(sample));
      }
      // Remote send accepted by BIU (before its egress queue and the NoC).
      if(r.Core__DOT__exec__DOT__biu_msg_valid && r.Core__DOT__exec__DOT__biuAccepted){
        const unsigned tag=r.Core__DOT__exec__DOT__alu2&4095;
        std::array<uint32_t,4> a{r.Core__DOT__exec__DOT__directval_0,r.Core__DOT__exec__DOT__directval_1,r.Core__DOT__exec__DOT__directval_2,r.Core__DOT__exec__DOT__directval_3};
        const auto cls=MorrisonFlowProbe::traffic(tag,r.Core__DOT__exec__DOT__rs1val&65535);
        if(cls<6)p.event(cycle,step,pu,0,r.Core__DOT__exec__DOT__rs1val&65535,tag,a,0,smt);
      }
      if(c.ext_out_valid && routers[pu].buf.core_inject_accepting){
        const unsigned tag=c.ext_out_bits_tag;const auto cls=MorrisonFlowProbe::traffic(tag,c.ext_out_bits_dst);
        if(cls<6)p.event(cycle,step,pu,1,c.ext_out_bits_dst,tag,{c.ext_out_bits_data_0,c.ext_out_bits_data_1,c.ext_out_bits_data_2,c.ext_out_bits_data_3});
      }
      if(routers[pu].buf.core_eject_accepting){
        const auto *f=routers[pu].rt.peek(*routers[pu].buf.core_eject_presenting);
        if(f && (f->tag==0xc0c || f->tag==0x405))p.event(cycle,step,pu,2,f->src,f->tag,{f->data[0],f->data[1],f->data[2],f->data[3]});
      }
      auto &prev=p.previousBanks[pu];
      for(unsigned bank=0;bank<2;++bank){
        unsigned base=MB_CONTROL+B_BANK0+bank*B_BANK_BYTES,state=w(base);
        if(state!=prev[bank]){p.event(cycle,step,pu,6,bank,state,{w(base+B_ADDRESS),w(base+B_LENGTH),w(base+B_MC),w(base+B_KIND)},w(base+B_RETURN));prev[bank]=state;}
      }
      const unsigned remaining=r.Core__DOT__exec__DOT__lsu__DOT__acRemaining;
      if(bool(remaining)!=bool(prev[2]))p.event(cycle,step,pu,8,remaining,0,{w(MB_CONTROL+B_COPY),w(MB_CONTROL+B_EMITTING),0,0});
      prev[2]=remaining;
      if(cycle%251==0)for(unsigned input=0;input<routers[pu].links.num_forwards;++input){
        const auto &buf=routers[pu].buf;auto token=buf.forward_presenting[input];
        if(token && !buf.forward_accepting[input]){
          auto src=routers[pu].links.forwards[input]->first;
          const auto *f=routers[src].rt.peek(*token);
          if(f)++p.network[{step,pu,input,MorrisonFlowProbe::traffic(f->tag,f->dst)}];
        }
      }
    }
    // Core broadcast FIFO preserves line/entry order. Track only actual
    // static handler5 entries addressed to this PU, not the dummy recipients.
    if(core_mem_accept[pu].broadcast && core_mem[pu].bcast){
      const auto &b=*core_mem[pu].bcast;
      if((b.tag&15)==5 && b.flowToken){
        for(unsigned i=0;i<MEM_BUS_WIDTH_B/8;++i)if((b.line[2*i]&65535)==pu){
          p.statics[pu].push_back({b.flowToken,b.line[2*i]>>16,b.line[2*i+1]});
          p.event(cycle,currentStep,pu,4,0,b.tag,{b.line[2*i]>>16,b.line[2*i+1],0,0},b.flowToken,i);
        }
      }
    }
    const unsigned wake=r.Core__DOT__exec__DOT__biu__DOT___sched_wakeup_output;
    if(wake){
      const unsigned handler=r.Core__DOT__exec__DOT__biu__DOT___scheduleArb_io_chosen;
      const unsigned thread=wake==2;
      auto &ctx=p.contexts[pu][thread];
      if(ctx.spike)throw runtime_error("forward spike context replaced before completion");
      ctx.handler=handler;ctx.token=0;
      ctx.args={r.Core__DOT__exec__DOT___biu_sched_regs_0,r.Core__DOT__exec__DOT___biu_sched_regs_1,r.Core__DOT__exec__DOT___biu_sched_regs_2,r.Core__DOT__exec__DOT___biu_sched_regs_3};
      if(w(28)==2 && handler==5){
        ctx.spike=true;
        if(ctx.args[2]==0){
          auto &q=p.statics[pu];if(q.empty())throw runtime_error("static spike has no observed broadcast");
          const auto item=q.front();q.pop_front();
          if(item.offset!=ctx.args[0]||item.weight!=ctx.args[1])throw runtime_error("static broadcast/event order mismatch");
          ctx.token=item.token;
        }
      }
      if(w(28)==2 && (handler==12 || handler==5))p.event(cycle,currentStep,pu,3,thread,handler,ctx.args,ctx.token);
    }
    if(r.Core__DOT__exec__DOT__sendFastPath && advanced){
      auto &ctx=p.contexts[pu][smt];
      if(ctx.spike)throw runtime_error("spike yielded before receipt count");
      ctx.handler=r.Core__DOT__exec__DOT__alu2&15;
    }
  }
}
