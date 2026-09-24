/* Named offsets for the timestep-buffer BrHeader / BrNeuron / BrReplay ABI.
 * Source structs and size assertions: brunel.h. */
#include "brunel_proxy_config.h"
.equ H_MAGIC, 0
.equ H_PU, 4
.equ H_NUM_PU, 8
.equ H_RULE, 12
.equ H_NN, 16
.equ H_STEP, 20
.equ H_COMPLETED, 24
.equ H_PHASE, 28
.equ H_WARMUP, 32
.equ H_STEPS, 36
.equ H_MC, 40
.equ H_ACTIVE_MCS, 44
.equ H_EXCITATORY_END, 44 /* Boot replaces the unused MC mask with the E end. */
.equ H_WORK_NEURON, 48
.equ H_WORK_LINE, 52
.equ H_WORK_MC, 56
.equ H_SENT, 60
.equ H_RECEIVED, 64
.equ H_REVERSE_RECEIVED, 68
.equ H_ISSUED, 72
.equ H_CONSUMED, 76
.equ H_READY0, 80
.equ H_READY1, 84
.equ H_READY2, 88
.equ H_READY3, 92
.equ H_REPORT_COUNT, 96
.equ H_REPORT_SUM, 100
.equ H_EXPECTED_FORWARD, 104
.equ H_EXPECTED_REVERSE, 108
.equ H_REPORT_MCS, 112
.equ H_NEURON_END, 112 /* Boot replaces unused report_mcs with the end pointer. */
.equ H_JOIN_COUNT, 116
.equ H_BUFFER_UPDATES, 120
.equ H_MSHR_LOADS, 124
.equ H_MSHR_COMMITS, 128
.equ H_OVERFLOW_ROWS, 132
.equ H_WRITE_WORDS, 136
.equ H_POOL_FULL, 140
.equ H_FORWARD_WEIGHTS, 144
.equ H_REVERSE_EVENTS, 148
.equ H_P11, 184
.equ H_P21, 188
.equ H_P31, 192
.equ H_P32, 196
.equ H_P33, 200
.equ H_EPSC, 204
.equ H_DELIVERY, 208
.equ H_EXTERNAL_MEAN, 212
.equ H_EXTERNAL_LIMIT, 216
/* The image contains the original constants here. Boot replaces these three
 * unused runtime words with derived constants and the fired-list count. */
.equ H_EXTERNAL_CURRENT, 208
.equ H_FIRE_COUNT, 212
.equ H_TREE_REPORTS, 216
.equ H_EXTERNAL_WEIGHT, 220
.equ H_DEP, 224
.equ H_POT, 228
.equ H_WMAX, 232
.equ H_DECAY_PRE, 236
.equ H_DECAY_POST, 240
.equ H_MC_DEST, 244
.equ H_WEIGHT_LOADS, 248
.equ H_MSHR_WAITS, 252
.equ N_V, 0
.equ N_IE, 4
.equ N_DE, 8
.equ N_II, 12
.equ N_DI, 16
/* Images hold split currents. Boot combines them into IE/DE and clears II/DI. */
.equ N_I_TOTAL, N_IE
.equ N_D_TOTAL, N_DE
.equ N_REFRAC, 20
/* Images hold raw traces; boot scales E X/Y by H_POT/H_DEP and zeros I X/Y. */
.equ N_X, 24
.equ N_Y, 28
.equ N_RNG, 32
.equ N_FIRE, 36
.equ N_RESERVED_FIRE, 40
.equ N_GID, 44
.equ N_WEIGHTS, 48
.equ N_COUNT, 52
.equ N_TARGETS, 56
.equ N_STATIC_DIR, 60
.equ N_REVERSE_DIR, 64
.equ N_INPUT_E, 68
.equ N_INPUT_I, 72
.equ N_BUFFER, 76 /* BR_PROXY: descriptor offset shared by the group members */
.equ X_COUNT, 0
.equ X_PEAK, 4
.equ X_CURSOR, 8
.equ X_SPILL_BASE, 12
.equ X_SPILLED, 16
.equ X_MANUAL_REVERSE, 20
.equ X_SCATTER_REVERSE, 24
.equ X_CAPACITY, 28
.equ X_SPILL_CACHED, 32

/* Timestep-buffer ABI, reuses the former unused writeback buckets. */
.equ H_BUF_USED, 152
.equ H_MSHR_COUNT, 156
.equ H_MSHR_HEAD, 160
.equ H_MSHR_TAIL, 164
.equ H_MSHR_SCHEDULED, 168
.equ H_RNG0, 172
.equ H_RNG1, 176
.equ H_MSHR_PEAK, 180

/* BR_PROXY=1: compact synapse descriptors, not duplicated neuron state. */
.equ PROXY_CONFIG, BR_PROXY_CONFIG
.equ PROXY_JOBS, BR_PROXY_JOBS
.equ PROXY_ROWS, BR_PROXY_ROWS
.equ PROXY_CAPACITY, BR_PROXY_CAPACITY
.equ G_COUNT, 32
.equ G_EXC_END, 36
.equ G_JOBS, 40
.equ G_MARKERS, 44
.equ G_EXPECTED, 48
.equ G_TRACE_WORK, 52
.equ G_MAGIC, 56
.equ G_NOTIFY_CURSOR, 60
.equ P_X, 0
.equ P_FIRE, 4
.equ P_GID, 8
.equ P_WEIGHTS, 12
.equ P_COUNT, 16
.equ P_STATIC_DIR, 20
