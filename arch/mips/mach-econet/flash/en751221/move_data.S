/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EN751221 first-stage flash copier.
 *
 * Source-level port of the active EN751221 path from the EcoNet SDK
 * en7512_boot/move_data/init/move_data.S.
 *
 * Runtime ABI:
 *
 *	s1	source address in the flash image
 *	s2	last source address
 *	s3	destination address
 *	s4	return address
 *
 * The EN751221 SPI-NAND boot path uses fixed 2048-byte NAND pages.
 */

#include <asm/regdef.h>

#define SF_READ_IDLE_EN			0xbfa10004
#define SF_MTX_MODE_TOG			0xbfa10014
#define SF_RDCTL_FSM			0xbfa10018
#define SF_MACMUX_SEL			0xbfa1001c
#define SF_MANUAL_EN			0xbfa10020
#define SF_MANUAL_OPFIFO_EMPTY		0xbfa10024
#define SF_MANUAL_OPFIFO_WDATA		0xbfa10028
#define SF_MANUAL_OPFIFO_FULL		0xbfa1002c
#define SF_MANUAL_OPFIFO_WR		0xbfa10030
#define SF_MANUAL_DFIFO_FULL		0xbfa10034
#define SF_MANUAL_DFIFO_WDATA		0xbfa10038
#define SF_MANUAL_DFIFO_EMPTY		0xbfa1003c
#define SF_MANUAL_DFIFO_RD		0xbfa10040
#define SF_MANUAL_DFIFO_RDATA		0xbfa10044
#define SF_SI_CK_SEL			0xbfa1009c
#define SF_STRAP			0xbfa10114

#define OP_CSH				0x00
#define OP_CSL				0x01
#define OP_CK				0x02
#define OP_OUTS				0x08
#define OP_INS				0x0c

#define SF_OP_RD_DATA			0x03
#define SF_OP_RD_PAGE			0x13
#define SF_OP_GET_FEATURE		0x0f
#define SF_COMMAND_STATUS		0xc0
#define SF_STATUS_OIP			0x01

#define OP_SHIFT			9
#define OP_CMD_MASK			0x1f
#define OP_LEN_MASK			0x1ff

	.section .text, "ax", @progbits
	.set	noreorder

	.globl	move_data_entry
move_data_entry:
	/* SFC serial-input clock selector. */
	li	t1, SF_SI_CK_SEL
	li	t2, 9
	sw	t2, 0(t1)
	nop

	/* Leave the automatic reader and switch the SFC to manual mode. */
	li	t1, SF_READ_IDLE_EN
	sw	zero, 0(t1)
	nop

sfc_wait_idle:
	li	t1, SF_RDCTL_FSM
	lw	t7, 0(t1)
	nop
	bnez	t7, sfc_wait_idle
	nop

	li	t1, SF_MTX_MODE_TOG
	li	t2, 9
	sw	t2, 0(t1)
	nop

	li	t1, SF_MACMUX_SEL
	li	t2, 1
	sw	t2, 0(t1)
	nop

	li	t1, SF_MANUAL_EN
	li	t2, 1
	sw	t2, 0(t1)
	nop

	/*
	 * Strap:
	 *  bit 1: 0 = NOR, 1 = NAND
	 *  bit 0: 0 = 3-byte, 1 = 4-byte addressing
	 *  bit 2: dummy-byte placement
	 */
	li	t1, SF_STRAP
	lw	t4, 0(t1)
	nop
	andi	t4, t4, 0x2

	lw	t8, 0(t1)
	nop
	andi	t8, t8, 0x1

	lw	t0, 0(t1)
	nop
	andi	t0, t0, 0x4

start_load:
	/* NOR has no PAGE READ / GET FEATURE stage. */
	beqz	t4, read_data
	nop

load_page:
	slt	t3, s2, s1
	bnez	t3, load_done
	nop

	/* CS low. */
	li	t5, OP_CSL
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	/* PAGE READ, 13h. */
	li	t5, OP_OUTS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, SF_OP_RD_PAGE
	jal	send_datafifo_write_cmd
	nop

	/*
	 * EN751221 boot NAND is addressed as 2 KiB pages.
	 * s1 is a bfc0xxxx flash mapping, not a zero-based offset.
	 */
	li	t6, 0xbfc00000
	subu	t6, s1, t6
	srl	t6, t6, 11

	li	t5, OP_OUTS
	li	t7, 3
	jal	send_opfifo_write_cmd
	nop

	srl	t5, t6, 16
	andi	t5, t5, 0xff
	jal	send_datafifo_write_cmd
	nop

	srl	t5, t6, 8
	andi	t5, t5, 0xff
	jal	send_datafifo_write_cmd
	nop

	andi	t5, t6, 0xff
	jal	send_datafifo_write_cmd
	nop

	li	t5, OP_CSH
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, OP_CK
	li	t7, 5
	jal	send_opfifo_write_cmd
	nop

