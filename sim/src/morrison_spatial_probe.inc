// Read-only inspection after stage has resolved all transfers, before step.
// Queue records: cycleLo,cycleHi,step,pu,input,size,priorityCounts,src/dst,
// tag,chosenOutput,queuedOutputMask,distinctDestinations,wonArbitration,
// accepted,chosenSlot,0. Priority counts pack four8-bit counts; src/dst is
// src<<16|dst. An absent candidate has chosenOutput/chosenSlot=UINT32_MAX.
// Port records: cycleLo,cycleHi,step,pu,port,kind,src,dst,tag,valid,accepted,
// inputQueueSize. Kinds:0 neighbor ingress,1 core ingress,2 MemIf ingress,
// 3 core egress. Only kind3 has no associated router input queue.
namespace {
struct MorrisonSpatialProbe {
  std::ofstream queues,ports;
  explicit MorrisonSpatialProbe(unsigned pus) {
    const auto d=std::filesystem::path(morrisonSpatialDirectory());
    queues.open(d/"router-spatial.bin",std::ios::binary);
    ports.open(d/"ports-spatial.bin",std::ios::binary);
    const std::array<uint32_t,8> qh{0x4d535251,1,16,251,pus,8,0,0};
    const std::array<uint32_t,8> ph{0x4d535250,1,12,251,pus,8,0,0};
    queues.write(reinterpret_cast<const char*>(qh.data()),sizeof(qh));
    ports.write(reinterpret_cast<const char*>(ph.data()),sizeof(ph));
    if (!queues || !ports) throw std::runtime_error("Cannot open spatial NoC samples");
  }
};
}

void SoftSystemBackend::observeMorrisonSpatial(uint64_t cycle) {
  if (!morrisonSpatialDirectory()) return;
  if (!morrison_probe::enabled()) throw runtime_error("spatial probe requires phase probe");
  if (cycle%251) return;
  const auto [step,phase]=morrisonProbePhase();
  if (!step || phase!=2) return;
  static MorrisonSpatialProbe p(cfg.numPU);
  for (unsigned pu=1;pu<=cfg.numPU;++pu) {
    const auto &wr=routers[pu];
    const auto &rt=wr.rt;
    const auto &c=*cores[pu];
    const auto accepted=[&](unsigned output) {
      if (output<wr.links.num_forwards) {
        const auto [peer,input]=*wr.links.forwards[output];
        return routers[peer].buf.forward_accepting[input];
      }
      if (output==wr.links.num_forwards) return wr.buf.core_eject_accepting;
      if (wr.links.memif) {
        const auto [mc,port]=*wr.links.memif;
        return memif_eject_accept[mc] && *memif_eject_accept[mc]==port;
      }
      throw runtime_error("Spatial probe: unexpected router output");
    };
    const auto port=[&](unsigned input,unsigned kind,const Flit *f,bool fire) {
      const std::array<uint32_t,12> a{uint32_t(cycle),uint32_t(cycle>>32),step,pu,
        input,kind,f?f->src:0u,f?f->dst:0u,f?f->tag:0u,bool(f),fire,
        kind==3?0u:uint32_t(rt.inputQueueSize(input))};
      p.ports.write(reinterpret_cast<const char*>(a.data()),sizeof(a));
    };
    for (unsigned input=0;input<wr.links.num_forwards;++input) {
      const auto token=wr.buf.forward_presenting[input];
      const unsigned peer=wr.links.forwards[input]->first;
      port(input,0,token?routers[peer].rt.peek(*token):nullptr,wr.buf.forward_accepting[input]);
    }
    const Flit core{uint16_t(pu),c.ext_out_bits_dst,c.ext_out_bits_tag,
      {c.ext_out_bits_data_0,c.ext_out_bits_data_1,c.ext_out_bits_data_2,c.ext_out_bits_data_3}};
    port(wr.links.num_forwards,1,c.ext_out_valid?&core:nullptr,wr.buf.core_inject_accepting);
    if (wr.links.memif_inject)
      port(wr.links.num_forwards+1,2,wr.buf.memif_inject_presenting?&*wr.buf.memif_inject_presenting:nullptr,wr.buf.memif_inject_accepting);
    port(wr.links.num_forwards,3,wr.buf.core_eject_presenting?rt.peek(*wr.buf.core_eject_presenting):nullptr,wr.buf.core_eject_accepting);
    for (unsigned input=0;input<rt.numInputs();++input) {
      std::array<uint32_t,16> a{uint32_t(cycle),uint32_t(cycle>>32),step,pu,input};
      a[9]=a[14]=UINT32_MAX;
      const Flit *chosen=nullptr;
      std::array<uint16_t,ROUTER_Q_DEPTH> destinations{};
      rt.visitInputQueue(input,[&](size_t slot,const Flit &f) {
        ++a[5];a[6]+=1u<<(8*f.prio());a[10]|=1u<<rt.outputFor(f.dst);
        if (std::find(destinations.begin(),destinations.begin()+a[11],f.dst)==destinations.begin()+a[11])
          destinations[a[11]++]=f.dst;
        if (!chosen || f.prio()<chosen->prio()) {chosen=&f;a[14]=slot;}
      });
      if (chosen) {
        a[7]=(uint32_t(chosen->src)<<16)|chosen->dst;a[8]=chosen->tag;
        a[9]=rt.outputFor(chosen->dst);
        const auto winner=rt.peekToken(a[9]);
        a[12]=winner && winner->input==input;
        a[13]=a[12] && accepted(a[9]);
      }
      p.queues.write(reinterpret_cast<const char*>(a.data()),sizeof(a));
    }
  }
  if (!p.queues || !p.ports) throw runtime_error("Spatial NoC sample write failed");
}
