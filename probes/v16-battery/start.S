/*
 * Redmi A5 probe V12: CPU milestone. arm64 Image header; entered with MMU off, x0 = DTB.
 * Every core (boot core and PSCI-started secondaries) drops from EL2 to EL1 here.
 * Position independent; MMU stays off (Device memory: aligned accesses only).
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start
	.long	0
	.quad	0			/* text_offset */
	.quad	_end			/* image_size */
	.quad	0xa			/* LE, 4K, anywhere */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"
	.long	0

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB */
	adr	x20, _head		/* load address */
	mrs	x21, CurrentEL
	lsr	x21, x21, #2		/* entry EL */
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	bl	drop_to_el1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	probe_main
hang:	wfe
	b	hang

/* PSCI CPU_ON entry: x0 = context id = core index (1..7). MMU off. */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x22, x0
	mrs	x21, CurrentEL
	lsr	x21, x21, #2
	adr	x1, _sec_stacks
	add	x1, x1, x22, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	bl	drop_to_el1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	mov	x0, x22
	mov	x1, x21
	bl	secondary_main
	b	hang

/*
 * If at EL2: make EL1 AArch64 with nothing trapped, give EL1 the timers, the GIC
 * system-register interface and its real MIDR/MPIDR, then eret to the caller at EL1h
 * on the same stack. At EL1 already: plain return. Clobbers x0-x2.
 */
drop_to_el1:
	mrs	x0, CurrentEL
	cmp	x0, #8
	b.ne	9f
	adr	x1, vectors
	msr	vbar_el2, x1		/* report anything that faults at EL2 */
	mov	x0, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64; IMO/FMO = 0 -> IRQs go to EL1 */
	msr	hcr_el2, x0
	mov	x0, #3			/* EL1 physical counter + timer access */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mov	x0, #0x33ff		/* no FP/SIMD traps */
	msr	cptr_el2, x0
	msr	vttbr_el2, xzr
	mrs	x0, midr_el1
	msr	vpidr_el2, x0
	mrs	x0, mpidr_el1
	msr	vmpidr_el2, x0
	mrs	x0, icc_sre_el2
	orr	x0, x0, #0xf		/* SRE, DFB, DIB, Enable (EL1 may use ICC_*_EL1) */
	msr	icc_sre_el2, x0
	isb
	msr	ich_hcr_el2, xzr
	movz	x0, #0x0800
	movk	x0, #0x30d0, lsl #16	/* SCTLR_EL1 RES1 bits, MMU/caches off */
	msr	sctlr_el1, x0
	mov	x0, sp
	msr	sp_el1, x0
	mov	x0, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x0
	msr	elr_el2, x30
	isb
	eret
9:	ret

/* ---- exception vectors (used at EL1, and at EL2 before the drop) ---- */
	.macro	vreport n
	.balign	0x80
	mov	x0, #\n
	b	exc_common
	.endm

	.balign	0x800
vectors:
	vreport	0
	vreport	1
	vreport	2
	vreport	3
	vreport	4
	.balign	0x80		/* slot 5: IRQ, current EL, SPx */
	b	irq_entry
	vreport	6
	vreport	7
	vreport	8
	vreport	9
	vreport	10
	vreport	11
	vreport	12
	vreport	13
	vreport	14
	vreport	15

irq_entry:
	sub	sp, sp, #176
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x29, [sp, #144]
	str	x30, [sp, #160]
	bl	irq_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x29, [sp, #144]
	ldr	x30, [sp, #160]
	add	sp, sp, #176
	eret

exc_common:
	bl	exc_report
8:	wfe
	b	8b
