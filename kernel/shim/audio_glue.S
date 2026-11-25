@ ios3ds shim: IRQ-mode glue for the audio emulation (audio.c).
@
@ kernel/shim/entry.S shim_irq branches here right after it acknowledged the
@ GIC, with
@     r0 = PRIV_VA, r1 = GICC_IAR value, lr = lr_irq (interrupted pc + 4),
@     r0-r2 of the interrupted code in irq_save0..2.
@ XNU keeps no IRQ-mode stack (it uses sp_irq as a scratch register), so the C
@ code gets a private one. IRQs do not nest: the I bit stays set in here, and
@ nothing raises FIQs on this board.
@
@ shim_audio_irq() decides:
@     0  -> back to shim_irq (shim_irq_resume) with r0/r1 as they were
@     1  -> the interrupt was only ours: return from the exception
@     2  -> enter XNU's IRQ handler (the PL080 interrupt is pending in the fake VIC)
@
@ SPDX-License-Identifier: GPL-3.0-or-later

#include "kpi.h"

    .arch armv6k
    .arm
    .section .text, "ax"

    @ XNU's IRQ handler after the 2 instructions entry.S replaced. 4.2.1:
    @ 0x80066494 (kpi.h default). It was hard-coded here, so on 3.1.3/2.2.1
    @ (kernel at 0xC0000000) the first PL080 TC interrupt jumped into nothing.
    .equ KERNEL_IRQ_CONT, IOS3DS_KPI_KERNEL_IRQ_CONT

    .global shim_audio_hook
shim_audio_hook:
    ldr sp, =aud_irq_stack_top
    push {r0, r1, r2, r3, r12, lr}
    mov r0, r1
    bl shim_audio_irq
    mov r2, r0
    pop {r0, r1}
    add sp, sp, #4
    pop {r3, r12, lr}
    cmp r2, #1
    blo shim_irq_resume
    ldr r0, =irq_save0
    ldr r1, [r0, #4]                        @ irq_save1
    ldr r2, [r0, #8]                        @ irq_save2
    ldr r0, [r0]
    subeqs pc, lr, #4                       @ 1 (flags still from the cmp); 2 falls through
1:  @ the two instructions the irq-entry patch displaced, then XNU's handler
    sub lr, lr, #4
.if IOS3DS_KPI_KERNEL_IRQ_SPSR
    mrs sp, spsr                            @ 2.2.1/3.1.3 _fleh_irq: as entry.S
.else
    mrs sp, cpsr
.endif
    ldr pc, =KERNEL_IRQ_CONT

    .ltorg

    .section .data
    .balign 8
aud_irq_stack:
    .space 2048
    .global aud_irq_stack_top           @ entry.S: also the GPU mirror's IRQ-time stack
aud_irq_stack_top:
