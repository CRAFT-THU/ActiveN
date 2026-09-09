OUTPUT_ARCH("riscv")

ENTRY(_start)

MEMORY {
    /* DRAM bytes [0, 16) are the datagen boot header. */
    ram (arwx) : ORIGIN = 0x80000010, LENGTH = 0x3ffff0
}

SECTIONS {
  . = 0x80000010;

  /* Datagen places the first instruction immediately after its 16-byte header.
   * Keep that entry fixed even when an internal R_RISCV_ALIGN requests a cache
   * boundary; the RISC-V linker relaxes its padding using the final address.
   */
  .text ORIGIN(ram) : SUBALIGN(4) {
    *(.text)
    *(.text.*)
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
  ASSERT(_start == ORIGIN(ram), "MNIST entry moved past its boot header")
}
