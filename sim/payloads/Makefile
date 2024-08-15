.PHONY: clean

default: test.bin

CORE_CNT ?= 512
PREFIX ?= riscv64-unknown-none-elf-

test: test.S
	${PREFIX}gcc -mabi=ilp32 -march=rv32imf_zicsr -nostdlib -nostartfiles -Wl,-T,linker.ld -DCORE_CNT=${CORE_CNT} -o test ./test.S

test.bin: test
	${PREFIX}objcopy -j .text -j .data -O binary ./test test.bin

clean:
	rm -f test test.bin

