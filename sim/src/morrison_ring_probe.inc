// All-cycle, passive ring observations. Events describe the preceding edge;
// occupancies and admission decisions describe this resolved pre-edge state.
void SoftSystemBackend::observeMorrisonRing(uint64_t) {
  static const bool enabled=std::getenv("MORRISON_RING_PROBE")!=nullptr;
  if (!enabled) return;
  if (!morrison_probe::enabled()) throw runtime_error("ring probe requires phase probe");
  static morrison_probe::Counters counts{"ring-probe.csv"};
  static uint32_t previousStep=0,previousPhase=0;
  const auto [step,phase]=morrisonProbePhase();
  const bool current=step && (phase==2 || phase==3);
  const bool previous=previousStep && (previousPhase==2 || previousPhase==3);
  for (unsigned endpoint=0;endpoint<=cfg.numMC;++endpoint) {
    if (current) {
      const auto add=[&](const char *key,uint64_t n=1){counts.add(step,phase,endpoint,key,n);};
      add("cycles");add("occupied",ring.validAt(endpoint));
      add("can_inject",ringCanInject(endpoint));
      if (ring.validAt(endpoint)) {
        const auto &r=ring[endpoint];
        const bool local=endpoint && r.dst>=1+(endpoint-1)*(cfg.numPU/cfg.numMC) &&
          r.dst<1+endpoint*(cfg.numPU/cfg.numMC);
        add("local_arrival",local);add("local_arrival_rejected",local&&!memif_ring_accept[endpoint]);
        add("ejected",memif_ring_accept[endpoint]);
      }
      if (endpoint) {
        const bool pending=drams[endpoint-1].selectedRemoteUnicast();
        add("selected_remote",pending);
        add("remote_injection_blocked",pending&&!ringCanInject(endpoint));
        const auto next=(endpoint+1)%(cfg.numMC+1);
        add("remote_blocked_next_occupied",pending&&ring.validAt(next));
        add("remote_blocked_current_occupied",pending&&ring.validAt(endpoint)&&!memif_ring_accept[endpoint]);
      }
    }
    if (previous && endpoint) {
      const auto &e=drams[endpoint-1].cycleEvents();
      counts.add(previousStep,previousPhase,endpoint,"event_cycles");
      counts.add(previousStep,previousPhase,endpoint,"remote_injected",e.mshrUnicastRemoteInjected);
      counts.add(previousStep,previousPhase,endpoint,"ring_local_delivered",e.ringUnicastLocalDelivered);
      counts.add(previousStep,previousPhase,endpoint,"mshr_local_delivered",e.mshrUnicastLocalRetired);
    }
  }
  previousStep=step;previousPhase=phase;
}
