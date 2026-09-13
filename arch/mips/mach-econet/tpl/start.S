/* SPDX-License-Identifier: GPL-2.0+ */
#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/mipsregs.h>
#include <mach/boot.h>

	.set noreorder

ENTRY(_start)
	/* Match the vendor TCBoot reset vector. */
	j	reset
	 nop

	/*
	 * Common EcoNet/Airoha TCBoot descriptor. Raw bytes at +0x0c must be
	 * "6578" for both endian modes.  The vendor 128 KiB EN751221 layout
	 * treats the image at 0x10000 as bootram, while the newer 256 KiB layout
	 * treats the executable loader at 0x10000 as lzma.img.  Binman creates
	 * the fixed placement and tools/econet_tcboot_image.py patches the actual
	 * end address and CRC words after the image is assembled.
	 */
	.org	0x08
	.word	ECONET_TCBOOT_HEADER_LEN
	.ascii	"6578"
#ifdef CONFIG_TARGET_EN751221
	.word	0	/* lzma_flash_start_addr */
	.word	0	/* lzma_flash_end_addr */
	.word	ECONET_SPL_IMAGE_OFFSET	/* bootram_flash_start_addr */
	.word	0	/* bootram_flash_end_addr, finalized after Binman */
#else
	.word	ECONET_SPL_IMAGE_OFFSET	/* lzma_flash_start_addr */
	.word	0	/* lzma_flash_end_addr, finalized after Binman */
	.word	0	/* bootram_flash_start_addr */
	.word	0	/* bootram_flash_end_addr */
#endif
	.word	0
	.word	ECONET_TCBOOT_CHIP_FLASH_INFO
	.word	ECONET_TCBOOT_WORD28
	.word	ECONET_TCBOOT_WORD2C
	.word	ECONET_DDR_BLOB_ADDR
	.word	CONFIG_SPL_TEXT_BASE
	.word	0	/* verify_start_addr: unsigned boot */
	.word	0	/* verify_end_addr: unsigned boot */

#ifdef CONFIG_TARGET_EN751221
	/*
	 * The stock EN751221 tcboot starts with:
	 *
	 *   0b f0 00 10    j 0xbfc00040
	 *
	 * Match that reset target exactly. Keep this diagnostic entry inside
	 * the small initial window made available by the internal ROM.
	 */
	.org	0x40
#else
	.word	0	/* efuse_clk_width */
	.word	0
	.word	0	/* move_data_int start */
	.word	0	/* move_data_int end */
	.word	0	/* boot2 start */
	.word	0	/* boot2 end */
	.word	ECONET_DDR_BLOB_OFFSET	/* spram_flash_start_addr */
	.word	ECONET_DDR_BLOB_OFFSET + ECONET_DDR_BLOB_SIZE
	.org	0x60
#endif
/*
 * FE SRAM destinations for the move_data diagnostic below.
 *
 * The real vendor TCBoot (airoha_sdk/boot/bootrom/en7512_boot/
 * start.S_7512_final) copies a small block from flash (0x310..0x78c in
 * the stock dump) into FE SRAM at 0xbfa30040 and jumps there *before*
 * ever touching the SFC. That is not decoration: NAND page 0 (this
 * file's first 0x800 bytes) is only guaranteed fetchable as code
 * *before* SF_MANUAL_EN=1. Once manual mode is on, even code inside
 * that same first page stops being safe to fetch from flash -- which is
 * exactly why an earlier version of this diagnostic (running everything
 * in place from flash) hung silently the instant it entered manual mode,
 * no matter which register fix was tried.
 *
 * So the SFC-touching code below (en751221_ram_start..en751221_ram_end)
 * is copied to EN751221_RAM_ADDR and executed from there, same as the
 * vendor does. Both addresses sit comfortably clear of the DDR blob copy
 * (EN7512_DDR_BLOB_ADDR + EN7512_DDR_BLOB_SIZE ends at +0x7770) and of
 * the TPL stack/scratch words at +0xbff0..+0xbfff.
 */
#define EN751221_RAM_ADDR	(EN7512_FE_SRAM_CACHED + 0x8000)
#define EN751221_STAGE1_ADDR	(EN7512_FE_SRAM_CACHED + 0x8800)
#define EN751221_STAGE1_SIZE	0x800
#define EN751221_SPIN_LIMIT	0x100000

/*
 * U-Boot proper loading, skipping SPL entirely (see the comment by the
 * 'R' checkpoint below). EN751221_UBOOT_HDR_ADDR reuses EN751221_RAM_ADDR:
 * by the time stage1 reads the uImage header, the move_data block that was
 * copied there has already run and jumped away, so that FE SRAM is dead
 * scratch space again.
 */
#define EN751221_UBOOT_OFFSET	EN7512_UBOOT_IMAGE_OFFSET
#define EN751221_UBOOT_HDR_ADDR	EN751221_RAM_ADDR
#define IH_MAGIC		0x27051956
#define IH_HDR_SIZE		64

/* SFC manual-mode primitives, mirrored from early_sfc.c's op encoding. */
#define SF_OP_CSH		0x00
#define SF_OP_CSL		0x01
#define SF_OP_CK		0x02
#define SF_OP_OUTS		0x08
#define SF_OP_INS		0x0c
#define SF_OP_SHIFT		9

