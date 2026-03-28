.PHONY: clean all-tests

default: test.single.bin test.double.bin

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

# Assembly tests (RV32IMF)
ASM_SRCS := $(wildcard asm/*.S)
ASM_BINS := $(ASM_SRCS:.S=.bin)

asm/%.bin: asm/%.S linker.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -o asm/$*.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary asm/$*.elf $@
	rm -f asm/$*.elf

# C tests (RV32I, -O1)
C_SRCS := $(wildcard c/*.c)
C_BINS := $(C_SRCS:.c=.bin)

c/%.bin: c/%.c c/crt0.S c/linker_c.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32i -nostdlib -nostartfiles -Wl,-T,c/linker_c.ld -O1 -o c/$*.elf c/crt0.S $<
	${PREFIX}objcopy -j .text -j .data -O binary c/$*.elf $@
	rm -f c/$*.elf

all-tests: $(ASM_BINS) $(C_BINS)

# System-level tests (multicore, RV32I + custom AM instructions)
NUM_PU ?= 16

sys/pingpong_test.bin: asm/pingpong_test.S linker.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -DNUM_PU=${NUM_PU} -o sys/pingpong_test.elf $<
	${PREFIX}objcopy -j .text -j .data -O binary sys/pingpong_test.elf $@
	rm -f sys/pingpong_test.elf

sys/sha256_concurrent.bin: sys/sha256_concurrent.S sys/sha256_lib.c sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32i_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -DNUM_PU=${NUM_PU} -O1 -o sys/sha256_concurrent.elf sys/sha256_concurrent.S sys/sha256_lib.c
	${PREFIX}objcopy -j .text -j .data -O binary sys/sha256_concurrent.elf $@
	rm -f sys/sha256_concurrent.elf

sys/spm_init.bin: sys/spm_init.S sys/linker_system.ld
	${PREFIX}gcc -mabi=ilp32 -march=rv32i_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_system.ld -DNUM_PU=${NUM_PU} -O1 -o sys/spm_init.elf sys/spm_init.S
	${PREFIX}objcopy -j .text -j .data -O binary sys/spm_init.elf $@
	rm -f sys/spm_init.elf

sys-tests: sys/pingpong_test.bin sys/sha256_concurrent.bin sys/spm_init.bin

clean:
	rm -f test.single test.double test.single.bin test.double.bin
	rm -f asm/*.bin asm/*.elf c/*.bin c/*.elf sys/*.bin sys/*.elf

