.PHONY: clean

default: test.single.bin test.double.bin

CORE_CNT ?= 512
PREFIX ?= riscv64-unknown-none-elf-

test.double: test.S
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -DCORE_CNT=${CORE_CNT} -DDOUBLE_DRAM -o test.double ./test.S

test.double.bin: test.double
	${PREFIX}objcopy -j .text -j .data -O binary ./test.double test.double.bin

test.single: test.S
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -DCORE_CNT=${CORE_CNT} -o test.single ./test.S

test.single.bin: test.single
	${PREFIX}objcopy -j .text -j .data -O binary ./test.single test.single.bin

clean:
	rm -f test.single test.double test.single.bin test.double.bin

