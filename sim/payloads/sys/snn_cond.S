  # Conductance-based one-timestep payload.
  #
  # Keep this as a separate assembly entrypoint while sharing the boot,
  # scheduling, scatter, and completion machinery with snn_main.S.

#define CONDUCTANCE 1
#include "snn_main.S"
