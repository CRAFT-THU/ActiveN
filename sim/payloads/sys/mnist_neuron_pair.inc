/* Shared dense/sparse E/I integration, preserving the FP32 operation order. */
neuron_pair_update:
  /* Shared E/I conductance factors; GE half and GI full are identical. */
  li    a0, F_GE_HALF_DECAY
  li    a1, F_GI_HALF_DECAY
  li    a2, F_GE_DECAY
  li    s9, 0
  lw    t1, EXC_FLAGS(s0)
  andi  t1, t1, NEURON_FF_INPUT
  sw    t1, EXC_FLAGS(s0)
  lw    t2, EXC_REFRACTORY(s0)
  beqz  t2, neuron_pair_exc_active
  addi  t2, t2, -1
  sw    t2, EXC_REFRACTORY(s0)
  j     neuron_pair_exc_done

neuron_pair_exc_active:
  lw    t2, EXC_GE(s0)
  lw    t3, EXC_GI(s0)
  lw    t4, EXC_V(s0)
  fmul.s t5, t2, a0
  fmul.s t6, t3, a1

  li    a4, F_EXC_REST
  fsub.s a4, a4, t4
  fsub.s a5, zero, t4
  fmul.s a5, t5, a5
  fadd.s a4, a4, a5
  li    a5, F_EXC_INH_REV
  fsub.s a5, a5, t4
  fmul.s a5, t6, a5
  fadd.s a4, a4, a5
  li    a5, F_EXC_DT
  fmul.s a4, a4, a5
  fadd.s t4, t4, a4
  sw    t4, EXC_V(s0)

  fmul.s t2, t2, a2
  sw    t2, EXC_GE(s0)
  fmul.s t3, t3, a0
  sw    t3, EXC_GI(s0)
  lw    t2, EXC_THETA(s0)
  li    a4, F_THETA_DECAY
  fmul.s t2, t2, a4
  sw    t2, EXC_THETA(s0)
  li    a4, F_EXC_THRESHOLD_OFFSET
  fsub.s a4, t2, a4
  flt.s a5, a4, t4
  beqz  a5, neuron_pair_exc_done
  li    t4, F_EXC_RESET
  sw    t4, EXC_V(s0)
  li    t4, 10
  sw    t4, EXC_REFRACTORY(s0)
  li    a4, F_THETA_INCREMENT
  fadd.s t2, t2, a4
  sw    t2, EXC_THETA(s0)
  ori   t1, t1, NEURON_FIRED
  sw    t1, EXC_FLAGS(s0)
  li    s9, 1

neuron_pair_exc_done:
  li    s8, 0
  sw    zero, INH_FLAGS(s2)
  lw    t1, INH_REFRACTORY(s2)
  beqz  t1, neuron_pair_inh_active
  addi  t1, t1, -1
  sw    t1, INH_REFRACTORY(s2)
  j     neuron_pair_inh_done

neuron_pair_inh_active:
  lw    t2, INH_GE(s2)
  lw    t3, INH_GI(s2)
  lw    t4, INH_V(s2)
  fmul.s t5, t2, a0
  fmul.s t6, t3, a1

  li    a4, F_INH_REST
  fsub.s a4, a4, t4
  fsub.s a5, zero, t4
  fmul.s a5, t5, a5
  fadd.s a4, a4, a5
  li    a5, F_INH_INH_REV
  fsub.s a5, a5, t4
  fmul.s a5, t6, a5
  fadd.s a4, a4, a5
  li    a5, F_INH_DT
  fmul.s a4, a4, a5
  fadd.s t4, t4, a4
  sw    t4, INH_V(s2)

  fmul.s t2, t2, a2
  sw    t2, INH_GE(s2)
  fmul.s t3, t3, a0
  sw    t3, INH_GI(s2)
  li    a4, F_INH_THRESHOLD
  flt.s a5, a4, t4
  beqz  a5, neuron_pair_inh_done
  li    t4, F_INH_RESET
  sw    t4, INH_V(s2)
  li    t4, 4
  sw    t4, INH_REFRACTORY(s2)
  li    t4, NEURON_FIRED
  sw    t4, INH_FLAGS(s2)
  li    s8, 1

neuron_pair_inh_done:
  /* Preserve the model's one-step recurrent delay within this pair. */
  lw    t2, EXC_FLAGS(s0)
  andi  t2, t2, NEURON_FIRED
  beqz  t2, neuron_pair_done
  lw    t3, INH_GE(s2)
  li    t4, F_E_TO_I
  fadd.s t3, t3, t4
  sw    t3, INH_GE(s2)
neuron_pair_done:
  jr    s7

