/* Shared compile-time selection for boot handlers and inline learning macros.
 * The image's H_RULE is checked at boot; it never selects runtime code. */
#ifndef BR_RULE
#error Build a rule-specific Brunel payload with BR_RULE=0 or BR_RULE=1
#endif
#if BR_RULE == 0
#define SPIKE_ENTRY br_spike_additive
#elif BR_RULE == 1
#define SPIKE_ENTRY br_spike
#else
#error Invalid BR_RULE: use 0 for Additive or 1 for Morrison
#endif
