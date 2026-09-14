OUTPUT_ARCH("riscv")
ENTRY(_start)
MEMORY { ram (arwx) : ORIGIN = 0x80000010, LENGTH = 0x3ffff0 }
SECTIONS {
 .text ORIGIN(ram) : { *(.text.boot) *(.text .text.*) } >ram
 .data : { *(.rodata .rodata.* .srodata .srodata.* .data .data.* .sdata .sdata.*) } >ram
 .bss : { *(.bss .bss.* .sbss .sbss.*) } >ram
 /DISCARD/ : { *(.eh_frame .eh_frame.* .riscv.*) }
 ASSERT(_start == ORIGIN(ram), "Brunel entry moved")
 ASSERT(SIZEOF(.bss)==0,"Brunel mutable globals must live in per-PU SPM")
 ASSERT(ADDR(.data)+SIZEOF(.data)<=0x80002000,"Brunel payload exceeds preloaded 8 KiB ICache")
}