/*
 * These are macros, not subroutines. en751221_ram_start..end gets copied
 * to FE SRAM and run from there, and MIPS jal/j targets are not fully
 * PC-relative: the low bits are fixed at assemble time for the *original*
 * (flash) link address, while the high segment bits come from wherever
 * the processor is *currently* executing. A jal from the FE SRAM copy
 * back into itself would combine the FE SRAM segment with a flash-era
 * target and jump into the wrong segment entirely. Macros sidestep this
 * by inlining the same instructions at every use site, so all branches
 * are local (PC-relative) and survive being copied verbatim to a new
 * address. Local numeric labels use \@ (unique per macro invocation) so
 * repeated expansions never collide with each other or with the plain
 * numeric labels used outside the macros elsewhere in this file.
 */
.macro sf_op opval
	li	a0, \opval
	li	t0, ECONET_SFC_BASE
	sw	a0, 0x028(t0)		/* SF_MANUAL_OPFIFO_WDATA */
1\@:	lw	t1, 0x02c(t0)		/* SF_MANUAL_OPFIFO_FULL */
	bnez	t1, 1\@b
	 nop
	li	t1, 1
	sw	t1, 0x030(t0)		/* SF_MANUAL_OPFIFO_WR */
2\@:	lw	t1, 0x024(t0)		/* SF_MANUAL_OPFIFO_EMPTY */
	beqz	t1, 2\@b
	 nop
.endm

.macro sf_put_byte val
	li	a0, \val
	li	t0, ECONET_SFC_BASE
3\@:	lw	t1, 0x034(t0)		/* SF_MANUAL_DFIFO_FULL */
	bnez	t1, 3\@b
	 nop
	sw	a0, 0x038(t0)		/* SF_MANUAL_DFIFO_WDATA */
.endm

/* Result in v0. */
.macro sf_get_byte
	li	t0, ECONET_SFC_BASE
4\@:	lw	t1, 0x03c(t0)		/* SF_MANUAL_DFIFO_EMPTY */
	bnez	t1, 4\@b
	 nop
	lw	v0, 0x044(t0)		/* SF_MANUAL_DFIFO_RDATA */
	andi	v0, v0, 0xff
	li	t1, 1
	sw	t1, 0x040(t0)		/* SF_MANUAL_DFIFO_RD */
.endm

.macro sf_finish
	sf_op	((SF_OP_CSH << SF_OP_SHIFT) | 1)
	sf_op	((SF_OP_CK << SF_OP_SHIFT) | 5)
.endm

.macro sf_putc ch
	li	a0, \ch
	li	t0, ECONET_UART0_BASE
5\@:	lw	t1, 0x14(t0)		/* LSR */
	andi	t1, t1, 0x20		/* THRE */
	beqz	t1, 5\@b
	 nop
	sw	a0, 0x00(t0)		/* THR */
	sync
.endm

reset:
#ifdef CONFIG_TARGET_EN751221
	/*
	 * Cold-boot move_data diagnostic -- flash-resident half. Only UART,
	 * arbiter/SMC setup and the copy-to-FE-SRAM loop run from here; the
	 * SFC itself is never touched while executing from flash.
	 *
	 * Breadcrumbs:
	 *   S = reached reset, UART is alive
	 *   I = SFC manual-mode init timed out (RDCTL_FSM never idled); hangs
	 *   L = PAGE READ (0x13) command issued, entering the OIP poll
	 *   P = GET FEATURE OIP poll timed out; hangs
	 *   C = OIP cleared, page 1 is loaded into the NAND's cache
	 *   D = READ CACHE (0x03) command + address issued, copy loop next
	 *   J = stage1 copied to FE SRAM, about to jump
	 *   M = stage1 itself executed (printed by stage1, not stage0)
	 */
	li	t0, ECONET_UART0_BASE

	/* Same EN751221 UART programming used by tpl_uart_init(). */
	li	t1, 0x80
	sw	t1, 0x0c(t0)		/* LCR: DLAB */
	li	t1, 0xea00fde8
	sw	t1, 0x2c(t0)		/* XYD */
	li	t1, 0x01
	sw	t1, 0x00(t0)		/* DLL / THR */
	sw	zero, 0x04(t0)		/* DLM / IER */
	li	t1, 0x03
	sw	t1, 0x0c(t0)		/* LCR: 8N1 */
	li	t1, 0x0f
	sw	t1, 0x08(t0)		/* FCR */
	sw	zero, 0x10(t0)		/* MCR */
	sw	zero, 0x24(t0)		/* MISCC */
	sw	zero, 0x04(t0)		/* IER */
	sync

	/* Arbiter and static-memory mappings (matches start.S_7512_final). */
	li	t0, ECONET_SYSCTL_BASE
	li	t1, 0x80071f1e
	sw	t1, 0x20(t0)
	li	t1, 0x00071f1f
	sw	t1, 0x24(t0)
	li	t1, 0x80050000
	sw	t1, 0x34(t0)

	li	t0, ECONET_SMC_BASE
	li	t1, 0x102d1040
	sw	t1, 0x00(t0)
	li	t1, 0x200028d0
	sw	t1, 0x14(t0)

	li	t0, ECONET_INTC_BASE
	sw	zero, 0x00(t0)
	sw	zero, 0x04(t0)

	/* Enable PBUS access to the FE SRAM window before touching 0x9fa3xxxx. */
	li	t0, ECONET_FE_SRAM_SEL
	lw	t1, 0(t0)
	ori	t1, t1, 1
	sw	t1, 0(t0)
	sync

	sf_putc	'S'

	/*
	 * Copy the SFC-touching block to FE SRAM and run it from there.
	 * Still executing from flash here, so plain la (absolute, computed
	 * for the flash link address) is correct for the copy source.
	 */
	la	t0, en751221_ram_start
	la	t1, en751221_ram_end
	li	t2, EN751221_RAM_ADDR
