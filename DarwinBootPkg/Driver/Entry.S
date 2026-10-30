// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>

ASM_FUNC(DarwinCurrentEl)
  mrs x0, CurrentEL
  ret

// x0=boot_args PA, x1=entry PA, x2=owned PA, x3=owned size, x4=frame buffer
// PA (0 for none), x5=its size in bytes. EL1 only.
ASM_FUNC(DarwinEnter)
  msr daifset, #15
  // Breadcrumb: a green band (BGRA 0xFF00FF00) along the physical RIGHT edge
  // (= visually the top edge when the panel is held landscape, matching the
  // kernel-side blue/magenta probes' rotated orientation). DBY-W09 panel is
  // physically 1600x2560 with a 6400-byte row stride; edge block is 160 px
  // wide, i.e. row offset (1600-160)*4 = 5760.
  mov w10, #0xFF00
  movk w10, #0xFF00, lsl #16
  add x4, x4, #0x1000         // right-edge offset 5760 = 0x1680 (split: add
  add x4, x4, #0x600          //  immediates cannot exceed 4095)
  add x4, x4, #0x80
  mov x9, #2560               // physical rows
1:
  mov x11, x4                 // row start
  mov x12, #80                // 160 px * 4 B / 8 B per stp
3:
  stp w10, w10, [x11], #8
  subs x12, x12, #1
  b.ne 3b
  add x4, x4, #0x1000         // next physical row: 6400 = 0x1900 (add imm
  add x4, x4, #0x900          //  cannot encode 6400 directly)
  subs x9, x9, #1
  b.ne 1b
  dsb sy
2:
  // EBS notify group was skipped, so TimerDxe never disabled its timer:
  // its level-triggered interrupt would storm the early kernel (EOI at the
  // GIC does not stop it). Disable the physical timer; XNU reprograms it.
  msr cntp_ctl_el0, xzr
  isb
  add x5, x2, x3
  mrs x6, ctr_el0
  ubfx x6, x6, #16, #4
  mov x7, #4
  lsl x6, x7, x6
  sub x7, x6, #1
  bic x4, x2, x7
  dsb sy
1:
  dc civac, x4
  add x4, x4, x6
  cmp x4, x5
  b.lo 1b
  dsb sy
  ic iallu
  dsb sy
  isb
  mrs x4, sctlr_el1
  bic x4, x4, #1
  bic x4, x4, #4
  bic x4, x4, #0x1000
  msr sctlr_el1, x4
  isb
  tlbi vmalle1
  dsb sy
  isb
  br x1
