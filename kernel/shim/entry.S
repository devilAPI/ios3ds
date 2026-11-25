@ ios3ds kernel shim: ARMv6 code the patched 4.2.1 kernel branches into.
@ Linked at VA 0x80840000 (PA 0x20840000, below topOfKernelData, so XNU maps
@ it - RWX, physmap - and never reuses it). See kernel/shim/shim.h for the
@ memory layout shared with the ARM9 (mailbox, bounce buffer, log ring).
@
@ Entry points sit at fixed offsets (kernel/patches.json: @shim+0xNN@):
@   +0x00  b shim_putc            serial "transmit data"     (r0 = char)
@   +0x10  b shim_get_abstime     cpu_data timer hook +0x70  (r3 = cpu_data)
@   +0x14  b shim_get_decrementer cpu_data timer hook +0x64  (r3 = cpu_data)
@   +0x18  b shim_set_decrementer cpu_data timer hook +0x68  (r0 = ticks, r3 = cpu_data)
@   +0x1C  b shim_irq             kernel IRQ handler entry (IRQ mode)
@   +0x20  b shim_mdevstrategy    bsd/dev/memdev.c mdevstrategy(buf_t) -> SD via ARM9
@   +0x24  b shim_iic_transfer    AppleARMIICDevice transfer -> register file (audio codec/amp)
@   +0x28  b shim_quiet_clk_function  AppleS5L8720XClockController PMU wait, off the busy count (quiet.c)
@   +0x2C  b shim_gyro_dispatch   AppleLIS331DLH vtable +0x3c0 -> gyro events (tools/ios3ds-mcu/shim/mcu.c)
@   +0x30..+0x3C                  MCU: RTC, IORTC wait, halt, set time (tools/ios3ds-mcu/shim/mcu.c)
@
@ Time base: MPCore watchdog in timer mode = free-running 32-bit down-counter,
@ extended to 64 bits here. Deadlines: MPCore private timer, one-shot, GIC
@ IRQ 29. Both run at PERIPHCLK/(PRESCALE+1) = 134055928/128 = 1047311 Hz,
@ which is what kernel/dtgen.py puts in timebase-frequency.
@ The kernel takes its timer interrupt as an FIQ (handler address in r9_fiq);
@ the GIC can only raise IRQ, so shim_irq re-enters the kernel's FIQ handler
@ as if a real FIQ had been taken.
@
@ SPDX-License-Identifier: GPL-3.0-or-later

#include "kpi.h"

    .arch armv6k
    .arm
    .section .text, "ax"

    .equ SHIM_PANIC_REQ, 0x6500         @ boot/common/ios3ds.h: SELECT+START request, written by the ARM9 only
    .equ SHIM_SIZE,     0x10000         @ ring header follows the shim region
    .equ RING_SIZE_OFF, 4
    .equ RING_HEAD_OFF, 8
    .equ RING_BUF_OFF,  16

    .equ PHYS_BASE,     0x20000000
    .equ VIRT_BASE,     IOS3DS_KPI_VIRT_BASE
    .equ PRIV_PA,       0x17E00000      @ MPCore private region (SCU/GIC/timers)
    .equ PRIV_VA,       0xBFF00000      @ 1 MiB section we add to the kernel L1
    .equ PRIV_SECTION,  (PRIV_PA | (1 << 10) | (1 << 4) | (1 << 2) | 2)  @ AP=01 XN B (device), section

    .equ GICC_CTRL, 0x100
    .equ GICC_PMR,  0x104
    .equ GICC_IAR,  0x10C
    .equ GICC_EOIR, 0x110
    .equ GICD_CTRL, 0x1000
    .equ GICD_ISENABLER0, 0x1100
    .equ GICD_IPRIORITY,  0x1400
    .equ PT_LOAD,   0x600
    .equ PT_COUNT,  0x604
    .equ PT_CTRL,   0x608
    .equ PT_ISR,    0x60C
    .equ WD_LOAD,   0x620
    .equ WD_COUNT,  0x624
    .equ WD_CTRL,   0x628
    .equ WD_DISABLE,0x634

    .equ PRESCALE,  127             @ default; New 3DS at 804 MHz: cfg_n3ds (191, shift 1)
    .equ PT_CTRL_RUN, ((PRESCALE << 8) | 4 | 1)     @ IT enable + enable, one-shot
    .equ WD_CTRL_RUN, ((PRESCALE << 8) | 2 | 1)     @ auto-reload + enable, timer mode
    .equ N3RES_L2,  (1 << 18)
    .equ L2C_VA,    (PRIV_VA + 0x10000) @ L2C-310 (New 3DS) at PA 0x17E10000, same section
    .equ IRQ_PTIMER, 29
    .equ IRQ_PDC0,   42                             @ top screen VBlank
    .equ PDC_PA,     0x10400000                     @ GPU/PDC block (1 MiB section)
    .equ PDC_VA,     0xBFE00000
    .equ PDC0,       0x400
    .equ AXI_PA,     0x1FF00000                     @ DSP RAM + AXI WRAM (fake S5L register pages)
    .equ AXI_VA,     0xBFD00000
    .equ KERNEL_IRQ_CONT, IOS3DS_KPI_KERNEL_IRQ_CONT @ kernel IRQ handler after the 2 patched insns

    .global _shim_base
