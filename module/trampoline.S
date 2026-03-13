#include <linux/linkage.h>
#include <asm/assembler.h>

  .text

SYM_FUNC_START(parasyte_trampoline)
  adrp    x4, parasyte_hb
  ldr     x4, [x4, :lo12:parasyte_hb]

  adrp    x0, parasyte_fdt
  ldr     x0, [x0, :lo12:parasyte_fdt]

  // Barrier before jumping
  dsb     sy
  isb

  // Jump to payload entry (physical address)
  adrp    x5, parasyte_kimage
  ldr     x5, [x5, :lo12:parasyte_kimage]
  br      x5
SYM_FUNC_END(parasyte_trampoline)

  .data

  .balign 8
SYM_DATA(parasyte_start, .quad 0x0)
  .balign 8
SYM_DATA(parasyte_hb, .quad 0x0)
  .balign 8
SYM_DATA(parasyte_fdt, .quad 0x0)
  .balign 8
SYM_DATA(parasyte_kimage, .quad 0x0)
  .balign 8
SYM_DATA(parasyte_end, .quad 0x0)
