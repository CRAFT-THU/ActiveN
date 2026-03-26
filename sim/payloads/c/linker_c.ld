OUTPUT_ARCH("riscv")

ENTRY(_start)

MEMORY {
    ram (arwx) : ORIGIN = 0x80000000, LENGTH = 0x100000
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

  _stack_top = ORIGIN(ram) + LENGTH(ram);

  /DISCARD/ : {
    *(.eh_frame .eh_frame.* .riscv.*)
  }
}
