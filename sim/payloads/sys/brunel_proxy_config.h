/* Shared compile-time proxy geometry. The ordinary neuron/header ABI stays fixed. */
#ifndef BRUNEL_PROXY_CONFIG_H
#define BRUNEL_PROXY_CONFIG_H
#ifndef BR_PROXY
#define BR_PROXY 0
#endif
#ifndef BR_PROXY_SIZE
#define BR_PROXY_SIZE 4
#endif
#ifndef BR_PROXY_TRACES
#define BR_PROXY_TRACES 1
#endif
#if BR_PROXY_SIZE != 2 && BR_PROXY_SIZE != 4 && BR_PROXY_SIZE != 8
#error Brunel proxy group size must be 2, 4 or 8
#endif
#if BR_PROXY_TRACES != 0 && BR_PROXY_TRACES != 1
#error BR_PROXY_TRACES must be zero or one
#endif
#define BR_PROXY_CONFIG 0x980
#define BR_PROXY_JOBS 0xa00
#define BR_PROXY_ROWS 0xd00
#define BR_PROXY_CAPACITY (22 * BR_PROXY_SIZE)
#define BR_PROXY_MAGIC (0x50580000 | (BR_PROXY_TRACES << 8) | BR_PROXY_SIZE)
#define BR_HOME_UPDATES (BR_PROXY && !BR_PROXY_TRACES)
#endif
