OUTPUT_ARCH("riscv")

/* Default Linker Script
 *
 * This is the default linker script for all Freedom Metal applications.
 */

ENTRY(_start)

MEMORY {
    ram (arw!xi) : ORIGIN = 0x80000000, LENGTH = 0x4000
}

SECTIONS {
  . = 0x80000000;

  .text : {
    *(.text .text.*)
  } >ram

  .data : {
    *(.data .data.*)
    *(.rodata .rodata.*)
    *(.sdata .sdata.*)
  } >ram

  .bss : {
    *(.bss .bss.*)
    *(.sbss .sbss.*)
  } >ram

  /DISCARD/ : {
    *(.eh_frame .eh_frame.* .riscv.*)
  }
}
