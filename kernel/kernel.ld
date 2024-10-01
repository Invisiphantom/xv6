OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

SECTIONS
{
  /* 保证entry.S处于地址0x80000000 (qemu -kernel) */
  . = 0x80000000;

  .text : {
    *(.text .text.*)
    . = ALIGN(0x1000);

    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);

    /* 检查 trampoline 是否不超过一个页 */
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    PROVIDE(etext = .);
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */

    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */

    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */

    . = ALIGN(16);
    *(.bss .bss.*)
  }

  PROVIDE(end = .);
}
