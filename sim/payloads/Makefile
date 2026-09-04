.PHONY: clean all-tests legacy-bins force-mnist-train

default: sys/snn_main.bin

CORE_CNT ?= 512
REPORT ?= yes
PREFIX ?= riscv64-unknown-none-elf-

test.double: test.S
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -DCORE_CNT=${CORE_CNT} -DDOUBLE_DRAM -DREPORT=${REPORT} -o test.double ./test.S

test.double.bin: test.double
	${PREFIX}objcopy -j .text -j .data -O binary ./test.double test.double.bin

test.single: test.S
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -DCORE_CNT=${CORE_CNT} -DREPORT=${REPORT} -o test.single ./test.S

test.single.bin: test.single
	${PREFIX}objcopy -j .text -j .data -O binary ./test.single test.single.bin

legacy-bins: test.single.bin test.double.bin

# Assembly tests (RV32IMF)
ASM_SRCS := $(wildcard asm/*.S)
ASM_BINS := $(ASM_SRCS:.S=.bin)

asm/%.bin: asm/%.S linker.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -o asm/$*.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary asm/$*.elf $@

# C tests (RV32I, -O1)
C_SRCS := $(wildcard c/*.c)
C_BINS := $(C_SRCS:.c=.bin)

c/%.bin: c/%.c c/crt0.S c/linker_c.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32i -nostdlib -nostartfiles -Wl,-T,c/linker_c.ld -O1 -o c/$*.elf c/crt0.S $<
	${PREFIX}objcopy -j .text -j .data -O binary c/$*.elf $@

all-tests: $(ASM_BINS) $(C_BINS)

# System-level tests (multicore, RV32I + custom AM instructions)
NUM_PU ?= 16
SNN_VERIFY_RESULT ?= 1
SNN_WARMUP_STEPS ?= 1
SNN_TIMED_STEPS ?= 4
SNN_DRAIN_MODE ?= 1
CSR_NUM_PU ?= 64
CSR_NUM_MC ?= 2
MEM_LINE_BYTES ?= 64
MNIST_ARCH := rv32ima_zfinx_zicsr
MNIST_CFLAGS := -O3 -fomit-frame-pointer -ffp-contract=off -fno-jump-tables
MNIST_RULE ?= three-trace
MNIST_RULE_ID := $(if $(filter one-trace,$(MNIST_RULE)),1,$(if $(filter two-trace,$(MNIST_RULE)),2,$(if $(filter three-trace,$(MNIST_RULE)),3,)))
ifeq ($(MNIST_RULE_ID),)
$(error MNIST_RULE must be one-trace, two-trace, or three-trace)
endif
MNIST_RECURRENT_MODE ?= tree
MNIST_RECURRENT_MODE_ID := $(if $(filter scatter,$(MNIST_RECURRENT_MODE)),0,$(if $(filter tree,$(MNIST_RECURRENT_MODE)),1,$(if $(filter tree-aggregate,$(MNIST_RECURRENT_MODE)),2,)))
ifeq ($(MNIST_RECURRENT_MODE_ID),)
$(error MNIST_RECURRENT_MODE must be scatter, tree, or tree-aggregate)
endif
MNIST_TRACE_BUNDLE ?= 0
ifeq ($(filter 0 1,$(MNIST_TRACE_BUNDLE)),)
$(error MNIST_TRACE_BUNDLE must be 0 or 1)
endif
MNIST_DEFINES := -DMNIST_RULE=$(MNIST_RULE_ID) -DMNIST_RECURRENT_MODE=$(MNIST_RECURRENT_MODE_ID) -DMNIST_TRACE_BUNDLE=$(MNIST_TRACE_BUNDLE)

sys/pingpong_test.bin: sys/pingpong_test.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32i_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -o sys/pingpong_test.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary sys/pingpong_test.elf $@

sys/sha256_concurrent.bin: sys/sha256_concurrent.S sys/sha256_lib.c sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imaf_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -o sys/sha256_concurrent.elf sys/sha256_concurrent.S sys/sha256_lib.c
	${PREFIX}objcopy -j .text -j .data -O binary sys/sha256_concurrent.elf $@

sys/spm_init.bin: sys/spm_init.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32ia_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -o sys/spm_init.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary sys/spm_init.elf $@

sys/barrier_test.bin: sys/barrier_test.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32ia_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -o sys/barrier_test.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary sys/barrier_test.elf $@

.PHONY: force-csr-scatter-data
force-csr-scatter-data:

sys/csr_scatter_data.S: sys/gen_csr_data.py force-csr-scatter-data
	python3 sys/gen_csr_data.py --num-pu ${CSR_NUM_PU} --line-bytes ${MEM_LINE_BYTES} sys/csr_scatter_data.S

sys/csr_scatter_test.bin: sys/csr_scatter_test.S sys/csr_scatter_data.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32ima_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -Isys -O1 -DNUM_PU=${CSR_NUM_PU} -DNUM_MC=${CSR_NUM_MC} -DMEM_LINE_BYTES=${MEM_LINE_BYTES} -o sys/csr_scatter_test.elf sys/csr_scatter_test.S
	${PREFIX}objcopy -j .text -j .data -O binary sys/csr_scatter_test.elf $@

sys/csr_scatter_dram1.bin: sys/csr_scatter_dram1.S sys/csr_scatter_data.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32im_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -Isys -O1 -DMEM_LINE_BYTES=${MEM_LINE_BYTES} -o sys/csr_scatter_dram1.elf sys/csr_scatter_dram1.S
	${PREFIX}objcopy -j .text -j .data -O binary sys/csr_scatter_dram1.elf $@

sys/config_rom_test.bin: sys/config_rom_test.S sys/linker_system.ld force-csr-scatter-data
	${PREFIX}gcc -mabi=ilp32 -march=rv32i_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -DNUM_PU=${CSR_NUM_PU} -DNUM_MC=${CSR_NUM_MC} -DMEM_LINE_BYTES=${MEM_LINE_BYTES} -o sys/config_rom_test.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary sys/config_rom_test.elf $@

sys/local_send_test.bin: sys/local_send_test.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32i_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -o sys/local_send_test.elf sys/local_send_test.S
	${PREFIX}objcopy -j .text -j .data -O binary sys/local_send_test.elf $@

sys/random_noc_test.bin: sys/random_noc_test.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imaf_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -O1 -o sys/random_noc_test.elf sys/random_noc_test.S
	${PREFIX}objcopy -j .text -j .data -O binary sys/random_noc_test.elf $@

sys/snn_main.bin: sys/snn_main.S sys/snn_init.c sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imaf_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -DNUM_PU=${NUM_PU} -DSNN_VERIFY_RESULT=${SNN_VERIFY_RESULT} -DSNN_WARMUP_STEPS=${SNN_WARMUP_STEPS} -DSNN_TIMED_STEPS=${SNN_TIMED_STEPS} -DSNN_DRAIN_MODE=${SNN_DRAIN_MODE} -DMEM_LINE_BYTES=${MEM_LINE_BYTES} -O1 -o sys/snn_main.elf sys/snn_main.S sys/snn_init.c
	${PREFIX}objcopy -j .text -j .data -O binary sys/snn_main.elf $@

sys/snn_cond.bin: sys/snn_cond.S sys/snn_main.S sys/snn_init.c sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imaf_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -DNUM_PU=${NUM_PU} -DSNN_VERIFY_RESULT=${SNN_VERIFY_RESULT} -DSNN_WARMUP_STEPS=0 -DSNN_TIMED_STEPS=1 -DSNN_DRAIN_MODE=${SNN_DRAIN_MODE} -DMEM_LINE_BYTES=${MEM_LINE_BYTES} -O1 -o sys/snn_cond.elf sys/snn_cond.S sys/snn_init.c
	${PREFIX}objcopy -j .text -j .data -O binary sys/snn_cond.elf $@

sys/mnist_train.bin: force-mnist-train sys/mnist_train.S sys/snn_init.c sys/linker_mnist_train.ld
	${PREFIX}gcc -mabi=ilp32 -march=$(MNIST_ARCH) -nostdlib -nostartfiles -Wl,-T,sys/linker_mnist_train.ld -Isys -DMEM_LINE_BYTES=32 -DMNIST_TRAIN=1 $(MNIST_DEFINES) $(MNIST_CFLAGS) -o sys/mnist_train.elf sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary sys/mnist_train.elf $@

force-mnist-train:

sys-tests: sys/pingpong_test.bin sys/sha256_concurrent.bin sys/spm_init.bin sys/barrier_test.bin sys/local_send_test.bin sys/random_noc_test.bin sys/config_rom_test.bin
	@echo "All system tests built successfully"

clean:
	rm -f test.single test.double test.single.bin test.double.bin
	rm -f asm/*.bin asm/*.elf c/*.bin c/*.elf sys/*.bin sys/*.elf