copy_ram_block:
	lw	t3, 0(t0)
	sw	t3, 0(t2)
	addiu	t0, t0, 4
	addiu	t2, t2, 4
	bne	t0, t1, copy_ram_block
	 nop

	li	t9, EN751221_RAM_ADDR
	jr	t9
	 nop

/*
 * Everything from here to en751221_ram_end is position-independent (only
 * PC-relative branches and macros, no jal/j to labels inside itself) and
 * gets copied to and executed at EN751221_RAM_ADDR, not at the flash
 * address it is assembled/linked at.
 */
en751221_ram_start:
	/*
	 * econet_sfc_init(), inlined. NFI2SPI_EN (0x130) doesn't exist in
	 * early_sfc.c or the older bootrom move_data.S -- it only showed up
	 * in the 2023 u-boot-airoha SPI-NAND driver's manual-mode entry
	 * (airht_spim.c: airoha_spim_set_mode, SPI_CONTROLLER_MODE_MANUAL),
	 * written to 0 before anything else. Left at its reset value, this
	 * mux may route the manual OPFIFO/DFIFO through the NFI engine
	 * instead of the raw SFC pins.
	 */
	li	t0, ECONET_SFC_BASE
	sw	zero, 0x130(t0)		/* SF_NFI2SPI_EN */
	li	t1, 0x9
	sw	t1, 0x09c(t0)		/* SF_SI_CK_SEL */
	sw	zero, 0x004(t0)		/* SF_READ_IDLE_EN */

	li	t2, EN751221_SPIN_LIMIT
fsm_wait:
	lw	t1, 0x018(t0)		/* SF_RDCTL_FSM */
	beqz	t1, fsm_ready
	 nop
	addiu	t2, t2, -1
	bnez	t2, fsm_wait
	 nop
	sf_putc	'I'
	sf_putc	0x0d
	sf_putc	0x0a
hang_i:
	b	hang_i
	 nop
fsm_ready:
	li	t1, 0x9
	sw	t1, 0x014(t0)		/* SF_MTX_MODE_TOG */
	li	t1, 1
	sw	t1, 0x01c(t0)		/* SF_MACMUX_SEL */
	sw	t1, 0x020(t0)		/* SF_MANUAL_EN */

	/* sf_nand_load_page(1): PAGE READ (0x13) + 3 address bytes. */
	sf_op	(SF_OP_CSL << SF_OP_SHIFT) | 1
	sf_op	(SF_OP_OUTS << SF_OP_SHIFT) | 1
	sf_put_byte 0x13
	sf_op	(SF_OP_OUTS << SF_OP_SHIFT) | 3
	sf_put_byte 0			/* page 1, byte 2 (bits 23:16) */
	sf_put_byte 0			/* page 1, byte 1 (bits 15:8) */
	sf_put_byte 1			/* page 1, byte 0 (bits 7:0) == stage1 */
	sf_finish

	sf_putc	'L'

	/* Poll GET FEATURE (0xc0) until OIP (status bit 0) clears. */
	li	s1, EN751221_SPIN_LIMIT
oip_poll:
	sf_op	(SF_OP_CSL << SF_OP_SHIFT) | 1
	sf_op	(SF_OP_OUTS << SF_OP_SHIFT) | 1
	sf_put_byte 0x0f
	sf_op	(SF_OP_OUTS << SF_OP_SHIFT) | 1
	sf_put_byte 0xc0
	sf_op	(SF_OP_INS << SF_OP_SHIFT) | 1
	sf_get_byte
	move	t3, v0
	sf_finish
	andi	t1, t3, 1
	beqz	t1, oip_done
	 nop
	addiu	s1, s1, -1
	bnez	s1, oip_poll
	 nop
	sf_putc	'P'
	sf_putc	0x0d
	sf_putc	0x0a
hang_p:
	b	hang_p
	 nop
oip_done:
	sf_putc	'C'

	/* sf_nand_read_cache(0, EN751221_STAGE1_ADDR, 0x800). */
	sf_op	(SF_OP_CSL << SF_OP_SHIFT) | 1
	sf_op	(SF_OP_OUTS << SF_OP_SHIFT) | 1
	sf_put_byte 0x03
	sf_op	(SF_OP_OUTS << SF_OP_SHIFT) | 3
	/* column 0, non-dummy-append byte order: {0, col>>8, col} = {0,0,0}. */
	sf_put_byte 0
	sf_put_byte 0
	sf_put_byte 0

	sf_putc	'D'

	/*
	 * Assemble 4 bytes from the DFIFO into a register before each store
	 * so the FE SRAM copy is a 32-bit sw, never a byte store.
	 */
	li	s0, EN751221_STAGE1_ADDR
	li	s1, EN751221_STAGE1_SIZE
copy_stage1_word:
	li	t2, 4
	li	t3, 0
copy_stage1_byte:
	sf_op	(SF_OP_INS << SF_OP_SHIFT) | 1
	sf_get_byte
	sll	t3, t3, 8
	or	t3, t3, v0
	addiu	t2, t2, -1
	bnez	t2, copy_stage1_byte
	 nop
	sw	t3, 0(s0)
	addiu	s0, s0, 4
	addiu	s1, s1, -4
	bnez	s1, copy_stage1_word
	 nop

	sf_finish

	sf_putc	'J'

	li	t9, EN751221_STAGE1_ADDR
	jr	t9
	 nop
