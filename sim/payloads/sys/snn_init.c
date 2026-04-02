/*
 * SNN initialization — C implementation
 *
 * Called from snn_main.S _start after stack setup.
 * Loads SPM from DRAM descriptors, configures event handlers,
 * runs the init loop (state += input, clear input).
 *
 * Returns hartid so ASM can set up s-registers.
 *
 * Memory map:
 *   0x20000000 - 0x20003FFF : Per-PU scratchpad (SPM)
 *   0x80100000              : Descriptor table (default DESCRIPTOR_BASE)
 *
 * SPM layout per neuron (variable stride = 8 + 4*num_mc bytes):
 *   +0:  state (f32)
 *   +4:  input accumulator (f32)
 *   +8:  CSR start for MC0 (MC-local addr)
 *   +12: CSR start for MC1 (MC-local addr, if num_mc >= 2)
 *   ...
 * A sentinel neuron at the end has past-the-end CSR addresses.
 * The "end" for neuron N's CSR row in MC m is neuron (N+1)'s CSR start for MC m.
 *
 * SPM metadata (at top of SPM):
 *   [SPM_SIZE-4]:  neuron count
 *   [SPM_SIZE-8]:  init state (0=uninit, 1=preloaded, 2=configured)
 *   [SPM_SIZE-12]: sync counter
 *   [SPM_SIZE-16]: num_mc
 *   [SPM_SIZE-20]: neuron stride (bytes)
 *   [SPM_SIZE-28]: expected XOR checksum
 *   [SPM_SIZE-32]: subtree XOR accumulator
 */

#define SPM_BASE        0x20000000u
#define SPM_SIZE        16384u

#ifndef NUM_PU
#define NUM_PU 16
#endif

#ifndef DESCRIPTOR_BASE
#define DESCRIPTOR_BASE 0x80100000u
#endif

#define SPM_NNCOUNT   (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 4))
#define SPM_STATE     (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 8))
#define SPM_COUNTER   (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 12))
#define SPM_NUM_MC    (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 16))
#define SPM_STRIDE    (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 20))
#define SPM_XOR_ACC   (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 32))

/* Handler addresses defined in snn_main.S */
extern void syncinc(void);
extern void spike(void);
extern void updateOne(void);
extern void xorwait(void);
extern void xoracc(void);

static inline unsigned int csrr_f14(void) {
    unsigned int id;
    __asm__ volatile ("csrr %0, 0xF14" : "=r"(id));
    return id;
}

static inline void csrw_handler(int idx, void *addr) {
    unsigned int val = (unsigned int)addr;
    switch(idx) {
        case 0: __asm__ volatile ("csrw 0x700, %0" :: "r"(val)); break;
        case 1: __asm__ volatile ("csrw 0x701, %0" :: "r"(val)); break;
        case 2: __asm__ volatile ("csrw 0x702, %0" :: "r"(val)); break;
        case 3: __asm__ volatile ("csrw 0x703, %0" :: "r"(val)); break;
        case 4: __asm__ volatile ("csrw 0x704, %0" :: "r"(val)); break;
    }
}

static inline void csrw_argcnt(int idx, unsigned int cnt) {
    switch(idx) {
        case 0: __asm__ volatile ("csrw 0x710, %0" :: "r"(cnt)); break;
        case 1: __asm__ volatile ("csrw 0x711, %0" :: "r"(cnt)); break;
        case 2: __asm__ volatile ("csrw 0x712, %0" :: "r"(cnt)); break;
        case 3: __asm__ volatile ("csrw 0x713, %0" :: "r"(cnt)); break;
        case 4: __asm__ volatile ("csrw 0x714, %0" :: "r"(cnt)); break;
    }
}

/*
 * snn_init: called once per PU on first boot.
 *
 * If SPM_STATE == 0: copies neuron data from DRAM descriptor.
 * If SPM_STATE == 1: SPM pre-loaded by driver, skip copy.
 * In both cases: configures handlers, runs init loop, sets SPM_STATE=2.
 * Returns: hartid (caller sets up s-registers and sync tree in ASM).
 */
unsigned int snn_init(void) {
    unsigned int hartid = csrr_f14();
    unsigned int state = SPM_STATE;

    if (state == 0) {
        /* Copy neuron data from global memory */
        unsigned int *desc = (unsigned int *)(DESCRIPTOR_BASE + (hartid - 1) * 8);
        volatile unsigned int *src = (volatile unsigned int *)desc[0];
        unsigned int data_bytes = desc[1]; /* (nn_count+1) * stride */
        volatile unsigned int *dst = (volatile unsigned int *)SPM_BASE;
        unsigned int nwords = data_bytes / 4;
        for (unsigned int i = 0; i < nwords; i++)
            dst[i] = src[i];
        /* Compute nn_count and stride from descriptor */
        /* We don't know num_mc here, but stride and num_mc are written by datagen
         * into the SPM data block at fixed offsets, which we just copied. */
    }
    /* state == 1: SPM already pre-loaded by driver, metadata already set */

    /* 2. Configure event handlers */
    csrw_handler(0, syncinc);   /* tag 0: syncinc, 1 arg (count) */
    csrw_argcnt(0, 1);
    csrw_handler(1, spike);     /* tag 1: spike, 2 args */
    csrw_argcnt(1, 2);
    csrw_handler(2, updateOne); /* tag 2: updateOne, 1 arg */
    csrw_argcnt(2, 1);
    csrw_handler(3, xorwait);   /* tag 3: xorwait, 1 arg */
    csrw_argcnt(3, 1);
    csrw_handler(4, xoracc);    /* tag 4: xoracc, 1 arg */
    csrw_argcnt(4, 1);

    /* 3. Compute neuron end address using stride from SPM metadata */
    unsigned int nn_count = SPM_NNCOUNT;
    unsigned int stride = SPM_STRIDE;
    unsigned int nn_end = SPM_BASE + nn_count * stride;

    /* Init loop: state += input, clear input.
     * MUST use lw/sw (not flw/fsw) because the architecture has shared
     * int/FP registers: xN and fN are the same physical register.
     * C float ops use flw/fsw which would clobber our pointer regs.
     * Instead, use integer loads and inline asm for fadd.s.
     */
    for (unsigned int addr = SPM_BASE; addr < nn_end; addr += stride) {
        volatile unsigned int *st = (volatile unsigned int *)addr;
        volatile unsigned int *input = (volatile unsigned int *)(addr + 4);
        unsigned int s = *st;
        unsigned int inp = *input;
        /* fadd.s using shared regs: load to t0/f5 and t1/f6,
         * fadd.s f5, f5, f6, then read t0/x5 */
        unsigned int result;
        __asm__ volatile (
            "mv t0, %1\n\t"        /* t0(x5)/f5 = state bits */
            "mv t1, %2\n\t"        /* t1(x6)/f6 = input bits */
            "fadd.s f5, f5, f6\n\t" /* f5 = state + input */
            "mv %0, t0\n\t"        /* result = x5 (modified by fadd) */
            : "=r"(result)
            : "r"(s), "r"(inp)
            : "t0", "t1"
        );
        *st = result;
        *input = 0;
    }

    /* 4. Init sync counter and mark fully configured */
    SPM_COUNTER = 0;
    SPM_XOR_ACC = 0;
    SPM_STATE = 2;

    return hartid;
}
