/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Minimal, non-returning BLDR jump test for the TP-Link Archer XR500v.
 *
 * This payload assumes that Bootbase/BLDR has already configured the UART at
 * 115200 baud.  It disables CPU interrupts, prints one fixed marker through
 * the uncached UART window and then loops forever.  It does not access flash,
 * Ethernet, GPIO, PON or any persistent storage.
 */

	.set	noreorder
	.set	noat

	.section .text.start, "ax", @progbits
	.globl	_start
	.ent	_start
_start:
	/* Do not inherit an enabled interrupt while no vector table is installed. */
	mfc0	$t0, $12
	li	$t1, -2
	and	$t0, $t0, $t1
	mtc0	$t0, $12
	ehb

	/* EN751221 UART, KSEG1 uncached alias of physical 0x1fbf0000. */
	lui	$s0, 0xbfbf
	la	$s1, marker

next_byte:
	lbu	$t0, 0($s1)
	beqz	$t0, halt
	 nop

wait_thre:
	lbu	$t1, 0x17($s0)
	andi	$t1, $t1, 0x20
	beqz	$t1, wait_thre
	 nop

	sb	$t0, 0x03($s0)
	addiu	$s1, $s1, 1
	b	next_byte
	 nop

halt:
	b	halt
	 nop
	.end	_start

	.section .rodata, "a", @progbits
marker:
	.asciz	"\r\nXR500V RAM JUMP OK\r\n"
