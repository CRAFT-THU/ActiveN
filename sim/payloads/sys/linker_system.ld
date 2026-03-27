OUTPUT_ARCH("riscv")

ENTRY(_start)

MEMORY {
    ram (arwx) : ORIGIN = 0x80000000, LENGTH = 0x400000
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

  . = ALIGN(4);
  result_buffer = .;
  . = . + 2048;           /* up to 512 x 4 bytes */

  . = ALIGN(4096);
  _stack_base = .;
  /* Each core: sp = _stack_base + hartid * 4096 (grows downward) */

  /DISCARD/ : {
    *(.eh_frame .eh_frame.* .riscv.*)
  }
}