en751221_ram_end:

	/*
	 * Stage1 lives at NAND offset 0x800 and executes from
	 * EN751221_STAGE1_ADDR (0x9fa38800).
	 *
	 * Keep this entire block position-independent.  bal/b are PC-relative;
	 * do not use jal/j/la for labels inside this relocated block.
	 */
	.org	0x800
en751221_stage1:

	li	a0, 'M'
	bal	s1_putc
	 nop

	/*
	 * en751221_ddr.bin:
	 *
	 * flash: 0x00008000
	 * page:  0x10
	 * size:  EN7512_DDR_BLOB_SIZE (currently 0x4f70)
	 * dst:   EN7512_DDR_BLOB_ADDR = 0x9fa32800
	 *
	 * s0 = current NAND page
	 * s1 = total bytes remaining
	 * s2 = destination
	 * s3 = current page byte count
	 * s4 = OIP timeout
	 * s5 = bytes left in current cache transfer
	 */
	li	s0, (EN7512_DDR_BLOB_OFFSET / 0x800)
	li	s1, EN7512_DDR_BLOB_SIZE
	li	s2, EN7512_DDR_BLOB_ADDR

ddr_next_page:
	/* PAGE READ 0x13 */
	li	a0, ((SF_OP_CSL << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, ((SF_OP_OUTS << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, 0x13
	bal	s1_put_byte
	 nop

	li	a0, ((SF_OP_OUTS << SF_OP_SHIFT) | 3)
	bal	s1_sf_op
	 nop

	/* DDR blob occupies NAND pages 0x10..0x19. */
	move	a0, zero
	bal	s1_put_byte
	 nop

	move	a0, zero
	bal	s1_put_byte
	 nop

	move	a0, s0
	bal	s1_put_byte
	 nop

	bal	s1_finish
	 nop

	/* Wait for NAND OIP == 0. */
	li	s4, EN751221_SPIN_LIMIT

ddr_oip_poll:
	li	a0, ((SF_OP_CSL << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, ((SF_OP_OUTS << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, 0x0f
	bal	s1_put_byte
	 nop

	li	a0, ((SF_OP_OUTS << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, 0xc0
	bal	s1_put_byte
	 nop

	li	a0, ((SF_OP_INS << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	bal	s1_get_byte
	 nop

	move	t3, v0

	bal	s1_finish
	 nop

	andi	t1, t3, 1
	beqz	t1, ddr_oip_done
	 nop

	addiu	s4, s4, -1
	bnez	s4, ddr_oip_poll
	 nop

	li	a0, 'P'
	bal	s1_putc
	 nop

ddr_oip_hang:
	b	ddr_oip_hang
	 nop

ddr_oip_done:
	/* READ CACHE 0x03, column 0. */
	li	a0, ((SF_OP_CSL << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, ((SF_OP_OUTS << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, 0x03
	bal	s1_put_byte
	 nop

	li	a0, ((SF_OP_OUTS << SF_OP_SHIFT) | 3)
	bal	s1_sf_op
	 nop

	move	a0, zero
	bal	s1_put_byte
	 nop

	move	a0, zero
	bal	s1_put_byte
	 nop

	move	a0, zero
	bal	s1_put_byte
	 nop

	/* s3 = min(s1, 0x800). */
	li	s3, 0x800
	sltu	t2, s1, s3
	beqz	t2, ddr_chunk_ready
	 nop
	move	s3, s1

ddr_chunk_ready:
	move	s5, s3

	/*
	 * Avoid byte stores to FE SRAM.  Assemble each four bytes into one
	 * big-endian word and commit with sw.
	 */
ddr_copy_word:
	li	t2, 4
	move	t3, zero

ddr_copy_byte:
	li	a0, ((SF_OP_INS << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	bal	s1_get_byte
	 nop

	sll	t3, t3, 8
	or	t3, t3, v0

	addiu	t2, t2, -1
	bnez	t2, ddr_copy_byte
	 nop

	sw	t3, 0(s2)
	addiu	s2, s2, 4

	addiu	s5, s5, -4
	bnez	s5, ddr_copy_word
	 nop

	bal	s1_finish
	 nop

	subu	s1, s1, s3
	addiu	s0, s0, 1

	bnez	s1, ddr_next_page
	 nop

	sync

	/* Complete DDR image is now resident at 0x9fa32800. */
	li	a0, 'B'
	bal	s1_putc
	 nop

	/*
	 * The DDR calibration payload may destroy sp.  Use the same scratch
	 * locations as econet_run_ddr_blob().
	 */
	li	sp, EN7512_TPL_STACK_ADDR
	addiu	sp, sp, -16

	li	t0, EN7512_TPL_SAVED_SP
	sw	sp, 0(t0)

	/*
	 * SCREG_WR0 must contain the SRAM runtime address, not the flash link
	 * address. bal obtains our actual PC after relocation.
	 */
	bal	1f
	 nop
1:
	addiu	t1, ra, ddr_return - 1b

	li	t0, EN7512_SCREG_WR0
	sw	t1, 0(t0)
	sync

	li	a0, 'G'
	bal	s1_putc
	 nop

	li	t9, EN7512_DDR_BLOB_ENTRY
	jr	t9
	 nop

ddr_return:
	li	t0, EN7512_SCREG_WR0
	sw	zero, 0(t0)

	li	t0, EN7512_TPL_SAVED_SP
	lw	sp, 0(t0)

	li	a0, 'R'
	bal	s1_putc
	 nop

	/*
	 * Match chainloader/start.S exactly before its XMODEM byte receiver.
	 * Invalidating both L1 caches before enabling KSEG0 caching is material:
	 * with stale reset/DDR cache state merely setting Config.K0=3 still left
	 * the NAND-loaded payload corrupted in DRAM.
	 */
	mfc0	t0, $16, 1		/* Config1 */

	srl	t1, t0, 10
	andi	t1, t1, 7
	beqz	t1, cache_prep_d_skip
	 nop
	li	t2, 2
	sllv	t2, t2, t1
	srl	t3, t0, 13
	andi	t3, t3, 7
	li	t4, 64
	sllv	t4, t4, t3
	srl	t5, t0, 7
	andi	t5, t5, 7
	addiu	t5, t5, 1
	mul	t6, t2, t4
	mul	t6, t6, t5
	lui	t7, 0x8000
	addu	t8, t7, t6
cache_prep_d_loop:
	cache	0x01, 0(t7)		/* Index Writeback Invalidate (D) */
	addu	t7, t7, t2
	bne	t7, t8, cache_prep_d_loop
	 nop
	sync
cache_prep_d_skip:

	srl	t1, t0, 19
	andi	t1, t1, 7
	beqz	t1, cache_prep_i_skip
	 nop
	li	t2, 2
	sllv	t2, t2, t1
	srl	t3, t0, 22
	andi	t3, t3, 7
	li	t4, 64
	sllv	t4, t4, t3
	srl	t5, t0, 16
	andi	t5, t5, 7
	addiu	t5, t5, 1
	mul	t6, t2, t4
	mul	t6, t6, t5
	lui	t7, 0x8000
	addu	t8, t7, t6
cache_prep_i_loop:
	cache	0x00, 0(t7)		/* Index Invalidate (I) */
	addu	t7, t7, t2
	bne	t7, t8, cache_prep_i_loop
	 nop
	sync
cache_prep_i_skip:

	mfc0	t0, $16
	li	t1, -8
	and	t0, t0, t1
	ori	t0, t0, 3		/* Config.K0 = cached, non-coherent */
	mtc0	t0, $16
	ehb
	sync

	/*
	 * DRAM is calibrated and our own DDR-return trampoline works. Skip
	 * SPL entirely (same shape as the now hardware-proven EN7528 flash
	 * loader): read the U-Boot proper uImage straight from NAND into
	 * DRAM at its own load address and jump to its entry point. U-Boot
	 * itself is already known-good on this board (chainloading it has
	 * worked all session) -- only getting it into DRAM from cold boot
	 * is new.
	 */
	li	a0, EN751221_UBOOT_OFFSET
	li	a1, EN751221_UBOOT_HDR_ADDR
	li	a2, IH_HDR_SIZE
	bal	s1_read_range
	 nop

	li	t0, EN751221_UBOOT_HDR_ADDR
	lw	t1, 0(t0)		/* magic */
	li	t2, IH_MAGIC
	beq	t1, t2, uboot_hdr_ok
	 nop
	li	a0, 'X'
	bal	s1_putc
	 nop
uboot_hdr_hang:
	b	uboot_hdr_hang
	 nop
uboot_hdr_ok:
	li	a0, 'H'
	bal	s1_putc
	 nop

	lw	s0, 12(t0)		/* size */
	lw	s1, 16(t0)		/* load */

	li	a0, 'P'			/* about to start the DRAM payload copy */
	bal	s1_putc
	 nop

	li	a0, EN751221_UBOOT_OFFSET + IH_HDR_SIZE
	move	a1, s1
	move	a2, s0
	bal	s1_read_range
	 nop

	sync

	li	a0, 'Z'
	bal	s1_putc
	 nop

	/*
	 * Diagnostic, gathered only *after* the copy loop returns (see the
	 * comment in s1_read_range's copy loop for why nothing may print
	 * mid-copy): the whole first chunk [0,1984) and the rest of the
	 * first quarter [1984,174952) were BOTH wrong last round (even
	 * though the very first word alone had checked out fine in an
	 * earlier, differently-instrumented build) -- so corruption starts
	 * somewhere inside the tricky first (partial-column) chunk itself.
	 * Bisect that chunk: [0,992) vs [992,1984). ih_load is reloaded from
	 * the header scratch since s1_read_range clobbers s0/s1.
	 */
	li	t0, EN751221_UBOOT_HDR_ADDR
	lw	t9, 16(t0)		/* ih_load, held here until reloaded below */

	move	a0, t9
	li	a1, 992
	bal	s1_csum_print
	 nop
	li	a0, ' '
	bal	s1_putc
	 nop

	addiu	a0, t9, 992
	li	a1, 992
	bal	s1_csum_print
	 nop
	li	a0, ' '
	bal	s1_putc
	 nop

	li	t0, EN751221_UBOOT_HDR_ADDR
	lw	t9, 20(t0)		/* ep; grab before t0-t8 get reused below */

	/*
	 * Full cache flush + uncached KSEG0 handoff, copied from the
	 * already-proven chainloader's chainload_jump (arch/mips/mach-econet/
	 * chainloader/start.S): the payload was written through the plain
	 * cached alias (see s1_read_range), so the D-cache needs an explicit
	 * writeback before the jump, and the I-cache needs invalidating so
	 * fetch doesn't see whatever used to live at these lines. Uses
	 * Index-type ops over the whole cache (geometry read from CP0
	 * Config1), not Hit-type ops over just our address range: an earlier
	 * version tried the targeted Hit approach and U-Boot never printed
	 * anything after the jump.
	 */
	mfc0	t0, $16, 1		/* Config1 */

	srl	t1, t0, 10
	andi	t1, t1, 7
	beqz	t1, cache_dflush_skip
	 nop
	li	t2, 2
	sllv	t2, t2, t1		/* D line size */
	srl	t3, t0, 13
	andi	t3, t3, 7
	li	t4, 64
	sllv	t4, t4, t3		/* D sets */
	srl	t5, t0, 7
	andi	t5, t5, 7
	addiu	t5, t5, 1		/* D ways */
	mul	t6, t2, t4
	mul	t6, t6, t5		/* D total size */
	lui	t7, 0x8000
	addu	t8, t7, t6
cache_dflush_loop:
	cache	0x01, 0(t7)		/* Index Writeback Invalidate (D) */
	addu	t7, t7, t2
	bne	t7, t8, cache_dflush_loop
	 nop
	sync
cache_dflush_skip:

	srl	t1, t0, 19
	andi	t1, t1, 7
	beqz	t1, cache_iflush_skip
	 nop
	li	t2, 2
	sllv	t2, t2, t1		/* I line size */
	srl	t3, t0, 22
	andi	t3, t3, 7
	li	t4, 64
	sllv	t4, t4, t3		/* I sets */
	srl	t5, t0, 16
	andi	t5, t5, 7
	addiu	t5, t5, 1		/* I ways */
	mul	t6, t2, t4
	mul	t6, t6, t5		/* I total size */
	lui	t7, 0x8000
	addu	t8, t7, t6
cache_iflush_loop:
	cache	0x00, 0(t7)		/* Index Invalidate (I) */
	addu	t7, t7, t2
	bne	t7, t8, cache_iflush_loop
	 nop
	sync
cache_iflush_skip:

	mfc0	t0, $16
	li	t1, -8
	and	t0, t0, t1
	ori	t0, t0, 2
	mtc0	t0, $16
	ehb
	sync

	jr	t9
	 nop

ddr_done:
	b	ddr_done
	 nop

/*
 * Stage1-local PIC helpers.
 *
 * They are called with bal, not jal, so relocation from bfc00800 to
 * 9fa38800 does not change their targets.
 */
s1_sf_op:
	li	t0, ECONET_SFC_BASE

	/* Hardware-proven ordering: WDATA before polling FULL. */
	sw	a0, 0x028(t0)

1:
	lw	t1, 0x02c(t0)
	bnez	t1, 1b
	 nop

	li	t1, 1
	sw	t1, 0x030(t0)

2:
	lw	t1, 0x024(t0)
	beqz	t1, 2b
	 nop

	jr	ra
	 nop

s1_put_byte:
	li	t0, ECONET_SFC_BASE

3:
	lw	t1, 0x034(t0)
	bnez	t1, 3b
	 nop

	andi	a0, a0, 0xff
	sw	a0, 0x038(t0)

	jr	ra
	 nop

s1_get_byte:
	li	t0, ECONET_SFC_BASE

4:
	lw	t1, 0x03c(t0)
	bnez	t1, 4b
	 nop

	lw	v0, 0x044(t0)
	andi	v0, v0, 0xff

	li	t1, 1
	sw	t1, 0x040(t0)

	jr	ra
	 nop

s1_finish:
	/*
	 * Preserve caller's ra because both bal below overwrite it.
	 * t9 is untouched by s1_sf_op.
	 */
	move	t9, ra

	li	a0, ((SF_OP_CSH << SF_OP_SHIFT) | 1)
	bal	s1_sf_op
	 nop

	li	a0, ((SF_OP_CK << SF_OP_SHIFT) | 5)
	bal	s1_sf_op
	 nop

	jr	t9
	 nop

s1_putc:
	li	t0, ECONET_UART0_BASE

5:
	lw	t1, 0x14(t0)
	andi	t1, t1, 0x20
	beqz	t1, 5b
	 nop

	sw	a0, 0x00(t0)
	sync

	jr	ra
	 nop

/*
 * a0 = start addr, a1 = byte length (multiple of 4). Sums the range as
 * big-endian 32-bit words and prints the sum as 8 hex digits via
 * s1_putc. Diagnostic only, called strictly after s1_read_range's copy
 * loop has already returned -- never from inside it (see the comment
 * there for why).
 *
 * bal-callable: saves its own incoming ra in s3, distinct from
 * s1_finish's t9 and s1_read_range's s7 (both long since returned by
 * the time stage1 calls this).
 */
s1_csum_print:
	move	s3, ra
	move	t6, a0
	addu	t5, a0, a1
	li	t7, 0
csum_print_loop:
	beq	t6, t5, csum_print_done
	 nop
	lw	t8, 0(t6)
	addu	t7, t7, t8
	addiu	t6, t6, 4
	b	csum_print_loop
	 nop
csum_print_done:
	li	t2, 8
	move	t3, t7
csum_print_hex_loop:
	srl	t4, t3, 28
	andi	t4, t4, 0xf
	sltiu	t5, t4, 10
	beqz	t5, csum_print_hex_alpha
	 nop
	addiu	a0, t4, '0'
	b	csum_print_hex_emit
	 nop
csum_print_hex_alpha:
	addiu	a0, t4, ('a' - 10)
csum_print_hex_emit:
	bal	s1_putc
	 nop
	sll	t3, t3, 4
	addiu	t2, t2, -1
	bnez	t2, csum_print_hex_loop
	 nop
	jr	s3
	 nop

/*
 * a0 = flash byte offset, a1 = destination, a2 = byte length.
 *
 * Generalizes the DDR-blob copy loop above to an arbitrary offset/length
 * spanning NAND page boundaries, for reading the U-Boot uImage header and
 * payload straight into DRAM. Never a byte store: each group of up to 4
 * bytes is assembled into a register first, and the final partial word
 * (if length isn't a multiple of 4) is zero-padded on the tail -- same
 * rule tpl_en751221_read_dram() uses for the SPL-loading path.
 *
 * Writes through the plain CACHED alias on purpose: the chainloader
 * (arch/mips/mach-econet/chainloader/chainloader.c, xmodem_receive())
 * writes the same 0x81000000 destination with plain cached byte stores
 * and documents why -- "uncached 8-bit stores corrupt the word on this
 * chip; with cache, every store becomes a full line writeback and the
 * problem goes away." An earlier version of this routine wrote through
 * the KSEG1 uncached alias instead, and while the header load (into FE
 * SRAM) still worked, the U-Boot payload load did not: it reached this
 * point every time but U-Boot never printed a single character. Cache
 * coherency for the jump is handled once, in bulk, by en751221_stage1
 * right before jumping to the entry point (mirroring chainload_jump),
 * not per range-read here.
 *
 * bal-callable: saves its own incoming ra in s7 (distinct from s1_finish's
 * t9) since it calls s1_finish internally once per page.
 */
s1_read_range:
	move	s7, ra
	move	s5, a0		/* s5 = flash offset cursor */
	move	s6, a1		/* s6 = dest cursor (plain cached address) */
	move	s4, a2		/* s4 = remaining length */

read_range_loop:
	beqz	s4, read_range_done
	 nop

	srl	s0, s5, 11		/* s0 = page = offset >> 11 */
	andi	s1, s5, 0x7ff		/* s1 = column = offset & 0x7ff */
	li	s2, 0x800
	subu	s2, s2, s1		/* s2 = bytes left in this page */
	sltu	t1, s4, s2
	beqz	t1, range_chunk_ready
	 nop
	move	s2, s4			/* clamp chunk to remaining length */
range_chunk_ready:

	/* PAGE READ(s0). */
	li	a0, (SF_OP_CSL << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, (SF_OP_OUTS << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, 0x13
	bal	s1_put_byte
	 nop
	li	a0, (SF_OP_OUTS << SF_OP_SHIFT) | 3
	bal	s1_sf_op
	 nop
	srl	t2, s0, 16
	andi	a0, t2, 0xff
	bal	s1_put_byte
	 nop
	srl	t2, s0, 8
	andi	a0, t2, 0xff
	bal	s1_put_byte
	 nop
	andi	a0, s0, 0xff
	bal	s1_put_byte
	 nop
	bal	s1_finish
	 nop

	/* Poll GET FEATURE (0xc0) until OIP (status bit 0) clears. */
	li	s3, EN751221_SPIN_LIMIT
range_oip_poll:
	li	a0, (SF_OP_CSL << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, (SF_OP_OUTS << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, 0x0f
	bal	s1_put_byte
	 nop
	li	a0, (SF_OP_OUTS << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, 0xc0
	bal	s1_put_byte
	 nop
	li	a0, (SF_OP_INS << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	bal	s1_get_byte
	 nop
	move	t3, v0
	bal	s1_finish
	 nop
	andi	t1, t3, 1
	beqz	t1, range_oip_done
	 nop
	addiu	s3, s3, -1
	bnez	s3, range_oip_poll
	 nop
	li	a0, 'Y'
	bal	s1_putc
	 nop
range_oip_hang:
	b	range_oip_hang
	 nop
range_oip_done:

	/* READ CACHE(column=s1), s2 bytes into (s6). */
	li	a0, (SF_OP_CSL << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, (SF_OP_OUTS << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	li	a0, 0x03
	bal	s1_put_byte
	 nop
	li	a0, (SF_OP_OUTS << SF_OP_SHIFT) | 3
	bal	s1_sf_op
	 nop
	/*
	 * MX35LF1GE4AB (this hardware's SPI-NAND, per spi_nand_probe's
	 * mfr_id=0xc2/dev_id=0x12 from the very first boot log) uses
	 * DUMMY_BYTE_APPEND column addressing: {col>>8, col, dummy}, not
	 * the {0, col>>8, col} order used everywhere else in this file.
	 * That was invisible as long as every READ CACHE used column 0
	 * (stage1 itself, every DDR blob page, the uImage header): both
	 * encodings produce {0,0,0} there. The U-Boot payload's first
	 * chunk is the first cold-boot read with column != 0 (0x40, right
	 * after the 64-byte header) and the first to expose this.
	 */
	srl	t2, s1, 8
	andi	a0, t2, 0xff
	bal	s1_put_byte
	 nop
	andi	a0, s1, 0xff
	bal	s1_put_byte
	 nop
	move	a0, zero
	bal	s1_put_byte
	 nop

	/*
	 * The EN751221's DRAM bridge corrupts direct uncached byte writes, but
	 * it also does not preserve the data written by our cached 32-bit store
	 * packing. The proven chainloader path uses cached sb stores: the D-cache
	 * combines them into complete cache lines, which the flush before the
	 * U-Boot handoff then writes back atomically enough for this controller.
	 *
	 * No UART (or any other slow MMIO) access is allowed inside this loop:
	 * an earlier version printed a one-shot breadcrumb from inside the very
	 * first iteration (right after the first byte landed in DRAM) and the
	 * NAND READ CACHE session hung on the very next byte, every time,
	 * reproducibly. A UART character at 115200 baud takes ~87us to clock
	 * out -- multiple orders of magnitude longer than the gap between two
	 * back-to-back SFC manual-mode strobes -- which is long enough for this
	 * chip's cache-read burst to idle out and drop CS/the read pointer.
	 * Diagnostics for this loop must be gathered by checksumming the
	 * copied range from DRAM *after* the whole copy returns (see the
	 * s1_csum_print calls after this s1_read_range call), never mid-loop.
	 *
	 * Keep s2 intact as the actual page chunk length for the cursor update
	 * below; t6 is the per-byte copy counter.
	 */
	move	t6, s2
range_copy_byte:
	li	a0, (SF_OP_INS << SF_OP_SHIFT) | 1
	bal	s1_sf_op
	 nop
	bal	s1_get_byte
	 nop
	sb	v0, 0(s6)
	addiu	s6, s6, 1
	addiu	t6, t6, -1
	bnez	t6, range_copy_byte
	 nop

	bal	s1_finish
	 nop

	addu	s5, s5, s2		/* advance by the real (unpadded) chunk */
	subu	s4, s4, s2

	bnez	s4, read_range_loop
	 nop

read_range_done:
	jr	s7
	 nop

	/* Never let stage0+stage1 escape their two NAND pages. */
	.org	0x1000
#else
#ifndef CONFIG_MIPS_SKIP_CP0_COUNT_CLEAR
	mtc0	zero, CP0_COUNT
#endif
	/* Disable interrupts and force KSEG0 uncached before DRAM exists. */
	mfc0	t0, CP0_STATUS
	li	t1, ~(ST0_IE | ST0_KSU)
	and	t0, t0, t1
	li	t1, ST0_BEV | ST0_ERL
	or	t0, t0, t1
	mtc0	t0, CP0_STATUS
	mtc0	zero, CP0_CAUSE
	mtc0	zero, CP0_COMPARE

	mfc0	t0, CP0_CONFIG
	li	t1, ~7
	and	t0, t0, t1
	ori	t0, t0, 2
	mtc0	t0, CP0_CONFIG
	ehb

	/* Arbiter and static-memory mappings shared by EN751221 and EN7580. */
	li	t0, ECONET_SYSCTL_BASE
	li	t1, 0x80071f1e
	sw	t1, 0x20(t0)
	li	t1, 0x00071f1f
	sw	t1, 0x24(t0)
	li	t1, 0x80050000
	sw	t1, 0x34(t0)

	li	t0, ECONET_SMC_BASE
	li	t1, 0x102d1040
	sw	t1, 0x00(t0)
	li	t1, 0x200028d0
	sw	t1, 0x14(t0)

	/* Disable the legacy interrupt controller. */
	li	t0, ECONET_INTC_BASE
	sw	zero, 0x00(t0)
	sw	zero, 0x04(t0)

	/* Enable PBUS access to the FE SRAM window. */
	li	t0, ECONET_FE_SRAM_SEL
#ifdef CONFIG_TARGET_EN7528
	/*
	 * Vendor start.o clears 0xbfb00954 before enabling the FE SRAM
	 * selector at 0xbfb00958.  Do this before touching 0x9fa3xxxx.
	 */
	sw	zero, -4(t0)
	sync
#endif
	lw	t1, 0(t0)
	ori	t1, t1, 1
	sw	t1, 0(t0)
	sync

	li	sp, ECONET_TPL_STACK_ADDR
	li	fp, ECONET_TPL_STACK_ADDR

	la	t9, tpl_main
	jalr	t9
	 nop

1:	b	1b
	 nop
#endif
END(_start)

LEAF(econet_run_ddr_blob)
	addiu	sp, sp, -48
	sw	ra, 0(sp)
	sw	gp, 4(sp)
	sw	fp, 8(sp)
	sw	s0, 12(sp)
	sw	s1, 16(sp)
	sw	s2, 20(sp)
	sw	s3, 24(sp)
	sw	s4, 28(sp)
	sw	s5, 32(sp)
	sw	s6, 36(sp)
	sw	s7, 40(sp)
	li	t0, ECONET_TPL_SAVED_SP
	sw	sp, 0(t0)

	la	t1, 1f
	li	t0, ECONET_SCREG_WR0
	sw	t1, 0(t0)
	sync

	li	t9, ECONET_DDR_BLOB_ENTRY
	jr	t9
	 nop
1:
	li	t0, ECONET_SCREG_WR0
	sw	zero, 0(t0)
	li	t0, ECONET_TPL_SAVED_SP
	lw	sp, 0(t0)
	lw	ra, 0(sp)
	lw	gp, 4(sp)
	lw	fp, 8(sp)
	lw	s0, 12(sp)
	lw	s1, 16(sp)
	lw	s2, 20(sp)
	lw	s3, 24(sp)
	lw	s4, 28(sp)
	lw	s5, 32(sp)
	lw	s6, 36(sp)
	lw	s7, 40(sp)
	jr	ra
	 addiu	sp, sp, 48
END(econet_run_ddr_blob)
