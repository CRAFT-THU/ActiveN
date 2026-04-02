/*
 * SNN initialization — C implementation
 *
 * Called from snn_main.S _start after stack setup.
 * Loads compact SPM init data from DRAM using the fixed boot header,
 * populates the runtime SPM metadata, then runs the init loop
 * (state += input, clear input).
 *
 * Returns hartid so ASM can set up s-registers.
 *
 * Memory map:
 *   0x20000000 - 0x20003FFF : Per-PU scratchpad (SPM)
 *   0x80100000              : Fixed SPM boot header (default DESCRIPTOR_BASE)
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
 *   [SPM_SIZE-8]:  init state (0=need init, 2=configured)
 *   [SPM_SIZE-12]: sync counter
 *   [SPM_SIZE-16]: num_mc
 *   [SPM_SIZE-20]: neuron stride (bytes)
 *   [SPM_SIZE-28]: expected XOR checksum
 *   [SPM_SIZE-32]: subtree XOR accumulator
 *   [SPM_SIZE-36]: boot cookie
 *
 * Boot header at DESCRIPTOR_BASE:
 *   +0 : magic
 *   +4 : version
 *   +8 : descriptor count
 *   +12: descriptor table base (absolute address)
 *   +16: descriptor stride in bytes
 *
 * Per-PU descriptor:
 *   +0 : init_src       (absolute address)
 *   +4 : init_bytes     (compact initializer size)
 *   +8 : nn_count
 *   +12: stride_bytes
 *   +16: num_mc
 *   +20: num_pu
 *   +24: expected_xor
 */

#define SPM_BASE        0x20000000u
#define SPM_SIZE        16384u
#define STOP_ADDR       0x40000000u
#define SPM_IMAGE_MAGIC 0x53504d49u
#define SPM_IMAGE_VERSION 1u
#define SNN_BOOT_COOKIE 0x534e4e31u
#define SPM_INIT_DESC_WORDS 7u

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
#define SPM_NUM_PU    (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 24))
#define SPM_EXPECTED_XOR (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 28))
#define SPM_XOR_ACC   (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 32))
#define SPM_BOOT_COOKIE (*(volatile unsigned int *)(SPM_BASE + SPM_SIZE - 36))

static inline unsigned int csrr_f14(void) {
    unsigned int id;
    __asm__ volatile ("csrr %0, 0xF14" : "=r"(id));
    return id;
}

static void boot_fail(unsigned int code) {
    (*(volatile unsigned int *)STOP_ADDR) = code;
    while (1)
        __asm__ volatile ("wfi");
}

/*
 * snn_init: called once per PU on first boot.
 *
 * Copies neuron data from the compact DRAM initializer, runs the init loop,
 * and leaves the PU in the boot-wait state.
 * Returns: hartid (caller sets up s-registers and sync tree in ASM).
 */
unsigned int snn_init(void) {
    unsigned int hartid = csrr_f14();

    volatile unsigned int *boot = (volatile unsigned int *)DESCRIPTOR_BASE;
    if (boot[0] != SPM_IMAGE_MAGIC || boot[1] != SPM_IMAGE_VERSION)
        boot_fail(0xbad10000u | hartid);

    unsigned int desc_count = boot[2];
    unsigned int desc_base = boot[3];
    unsigned int desc_stride = boot[4];
    if (hartid == 0 || hartid > desc_count || desc_stride < SPM_INIT_DESC_WORDS * 4)
        boot_fail(0xbad20000u | hartid);

    volatile unsigned int *desc =
        (volatile unsigned int *)(desc_base + (hartid - 1) * desc_stride);
    volatile unsigned int *src = (volatile unsigned int *)desc[0];
    unsigned int data_bytes = desc[1];
    volatile unsigned int *dst = (volatile unsigned int *)SPM_BASE;
    unsigned int nwords = data_bytes / 4;

    SPM_BOOT_COOKIE = 0;
    SPM_STATE = 0;
    for (unsigned int i = 0; i < nwords; i++)
        dst[i] = src[i];

    SPM_NNCOUNT = desc[2];
    SPM_STRIDE = desc[3];
    SPM_NUM_MC = desc[4];
    SPM_NUM_PU = desc[5];
    SPM_EXPECTED_XOR = desc[6];
    SPM_COUNTER = 0;
    SPM_XOR_ACC = 0;

    if (SPM_STRIDE == 0)
        boot_fail(0xbad30000u | hartid);

    if (SPM_NNCOUNT > 0 && data_bytes < (SPM_NNCOUNT + 1) * SPM_STRIDE)
        boot_fail(0xbad40000u | hartid);

    /* 2. Compute neuron end address using stride from SPM metadata */
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

    /* 3. Leave the PU waiting for the leader's runtime start signal. */
    SPM_COUNTER = 0;
    SPM_XOR_ACC = 0;
    SPM_BOOT_COOKIE = SNN_BOOT_COOKIE;
    SPM_STATE = 1;

    return hartid;
}