_shim_base:
    b shim_putc                     @ +0x00
    .word 0x4D494853                @ +0x04 'SHIM'
    .word 2                         @ +0x08 version
cfg_n3ds:   .word 0                 @ +0x0C New 3DS word from the ARM9 (ios3ds.h N3RES_*): timer
    .global cfg_n3ds                    @ prescaler (0 = 127), tick shift, L2 on. docs/n3ds.md
    b shim_get_abstime              @ +0x10
    b shim_get_decrementer          @ +0x14
    b shim_set_decrementer          @ +0x18
    b shim_irq                      @ +0x1C
    b shim_mdevstrategy             @ +0x20 (C, blk.c)
    b shim_iic_transfer             @ +0x24 (C, kernel/shim/audio.c)
    b shim_quiet_clk_function       @ +0x28 clock controller's PMU wait (C, kernel/shim/quiet.c)
    b shim_gyro_dispatch            @ +0x2C gyro service's dispatchAccelerometerEvent (vtable slot, tools/ios3ds-mcu/shim/mcu.c)
    b shim_mcu_get_gmt              @ +0x30 PEGetGMTTimeOfDay (C, tools/ios3ds-mcu/shim/mcu.c)
    b shim_mcu_wait_rtc             @ +0x34 IOKitInitializeTime's IORTC wait
    b shim_mcu_halt_restart         @ +0x38 PEHaltRestart's platform call
    b shim_mcu_set_gmt              @ +0x3C PESetGMTTimeOfDay

@ ---------------------------------------------------------------------------
@ statistics / state (read by the ARM9 via physical address = shim_pa + off)
    .balign 32
st_inited:      .word 0             @ +0x40 
st_ticks_hi:    .word 0             @ +0x44 high word of the 64-bit counter
st_last_up:     .word 0             @ +0x48 
st_timer_irqs:  .word 0             @ +0x4C 
st_other_irqs:  .word 0             @ +0x50 
st_last_irq:    .word 0             @ +0x54 
st_deferred:    .word 0             @ +0x58 
st_l1_old:      .word 0             @ +0x5C L1 entry found at PRIV_VA before we took it
irq_save0:      .word 0             @ +0x60 
irq_save1:      .word 0             @ +0x64 
irq_save2:      .word 0             @ +0x68 
st_panic_req:   .word 0             @ +0x6C unused (the SELECT+START request moved to +0x6500)
cfg_vic0_pa:    .word 0             @ +0x70 fake VIC0 page PA (patched in by mkboot)
cfg_clcd_pa:    .word 0             @ +0x74 fake CLCD page PA (patched in by mkboot)
    .global cfg_clcd_pa                 @ (kernel/shim/pica.c: the GPU mirror reads the CLCD page too)
st_vblanks:     .word 0             @ +0x78
st_injected:    .word 0             @ +0x7C
@ hardware bring-up probes, one cache line, cleaned to RAM on every update so
@ the ARM9 monitor can show them live (nothing else may move +0x00..+0x9F)
st_abs_calls:   .word 0             @ +0x80 get_abstime calls after init
st_abs_raw:     .word 0             @ +0x84 last watchdog up-count
st_abs_cpsr:    .word 0             @ +0x88 CPSR at that read (I bit 7 = IRQs masked)
st_dec_calls:   .word 0             @ +0x8C set_decrementer calls
st_dec_last:    .word 0             @ +0x90 last deadline (ticks)
st_dec_lr:      .word 0             @ +0x94 caller of the last set_decrementer
st_irq_n:       .word 0             @ +0x98 IRQ vector entries (any ID)
st_pc_idx:      .word 0             @ +0x9C IRQ PC samples taken (ring at shim+0x5F80, 16 x {pc, spsr})

