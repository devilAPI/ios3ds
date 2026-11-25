/* Lazy-binding glue for executables that iOS 2.x's dyld loads.
 *
 * 5H11a's dyld predates compressed link-edit info (LC_DYLD_INFO, the
 * dyld_stub_binder export), so a tool for 2.2.1 is linked with
 * -target armv6-apple-ios2.0: ld64 then emits classic lazy stubs whose
 * helpers branch to dyld_stub_binding_helper with the lazy pointer's
 * address in ip. Apple's old crt1 supplied that helper; ours (4.x) does not.
 * dyld fills the first two words of __DATA,__dyld at load with its lazy
 * binder and _dyld_func_lookup; the binder wants
 *   [sp+4] = address of the lazy pointer, [sp] = the image's mach header.
 * SPDX-License-Identifier: GPL-3.0-or-later */
	.section __DATA,__dyld,regular,no_dead_strip
	.align	2
dyld_lazy_binder:	.long	0
dyld_func_lookup_ptr:	.long	0

	.text
	.align	2
	.arm
	.private_extern	dyld_stub_binding_helper
dyld_stub_binding_helper:
	str	ip, [sp, #-4]!		@ the lazy pointer
	ldr	ip, 1f			@ __mh_execute_header (a 2.x executable is never slid)
	str	ip, [sp, #-4]!
	ldr	ip, 2f
	ldr	pc, [ip]		@ dyld's lazy binder
1:	.long	__mh_execute_header
2:	.long	dyld_lazy_binder