get_feature:
	/* GET FEATURE C0h until OIP clears. */
	li	t5, OP_CSL
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, OP_OUTS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, SF_OP_GET_FEATURE
	jal	send_datafifo_write_cmd
	nop

	li	t5, OP_OUTS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, SF_COMMAND_STATUS
	jal	send_datafifo_write_cmd
	nop

	li	t5, OP_INS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	jal	send_datafifo_read_cmd
	nop
	andi	t9, t5, SF_STATUS_OIP

	li	t5, OP_CSH
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, OP_CK
	li	t7, 5
	jal	send_opfifo_write_cmd
	nop

	bnez	t9, get_feature
	nop

read_data:
	slt	t3, s2, s1
	bnez	t3, load_done
	nop

	li	t5, OP_CSL
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, 0xbfc00000
	subu	t9, s1, t5

	/* NAND cache reads use only the column part here. */
	beqz	t4, address_ready
	nop
	andi	t9, t9, 0x7ff

address_ready:
	li	t5, OP_OUTS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, SF_OP_RD_DATA
	jal	send_datafifo_write_cmd
	nop

	/* Optional fourth address byte used by the strap-selected mode. */
	beqz	t8, three_byte_tail
	nop

	li	t5, OP_OUTS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	srl	t5, t9, 24
	andi	t5, t5, 0xff
	jal	send_datafifo_write_cmd
	nop

three_byte_tail:
	li	t5, OP_OUTS
	li	t7, 3
	jal	send_opfifo_write_cmd
	nop

	/* Dummy-prepend mode consumes the high column/address byte here. */
	bnez	t0, dummy_append
	nop

	srl	t5, t9, 16
	andi	t5, t5, 0xff
	jal	send_datafifo_write_cmd
	nop

dummy_append:
	srl	t5, t9, 8
	andi	t5, t5, 0xff
	jal	send_datafifo_write_cmd
	nop

	andi	t5, t9, 0xff
	jal	send_datafifo_write_cmd
	nop

	/* Dummy-append mode supplies the third byte after the column. */
	beqz	t0, read_one_byte
	nop

	move	t5, zero
	jal	send_datafifo_write_cmd
	nop

read_one_byte:
	slt	t3, s2, s1
	bnez	t3, load_done
	nop

	li	t5, OP_INS
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	jal	send_datafifo_read_cmd
	nop

	sb	t5, 0(s3)
	nop

	addiu	s1, s1, 1
	addiu	s3, s3, 1
	addiu	t9, t9, 1

	/*
	 * Preserve the SDK's 1 KiB cache-read chunks even though the
	 * physical NAND page is 2 KiB.
	 */
	andi	t5, t9, 0x3ff
	bnez	t5, read_one_byte
	nop

	li	t5, OP_CSH
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, OP_CK
	li	t7, 5
	jal	send_opfifo_write_cmd
	nop

	b	start_load
	nop

load_done:
	li	t5, OP_CSH
	li	t7, 1
	jal	send_opfifo_write_cmd
	nop

	li	t5, OP_CK
	li	t7, 5
	jal	send_opfifo_write_cmd
	nop

	jr	s4
	nop

send_datafifo_read_cmd:
	li	t1, SF_MANUAL_DFIFO_EMPTY
1:
	lw	t2, 0(t1)
	nop
	li	t3, 1
	beq	t2, t3, 1b
	nop

	li	t1, SF_MANUAL_DFIFO_RDATA
	lw	t5, 0(t1)
	nop
	andi	t5, t5, 0xff

	li	t1, SF_MANUAL_DFIFO_RD
	li	t2, 1
	sw	t2, 0(t1)
	nop

	jr	ra
	nop

send_opfifo_write_cmd:
	andi	t2, t5, OP_CMD_MASK
	sll	t2, t2, OP_SHIFT
	andi	t3, t7, OP_LEN_MASK
	or	t2, t2, t3

	li	t1, SF_MANUAL_OPFIFO_WDATA
	sw	t2, 0(t1)
	nop

	li	t1, SF_MANUAL_OPFIFO_FULL
1:
	lw	t2, 0(t1)
	nop
	bnez	t2, 1b
	nop

	li	t1, SF_MANUAL_OPFIFO_WR
	li	t2, 1
	sw	t2, 0(t1)
	nop

	li	t1, SF_MANUAL_OPFIFO_EMPTY
1:
	lw	t2, 0(t1)
	nop
	beqz	t2, 1b
	nop

	jr	ra
	nop

send_datafifo_write_cmd:
	li	t1, SF_MANUAL_DFIFO_FULL
1:
	lw	t2, 0(t1)
	nop
	li	t3, 1
	beq	t2, t3, 1b
	nop

	andi	t2, t5, 0xff
	li	t1, SF_MANUAL_DFIFO_WDATA
	sw	t2, 0(t1)
	nop

	jr	ra
	nop
