/* Shared compile-time proxy geometry. The ordinary neuron/header ABI stays fixed. */
#ifndef BRUNEL_PROXY_CONFIG_H
#define BRUNEL_PROXY_CONFIG_H
#ifndef BR_BULK
#define BR_BULK 0
#endif
#ifndef BR_BULK_TREE
#define BR_BULK_TREE 0
#endif
#ifndef BR_BULK_DISTANT
#define BR_BULK_DISTANT 0
#endif
#ifndef BR_BULK_UNCOUNTED
#define BR_BULK_UNCOUNTED 0
#endif
#if BR_BULK_UNCOUNTED && (!BR_BULK || !BR_BULK_TREE)
#error Uncounted overlap requires the bulk completion tree
#endif
#if BR_BULK
#include "morrison_bulk_layout.h"
#define BR_CONTROL_ID 6
#define BR_REPORT_ID 7
#define BR_SERVICE_ID 8
#define BR_WORK_ID 9
#else
#define BR_CONTROL_ID 2
#define BR_REPORT_ID 3
#define BR_SERVICE_ID 6
#define BR_WORK_ID 8
#endif
#ifndef BR_MORRISON
#define BR_MORRISON 0
#endif
#if BR_MORRISON
#include "morrison_layout.h"
#endif
#ifndef BR_PROXY
#define BR_PROXY 0
#endif
#ifndef BR_PROXY_SIZE
#define BR_PROXY_SIZE 4
#endif
#ifndef BR_PROXY_TRACES
#define BR_PROXY_TRACES 1
#endif
#ifndef BR_PROXY_RELAY
#define BR_PROXY_RELAY 0
#endif
#if BR_MORRISON
#if !BR_PROXY || BR_PROXY_TRACES || BR_PROXY_RELAY || (BR_PROXY_SIZE != 1 && BR_PROXY_SIZE != 4)
#error Compact Morrison supports home updates with group size 1 or 4
#endif
#elif BR_PROXY_SIZE != 2 && BR_PROXY_SIZE != 4 && BR_PROXY_SIZE != 8
#error Brunel proxy group size must be 2, 4 or 8
#endif
#if BR_PROXY_TRACES != 0 && BR_PROXY_TRACES != 1
#error BR_PROXY_TRACES must be zero or one
#endif
#if BR_PROXY_RELAY != 0 && BR_PROXY_RELAY != 1
#error BR_PROXY_RELAY must be zero or one
#endif
#if BR_PROXY_RELAY && (!BR_PROXY || BR_PROXY_TRACES)
#error Relay updates require proxy placement without replicated traces
#endif
#if BR_MORRISON
#define BR_PROXY_CONFIG MR_PROXY_CONFIG
#define BR_PROXY_JOBS MR_PROXY_JOBS
#define BR_PROXY_ROWS MR_DESCRIPTOR
#define BR_PROXY_CAPACITY (MR_MAX_NEURONS * BR_PROXY_SIZE)
#else
#define BR_PROXY_CONFIG 0x980
#define BR_PROXY_JOBS 0xa00
#define BR_PROXY_ROWS 0xd00
#define BR_PROXY_CAPACITY (22 * BR_PROXY_SIZE)
#endif
#define BR_PROXY_MAGIC (0x50580000 | (BR_PROXY_RELAY << 9) | (BR_PROXY_TRACES << 8) | BR_PROXY_SIZE)
#define BR_RELAY_UPDATES (BR_PROXY && BR_PROXY_RELAY)
#define BR_HOME_UPDATES (BR_PROXY && !BR_PROXY_TRACES && !BR_PROXY_RELAY)
#endif