@ ---------------------------------------------------------------------------
@ void shim_putc(int c): append to the log ring; clean the touched lines so
@ the ARM9 (reads DRAM, uncached) sees them. Clobbers r0-r3, r12.
shim_putc:
    adr r1, _shim_base
    add r1, r1, #SHIM_SIZE          @ r1 = ring header (VA)
    ldr r2, [r1, #RING_HEAD_OFF]
    ldr r3, [r1, #RING_SIZE_OFF]
    cmp r3, #0
    bxeq lr
    sub r3, r3, #1
    and r3, r2, r3
    add r3, r3, r1
    add r3, r3, #RING_BUF_OFF
    strb r0, [r3]
    bic r3, r3, #31
    mcr p15, 0, r3, c7, c10, 1
    add r2, r2, #1
    str r2, [r1, #RING_HEAD_OFF]
    add r3, r1, #RING_HEAD_OFF
    bic r3, r3, #31
    mcr p15, 0, r3, c7, c10, 1
    mov r3, #0
    mcr p15, 0, r3, c7, c10, 4
    bx lr

@ ---------------------------------------------------------------------------
@ shim_sb_evaluate: hook on the Seatbelt sandbox's sb_evaluate (0x803ced98).
@ The sandbox patch (kernel/patches-sandbox.json, opt-in) replaces sb_evaluate's
@ first instruction with a branch here, resolved from this symbol at build time
@ (mkboot reads it from the shim ELF; the patch only ever applies to a freshly
@ built shim, so no fixed jump-table slot is needed). r2 = operation index. For
@ the file-read operations (file*, file-read* and children, file-search) it
@ returns 0 (allow), so a sandboxed app (MobileSafari, App Store apps) can read
@ the Cydia Substrate / TLSFix dylibs under /Library; every other operation
@ (write, exec, network, iokit, mach, ...) runs the real evaluator and stays
@ enforced. The "enforce" path replays the overwritten push and jumps to the
@ second instruction. docs/cydia.md.
    .global shim_sb_evaluate
shim_sb_evaluate:
    cmp r2, #1                      @ file*
    beq 1f
    cmp r2, #11                     @ file-search
    beq 1f
    cmp r2, #6                      @ file-read* .. file-read-xattr (6..9)
    blo 2f
    cmp r2, #9
    bls 1f
2:  push {r4, r5, r6, r7, lr}       @ sb_evaluate's overwritten first insn
    ldr pc, =0x803ced9c             @ continue in sb_evaluate's body
1:  mov r0, #0                      @ allow
    bx lr
    .ltorg                          @ =0x803ced9c literal

@ ---------------------------------------------------------------------------
@ shim_init: map the MPCore private region at PRIV_VA, start the free-running
@ watchdog counter, enable the GIC for the private timer. Idempotent.
@ Clobbers r0-r2, r12. Must be called with interrupts in any state.
shim_init:
    ldr r0, st_inited
    cmp r0, #0
    bxne lr
    mrs r12, cpsr
    cpsid if
    @ L1 table VA = (TTBR1 & ~0x3FFF) - PHYS_BASE + VIRT_BASE
    mrc p15, 0, r0, c2, c0, 1
    bic r0, r0, #0xFF
    bic r0, r0, #0x3F00
    sub r0, r0, #PHYS_BASE
    add r0, r0, #VIRT_BASE
    ldr r1, =((PRIV_VA >> 20) << 2)
    add r0, r0, r1
    ldr r1, [r0]
    str r1, st_l1_old
    ldr r1, =PRIV_SECTION
    str r1, [r0]
    mcr p15, 0, r0, c7, c10, 1      @ clean the L1 entry (walks don't snoop L1)
    mov r1, #0
    mcr p15, 0, r1, c7, c10, 4      @ DSB
    ldr r0, =PRIV_VA
    mcr p15, 0, r0, c8, c7, 1       @ TLB invalidate by MVA
    mcr p15, 0, r1, c7, c10, 4
    mcr p15, 0, r1, c7, c5, 4       @ prefetch flush

    @ two more 1 MiB device sections: PDC (VBlank) and AXI WRAM (fake regs)
    mrc p15, 0, r2, c2, c0, 1
    bic r2, r2, #0xFF
    bic r2, r2, #0x3F00
    sub r2, r2, #PHYS_BASE
    add r2, r2, #VIRT_BASE
    ldr r1, =((PDC_VA >> 20) << 2)
    add r1, r1, r2
    ldr r0, =(PDC_PA | (1 << 10) | (1 << 4) | (1 << 2) | 2)
    str r0, [r1]
    mcr p15, 0, r1, c7, c10, 1
    ldr r1, =((AXI_VA >> 20) << 2)
    add r1, r1, r2
    ldr r0, =(AXI_PA | (1 << 10) | (1 << 4) | (1 << 2) | 2)
    str r0, [r1]
    mcr p15, 0, r1, c7, c10, 1
    mov r1, #0
    mcr p15, 0, r1, c7, c10, 4
    ldr r0, =PDC_VA
    mcr p15, 0, r0, c8, c7, 1
    ldr r0, =AXI_VA
    mcr p15, 0, r0, c8, c7, 1
    mcr p15, 0, r1, c7, c10, 4
    mcr p15, 0, r1, c7, c5, 4
    @ PDC0: VBlank IRQ on (H and error IRQs off), ack anything pending
    ldr r2, =(PDC_VA + PDC0)
    ldr r1, [r2, #0x74]
    bic r1, r1, #(1 << 9)
    orr r1, r1, #((1 << 8) | (1 << 10))
    str r1, [r2, #0x74]
    ldr r1, [r2, #0x78]
    and r1, r1, #1
    orr r1, r1, #(7 << 16)
    str r1, [r2, #0x78]
    ldr r0, =PRIV_VA

    @ watchdog -> timer mode, free running
    ldr r1, =0x12345678
    str r1, [r0, #WD_DISABLE]
    ldr r1, =0x87654321
    str r1, [r0, #WD_DISABLE]
    mvn r1, #0
    str r1, [r0, #WD_LOAD]
    ldr r1, cfg_n3ds                @ prescaler from the ARM9 (New 3DS 804 MHz), 0 = default
    ands r1, r1, #0xFF
    moveq r1, #PRESCALE
    mov r1, r1, lsl #8
    orr r2, r1, #(4 | 1)
    str r2, shim_pt_ctrl_run        @ private timer: IT enable + enable, one-shot
    orr r1, r1, #(2 | 1)
    str r1, [r0, #WD_CTRL]          @ watchdog: auto-reload + enable, timer mode
    @ private timer stopped, status clear
    mov r1, #0
    str r1, [r0, #PT_CTRL]
    mov r1, #1
    str r1, [r0, #PT_ISR]
    @ GIC: priority 0 for IRQ 29, enable it, enable distributor + CPU interface
    add r2, r0, #GICD_CTRL           @ r2 = distributor base (offsets > 4 KiB)
    @ real hardware: whatever boot ROM / Luma left enabled or pending must not
    @ reach us (an enabled, still-asserted level IRQ = endless IRQ storm and a
    @ silent hang; QEMU starts with everything off). Disable + unpend all
    @ PPIs/SPIs (SGIs stay), then enable exactly ours below.
    mvn r1, #0
    str r1, [r2, #0x180]            @ ICENABLER0..7
    str r1, [r2, #0x184]
    str r1, [r2, #0x188]
    str r1, [r2, #0x18C]
    str r1, [r2, #0x190]
    str r1, [r2, #0x194]
    str r1, [r2, #0x198]
    str r1, [r2, #0x19C]
    str r1, [r2, #0x280]            @ ICPENDR0..7
    str r1, [r2, #0x284]
    str r1, [r2, #0x288]
    str r1, [r2, #0x28C]
    str r1, [r2, #0x290]
    str r1, [r2, #0x294]
    str r1, [r2, #0x298]
    str r1, [r2, #0x29C]
    mov r1, #0
    strb r1, [r2, #(GICD_IPRIORITY - GICD_CTRL + IRQ_PTIMER)]
    mov r1, #(1 << IRQ_PTIMER)
    str r1, [r2, #(GICD_ISENABLER0 - GICD_CTRL)]
    mov r1, #0
    strb r1, [r2, #(GICD_IPRIORITY - GICD_CTRL + IRQ_PDC0)]
    mov r1, #1                                      @ SPI 42 -> CPU0
    add r3, r2, #0x800
    strb r1, [r3, #IRQ_PDC0]
    mov r1, #(1 << (IRQ_PDC0 - 32))
    str r1, [r2, #(GICD_ISENABLER0 - GICD_CTRL + 4)]
    ldr r1, [r2]
    orr r1, r1, #1
    str r1, [r2]
    mov r1, #0xF0
    str r1, [r0, #GICC_PMR]
    mov r1, #1
    str r1, [r0, #GICC_CTRL]

    mov r1, #1
    str r1, st_inited
    msr cpsr_c, r12
    bx lr

@ ---------------------------------------------------------------------------
@ u64 get_abstime(cpu_data *r3) = ticks + cpu_data->abstime_offset(+0x50/+0x54)
@ STACKLESS: the kernel's fast mach_absolute_time path (SWI handler, ~0x80066290)
@ calls this hook with sp pointing at nothing usable. Only r0-r2, r12.
@
@ The clock must run from the first read: rtclock_init (0x80069936) stores
@ mach_absolute_time() as the tick base (rtclock+0x30) before the first
@ set_decrementer, and the calendar conversion (0x8006916c) spins with IRQs
@ masked while that base is 0, waiting for a tick that cannot arrive. Returning
@ 0 here before init hung real hardware right after "iBoot version" (plain
@ QEMU hid it: its timer fired before the spin). So init lazily, stacklessly:
@ shim_init clobbers r0-r3, r12, and bl clobbers lr; both are parked in
@ pc-relative words with IRQ/FIQ masked.
shim_get_abstime:
    ldr r2, st_inited
    cmp r2, #0
    bne 1f
    mrs r12, cpsr
    cpsid if
    str r12, abs_init_cpsr
    str r3, abs_init_r3
    str lr, abs_init_lr
    bl shim_init
    ldr lr, abs_init_lr
    ldr r3, abs_init_r3
    ldr r12, abs_init_cpsr
    msr cpsr_c, r12
1:
    @ bring-up snapshot (shim+0x5F00, 2 lines): timer + GIC state at every
    @ clock read, so a hang shows whether the tick is pending/delivered
    ldr r2, =PRIV_VA
    ldr r1, =(_shim_base + 0x5F00)
    ldr r0, [r2, #PT_COUNT]
    str r0, [r1, #0x00]
    ldr r0, [r2, #PT_CTRL]
    str r0, [r1, #0x04]
    ldr r0, [r2, #PT_ISR]
    str r0, [r1, #0x08]
    ldr r0, [r2, #GICC_CTRL]
    str r0, [r1, #0x0C]
    ldr r0, [r2, #GICC_PMR]
    str r0, [r1, #0x10]
    ldr r0, [r2, #0x118]            @ highest pending (does not acknowledge)
    str r0, [r1, #0x14]
    add r2, r2, #0x1000             @ distributor
    ldr r0, [r2]
    str r0, [r1, #0x18]
    ldr r0, [r2, #0x100]            @ enabled 0-31
    str r0, [r1, #0x1C]
    ldr r0, [r2, #0x200]            @ pending 0-31
    str r0, [r1, #0x20]
    ldr r0, [r2, #0x204]            @ pending 32-63
    str r0, [r1, #0x24]
    ldr r0, [r2, #0x104]            @ enabled 32-63
    str r0, [r1, #0x28]
    ldr r0, [r2, #0x300]            @ active 0-31
    str r0, [r1, #0x2C]
    mrc p15, 0, r0, c1, c0, 0
    str r0, [r1, #0x30]
    @ MMU view of the IRQ vector (lines 2-4, +0x34..+0x7F): is the vector
    @ page at 0xFFFF0000 mapped, and what does the ARM11 see there? The ARM9
    @ walks the same tables in FCRAM after a hang and compares (monitor).
    mrc p15, 0, r0, c2, c0, 0
    str r0, [r1, #0x34]             @ TTBR0
    mrc p15, 0, r0, c2, c0, 1
    str r0, [r1, #0x38]             @ TTBR1
    mrc p15, 0, r0, c2, c0, 2
    str r0, [r1, #0x3C]             @ TTBCR
    mrc p15, 0, r0, c3, c0, 0
    str r0, [r1, #0x60]             @ DACR
    mrc p15, 0, r0, c0, c0, 5
    str r0, [r1, #0x64]             @ MPIDR
    mov r0, #0
    str r0, [r1, #0x7C]             @ walk stage reached (0 = nothing)
    mrc p15, 0, r0, c2, c0, 1       @ L1 VA = (TTBR1 & ~0x3FFF) + VIRT - PHYS
    bic r0, r0, #0xFF
    bic r0, r0, #0x3F00
    add r0, r0, #(VIRT_BASE - PHYS_BASE)
    ldr r2, =(0xFFF << 2)
    ldr r0, [r0, r2]                @ L1[0xFFF]
    str r0, [r1, #0x68]
    and r2, r0, #3
    cmp r2, #1                      @ coarse page table?
    bne 9f
    mov r2, #1
    str r2, [r1, #0x7C]
    bic r0, r0, #0xFF
    bic r0, r0, #0x300              @ L2 table PA
    sub r2, r0, #PHYS_BASE
    cmp r2, #0x08000000             @ inside FCRAM (linear map)?
    bhs 9f
    add r0, r0, #(VIRT_BASE - PHYS_BASE)
    ldr r0, [r0, #(0xF0 << 2)]      @ L2[0xF0] = 0xFFFF0000
    str r0, [r1, #0x6C]
    tst r0, #2                      @ extended small page?
    beq 9f
    mov r2, #2
    str r2, [r1, #0x7C]
    mvn r0, #0
    bic r0, r0, #0xFF00
    bic r0, r0, #0xFF               @ r0 = 0xFFFF0000: the vector page, ARM11 view
    ldr r2, [r0, #0x00]
    str r2, [r1, #0x40]
    ldr r2, [r0, #0x04]
    str r2, [r1, #0x44]
    ldr r2, [r0, #0x08]
    str r2, [r1, #0x48]
    ldr r2, [r0, #0x0C]
    str r2, [r1, #0x4C]
    ldr r2, [r0, #0x10]
    str r2, [r1, #0x50]
    ldr r2, [r0, #0x14]
    str r2, [r1, #0x54]
    ldr r2, [r0, #0x1C]
    str r2, [r1, #0x5C]
    ldr r2, [r0, #0x18]             @ IRQ vector
    str r2, [r1, #0x58]
    mov r12, r2, lsr #12
    ldr r0, =0xE59FF                @ ldr pc, [pc, #+imm12]?
    cmp r12, r0
    bne 9f
    mov r2, r2, lsl #20
    mov r2, r2, lsr #20             @ imm12
    mvn r0, #0
    bic r0, r0, #0xFF00
    bic r0, r0, #0xDF               @ r0 = 0xFFFF0020 = IRQ vector + 8 (pc)
    ldr r2, [r0, r2]                @ the literal = IRQ handler address
    str r2, [r1, #0x70]
    mov r0, #3
    str r0, [r1, #0x7C]
    sub r0, r2, #VIRT_BASE
    cmp r0, #0x08000000             @ in the kernel's linear map?
    bhs 9f
    ldr r0, [r2]                    @ first two words of the handler (patched
    str r0, [r1, #0x74]             @ to ldr pc,[pc,#-4] + shim_irq)
    ldr r0, [r2, #4]
    str r0, [r1, #0x78]
    mov r0, #4
    str r0, [r1, #0x7C]
9:
    mcr p15, 0, r1, c7, c10, 1
    add r1, r1, #32
    mcr p15, 0, r1, c7, c10, 1
    add r1, r1, #32
    mcr p15, 0, r1, c7, c10, 1
    add r1, r1, #32
    mcr p15, 0, r1, c7, c10, 1
    mrs r12, cpsr
    cpsid if
    ldr r2, =PRIV_VA
    ldr r0, [r2, #WD_COUNT]
    mvn r0, r0                      @ count up
    ldr r2, st_last_up
    ldr r1, st_ticks_hi
    cmp r0, r2
    addlo r1, r1, #1                @ wrapped since the last read
    strlo r1, st_ticks_hi
    str r0, st_last_up
    str r0, st_abs_raw
    str r12, st_abs_cpsr
    ldr r2, st_abs_calls
    add r2, r2, #1
    str r2, st_abs_calls
    ldr r2, =st_abs_calls
    mcr p15, 0, r2, c7, c10, 1      @ clean the probe line
    msr cpsr_c, r12
    ldr r12, cfg_n3ds               @ New 3DS 804 MHz: the counter runs at 2x the timebase
    ands r12, r12, #0xF00
    beq 2f
1:  movs r1, r1, lsr #1             @ (r1:r0) >>= 1
    mov r0, r0, rrx
    subs r12, r12, #0x100
    bne 1b
    adds r0, r0, #1                 @ never 0: rtclock_init's tick base must not be 0 (the
    adc r1, r1, #0                  @ first read comes 1 raw tick after the start, >> 1 = 0)
2:  ldr r2, [r3, #0x50]
    adds r0, r0, r2
    ldr r2, [r3, #0x54]
    adc r1, r1, r2
    bx lr

abs_init_cpsr:  .word 0             @ lazy-init scratch (shim_get_abstime)
abs_init_r3:    .word 0
abs_init_lr:    .word 0

@ u32 get_decrementer(cpu_data *r3): ticks left until the deadline (stackless)
shim_get_decrementer:
    ldr r2, st_inited
    cmp r2, #0
    moveq r0, #0
    bxeq lr
    ldr r2, =PRIV_VA
    ldr r0, [r2, #PT_COUNT]
    ldr r2, cfg_n3ds
    and r2, r2, #0xF00
    mov r2, r2, lsr #8
    mov r0, r0, lsr r2              @ timer ticks -> timebase ticks
    bx lr

@ void set_decrementer(u32 ticks /* r0 */, cpu_data *r3) - normal kernel context
shim_set_decrementer:
    @ SELECT+START: the ARM9 writes 1 to its own line at shim+0x6500 (it
    @ was +0x6C, in the line of irq_save0-2 and st_vblanks: the ARM11 kept
    @ that line dirty and its write-back put the 0 back over the ARM9's 1, so
    @ the request was lost: New 2DS XL run 1). Only the ARM9 writes this
    @ line; drop our copy (New 3DS L2 first) and read RAM.
    push {r0, r1, r3, lr}
    ldr r0, =(_shim_base + SHIM_PANIC_REQ)
    mov r1, #4
    bl shim_l2_inv_kva
    ldr r2, =(_shim_base + SHIM_PANIC_REQ)
    mcr p15, 0, r2, c7, c6, 1
    ldr r2, [r2]
    pop {r0, r1, r3, lr}
    cmp r2, #0
    bne shim_user_panic
    push {r0, r3, r4, lr}
    bl shim_init
    pop {r0, r3, r4, lr}
    str r0, [r3, #0x60]             @ cpu_data->decrementer, like the original
    str r0, st_dec_last
    str lr, st_dec_lr
    ldr r2, st_dec_calls
    add r2, r2, #1
    str r2, st_dec_calls
    ldr r2, =st_abs_calls
    mcr p15, 0, r2, c7, c10, 1
    push {r3, lr}
    bl shim_audio_setdec            @ audio: wake earlier if an emulated DMA item ends first
    pop {r3, lr}
    cmp r0, #0
    moveq r0, #1
    ldr r1, cfg_n3ds                @ timebase ticks -> timer ticks (New 3DS: << shift)
    and r1, r1, #0xF00
    mov r1, r1, lsr #8
    cmp r0, #0x40000000
    movhs r0, #0x40000000           @ no overflow (18 min at the timebase)
    mov r0, r0, lsl r1
    ldr r2, =PRIV_VA
    mov r1, #0
    str r1, [r2, #PT_CTRL]          @ stop
    mov r1, #1
    str r1, [r2, #PT_ISR]           @ clear a stale event
    str r0, [r2, #PT_LOAD]          @ load (also reloads the counter)
    ldr r1, shim_pt_ctrl_run
    str r1, [r2, #PT_CTRL]
    bx lr

@ u32 shim_now32(void): low word of the 64-bit timebase count (1047311 Hz on
@ every model), for C code (audio.c, cam.c, pica.c) instead of the raw
@ watchdog, which runs 2x faster at 804 MHz. Clobbers r0-r3, r12.
    .global shim_now32
shim_now32:
    mrs r12, cpsr
    cpsid if
    ldr r2, =PRIV_VA
    ldr r0, [r2, #WD_COUNT]
    mvn r0, r0
    ldr r2, st_last_up
    ldr r1, st_ticks_hi
    cmp r0, r2
    addlo r1, r1, #1
    strlo r1, st_ticks_hi
    str r0, st_last_up
    msr cpsr_c, r12
    ldr r2, cfg_n3ds
    ands r2, r2, #0xF00
    bxeq lr
    mov r2, r2, lsr #8
    rsb r3, r2, #32
    mov r0, r0, lsr r2
    orr r0, r0, r1, lsl r3
    bx lr

@ void shim_l2_inv(u32 pa, u32 len) / shim_l2_inv_kva(void *va, u32 len) (XNU's
@ linear map): New 3DS with n3ds_l2=1 only. The L2C-310 runs write-through
@ (boot/arm11/main.c), so it never holds dirty data, but it can hold stale
@ copies of RAM the ARM9 or the GPU wrote: drop them before reading such
@ RAM (callers do it next to their L1 invalidate). Clobbers r0-r2.
    .global shim_l2_inv, shim_l2_inv_kva
shim_l2_inv_kva:
    sub r0, r0, #(VIRT_BASE - PHYS_BASE)
shim_l2_inv:
    ldr r2, cfg_n3ds
    tst r2, #N3RES_L2
    bxeq lr
    ldr r2, =L2C_VA
    add r1, r1, r0
    bic r0, r0, #31
1:  cmp r0, r1
    strlo r0, [r2, #0x770]          @ invalidate line by PA
    addlo r0, r0, #32
    blo 1b
    mov r0, #0
    str r0, [r2, #0x730]            @ cache sync
2:  ldr r0, [r2, #0x730]
    tst r0, #1
    bne 2b
    ldr r0, st_l2_inv
    add r0, r0, #1
    str r0, st_l2_inv
    bx lr
    .global shim_pt_ctrl_run
shim_pt_ctrl_run: .word PT_CTRL_RUN
st_l2_inv:  .word 0                 @ shim_l2_inv calls that reached the L2C (debugging)
    .global st_l2_inv

shim_user_panic:
    ldr r1, =(_shim_base + SHIM_PANIC_REQ)
    mov r2, #0
    str r2, [r1]
    mcr p15, 0, r1, c7, c14, 1      @ back to RAM: the line must not stay dirty here
    mcr p15, 0, r2, c7, c10, 4
    adr r0, panic_msg
    ldr r12, =IOS3DS_KPI_PANIC + 1  @ _panic (Thumb, address from the staged kernelcache)
    blx r12
    b .
panic_msg:
    .asciz "ios3ds: panic requested from the 3DS (SELECT+START) - thread dump follows"
    .balign 4

@ ---------------------------------------------------------------------------
@ IRQ entry (patched over the kernel's IRQ handler at 0x8006648c). The kernel
@ keeps no IRQ-mode stack, so only pc-relative scratch words are used.
shim_irq:
    str r0, irq_save0
    str r1, irq_save1
    str r2, irq_save2
    @ PC sampler: where was the ARM11 when this IRQ (VBlank 60 Hz, ticks) hit?
    @ lr_irq = interrupted pc + 4 (the kernel's "sub lr, lr, #4" is patched out)
    ldr r0, st_pc_idx
    and r1, r0, #15
    add r0, r0, #1
    str r0, st_pc_idx
    ldr r2, =(_shim_base + 0x5F80)
    add r2, r2, r1, lsl #3
    sub r1, lr, #4
    str r1, [r2]
    mrs r1, spsr
    str r1, [r2, #4]
    mcr p15, 0, r2, c7, c10, 1      @ clean the sample's line for the ARM9
    ldr r0, =PRIV_VA
    ldr r1, [r0, #GICC_IAR]
    str r1, st_last_irq
    ldr r2, st_irq_n
    add r2, r2, #1
    str r2, st_irq_n
    ldr r2, =st_inited              @ make +0x40..+0x9F visible to the ARM9 now
    mcr p15, 0, r2, c7, c10, 1
    add r2, r2, #32
    mcr p15, 0, r2, c7, c10, 1
    add r2, r2, #32
    mcr p15, 0, r2, c7, c10, 1
    b shim_audio_hook               @ audio: PL080 emulation (kernel/shim/audio*); comes back
    .global shim_irq_resume, irq_save0  @ to shim_irq_resume with r0/r1 intact, or ends the IRQ itself
shim_irq_resume:
    mov r2, r1, lsl #22
    mov r2, r2, lsr #22             @ interrupt ID
    cmp r2, #IRQ_PTIMER
    bne 2f
    mov r2, #1
    str r2, [r0, #PT_ISR]           @ clear timer event
    str r1, [r0, #GICC_EOIR]
    ldr r2, st_timer_irqs
    add r2, r2, #1
    str r2, st_timer_irqs
    ldr r2, =st_inited              @ the ARM9 reads the stats from RAM: clean
    mcr p15, 0, r2, c7, c10, 1      @ +0x40..+0x7F (2 lines) so they are live
    add r2, r2, #32
    mcr p15, 0, r2, c7, c10, 1
    @ GPU display mirror (pica.c): advance a frame in flight. C needs a stack
    @ and XNU keeps none in IRQ mode: the audio glue's private one (free here).
    ldr sp, =aud_irq_stack_top
    push {r0, r1, r2, r3, r12, lr}
    mov r0, #IRQ_PTIMER
    bl shim_gpu_irq_hook
    pop {r0, r1, r2, r3, r12, lr}
    mrs r2, spsr
    tst r2, #0x40                   @ FIQs masked where we interrupted?
    bne 3f
    @ become an FIQ: lr_fiq = lr_irq, spsr_fiq = spsr_irq, then the vector's 'mov pc, r9'
    mov r0, lr
    mov r1, r2
    msr cpsr_c, #0xD1               @ FIQ mode, I+F masked
    mov lr, r0
    msr spsr_cxsf, r1
    ldr r0, irq_save0
    ldr r1, irq_save1
    ldr r2, irq_save2
    mov pc, r9

3:  @ FIQ masked: retry shortly (64 ticks ~ 61 us) instead of losing the tick
    ldr r2, st_deferred
    add r2, r2, #1
    str r2, st_deferred
    mov r2, #64
    str r2, [r0, #PT_LOAD]
    ldr r2, shim_pt_ctrl_run
    str r2, [r0, #PT_CTRL]
    b 9f

2:  cmp r2, #IRQ_PDC0
    bne 5f
    @ ---- top-screen VBlank: ack PDC + GIC, maybe inject the S5L CLCD frame IRQ
    ldr r2, =(PDC_VA + PDC0)
    ldr r0, [r2, #0x78]
    and r0, r0, #1
    orr r0, r0, #(1 << 17)
    str r0, [r2, #0x78]
    ldr r0, =PRIV_VA
    str r1, [r0, #GICC_EOIR]
    ldr r2, st_vblanks
    add r2, r2, #1
    str r2, st_vblanks
    @ GPU display mirror (pica.c): start / advance a frame (private stack, see above)
    ldr sp, =aud_irq_stack_top
    push {r0, r1, r2, r3, r12, lr}
    mov r0, #IRQ_PDC0
    bl shim_gpu_irq_hook
    pop {r0, r1, r2, r3, r12, lr}
    ldr r0, cfg_clcd_pa
    cmp r0, #0
    beq 9f
    ldr r1, =(AXI_VA - AXI_PA)
    add r0, r0, r1                  @ r0 = fake CLCD (VA)
    ldr r1, [r0, #0x08]
    tst r1, #1                      @ frame IRQ enabled by the driver?
    beq 9f
    @ Not one-shot: the enable stays set while the driver wants frames and it
    @ clears it itself when idle (AppleM2CLCD interrupt action 0x807a9df8,
    @ register shadow at ivar 0x254). Clearing it here starved vsync clients:
    @ GSHeartbeat (UIScrollView paging/bounce) never ticked once rendering
    @ stopped, so a swiped page froze where the finger lifted.
    mov r1, #1
    str r1, [r0, #0x0C]             @ frame IRQ status
    ldr r2, cfg_vic0_pa
    ldr r1, =(AXI_VA - AXI_PA)
    add r2, r2, r1                  @ r2 = fake VIC0 (VA)
    ldr r1, [r2, #0x134]            @ VICVECTADDR13 (as programmed by AppleARMPL192VIC)
    str r1, [r2, #0xF00]            @ -> VICADDRESS: "IRQ 13 pending"
    ldr r1, st_injected
    add r1, r1, #1
    str r1, st_injected
    @ enter the kernel's own IRQ path (the 2 instructions our patch displaced)
    ldr r0, irq_save0
    ldr r1, irq_save1
    ldr r2, irq_save2
    sub lr, lr, #4
.if IOS3DS_KPI_KERNEL_IRQ_SPSR
    @ 2.2.1/3.1.3 _fleh_irq reads the interrupted mode from SPSR: replaying
    @ 4.x's CPSR read made every injected CLCD frame IRQ that hit user mode
    @ look like a kernel-mode interrupt (SpringBoard died at pc 0xc00684c0)
    mrs sp, spsr
.else
    mrs sp, apsr
.endif
    ldr pc, =KERNEL_IRQ_CONT

5:  @ anything else
    ldr r0, =1023
    cmp r2, r0
    beq 9f                          @ spurious: no EOI
    ldr r0, =PRIV_VA
    str r1, [r0, #GICC_EOIR]
    @ and switch it off for good (ICENABLER[id/32] = 1 << id%32): an unknown
    @ level IRQ would otherwise fire again forever
    ldr r0, =(PRIV_VA + 0x1180)
    mov r1, r2, lsr #5
    add r0, r0, r1, lsl #2
    and r2, r2, #31
    mov r1, #1
    mov r1, r1, lsl r2
    str r1, [r0]
    ldr r2, st_other_irqs
    add r2, r2, #1
    str r2, st_other_irqs
9:  ldr r0, irq_save0
    ldr r1, irq_save1
    ldr r2, irq_save2
    subs pc, lr, #4

    .ltorg

