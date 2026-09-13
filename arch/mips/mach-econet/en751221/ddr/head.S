/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text, "ax", @progbits
.set noreorder
.space 0x280, 0
.globl start
.globl stext
.globl _stext
start:
stext:
_stext:
	la $26, head_start
	jr $26
	nop
head_start:
	la $28, init_task_union
	addiu $8, $28, 0x9e0
	addiu $29, $8, -16
	la $8, __bss_start
	la $9, _end
	beq $8, $9, 2f
	nop
1:
	sw $0, 0($8)
	addiu $8, $8, 4
	sltu $10, $8, $9
	bnez $10, 1b
	nop
2:
	jal init_arch
	nop
3:
	b 3b
	nop
.set reorder
.section .note.GNU-stack,"",@progbits
