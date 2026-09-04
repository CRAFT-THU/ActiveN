OUTPUT_ARCH("riscv")

ENTRY(_start)

MEMORY {
    /* DRAM bytes [0, 16) are the datagen boot header. */
    ram (arwx) : ORIGIN = 0x80000010, LENGTH = 0x3ffff0
}

SECTIONS {
  . = 0x80000010;

  .text : {
    *(.text .text.*)
  } >ram

  .data : {
    *(.data .data.*)
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    *(.sdata .sdata.*)
  } >ram

  .bss : {
    *(.bss .bss.*)
    *(.sbss .sbss.*)
  } >ram

  . = ALIGN(4);
  result_buffer = .;
  . = . + 2048;

  . = ALIGN(4096);
  _stack_base = .;

  /DISCARD/ : {
    *(.eh_frame .eh_frame.* .riscv.*)
  }

  ASSERT(SIZEOF(.text) <= 8192, "MNIST text exceeds the 8 KiB I-cache")
}
