/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EN751221 second FE-SRAM stage.
 *
 * Source-level port of the active EN751221 cold-boot path from the
 * EcoNet SDK en7512_boot/boot2/init/boot2.S.
 *
 * FPGA, debug and destructive cache-test paths from the vendor source
 * are intentionally omitted.
 */

#include <asm/regdef.h>

#define CR_ARB_BASE			0xbfb00000
#define CR_SMC_BASE			0xbfb10000
#define CR_INTC_BASE			0xbfb40000
#define SCREG_WR0			0x280

#define SHARE_FEMEM_SEL			0xbfb00958

#define SRAM_DESC_ADDR			0xbfa30000
#define MOVE_DATA_CACHED		0x9fa30040
#define MOVE_DATA_UNCACHED		0xbfa30040
#define DDR_STAGE_CACHED		0x9fa32800
#define DDR_STAGE_UNCACHED		0xbfa32800

#define ICACHE_INDEX_STORE_TAG		8
#define DCACHE_INDEX_STORE_TAG		9
#define ICACHE_ADDR_HIT_INVALIDATE	16
#define DCACHE_ADDR_HIT_WRITEBACK	25

#define CONFIG1_IS_SHIFT		22
#define CONFIG1_IS_MASK			(0x7 << CONFIG1_IS_SHIFT)
#define CONFIG1_IL_SHIFT		19
#define CONFIG1_IL_MASK			(0x7 << CONFIG1_IL_SHIFT)
#define CONFIG1_IA_SHIFT		16
#define CONFIG1_IA_MASK			(0x7 << CONFIG1_IA_SHIFT)

#define CONFIG1_DS_SHIFT		13
#define CONFIG1_DS_MASK			(0x7 << CONFIG1_DS_SHIFT)
#define CONFIG1_DL_SHIFT		10
#define CONFIG1_DL_MASK			(0x7 << CONFIG1_DL_SHIFT)
#define CONFIG1_DA_SHIFT		7
#define CONFIG1_DA_MASK			(0x7 << CONFIG1_DA_SHIFT)

/*
 * Keep the hazard sequence used by the old EcoNet MTC0_SEL_OPCODE
 * implementation.
 */
#define CP0_HAZARD			\
	sll zero, zero, 1;		\
	sll zero, zero, 1;		\
	sll zero, zero, 1;		\
	sll zero, zero, 3

	.section .text, "ax", @progbits
	.set	noreorder

	.globl	boot2_entry
boot2_entry:
	/*
	 * boot2 is initially entered through the bfa30800 uncached alias.
	 * J-format calls therefore remain in the same physical FE SRAM
	 * while sys_init_cache invalidates L1.
	 */
	jal	sys_init_cache
	nop

	/* Allow PBUS access to FE memory. */
	li	v0, SHARE_FEMEM_SEL
	lw	t6, 0(v0)
	nop
	ori	t6, t6, 1
	sw	t6, 0(v0)
	nop

	/*
	 * Copy the DDR calibration image from flash to FE SRAM using the
	 * already resident move_data stage.
	 */
	li	t1, SRAM_DESC_ADDR
	lw	s1, 0x08(t1)
	nop
	lw	s2, 0x0c(t1)
	nop

	li	s3, DDR_STAGE_UNCACHED
	la	s4, move_ddr_calibration

	li	t1, MOVE_DATA_CACHED
	jr	t1
	nop

move_ddr_calibration:
	nop

	/*
	 * The calibration code returns through SCREG_WR0, matching the
	 * vendor DDR calibration ABI.
	 */
	la	k1, ddr_calibration_done
	li	k0, CR_ARB_BASE
	sw	k1, SCREG_WR0(k0)
	nop

	li	k0, DDR_STAGE_CACHED
	jr	k0
	nop

ddr_calibration_done:
	move	k1, zero
	li	k0, CR_ARB_BASE
	sw	k1, SCREG_WR0(k0)
	nop

#ifndef TCSUPPORT_SLM_EN
	/* Vendor boot2 bypasses SLM on this configuration. */
	li	k0, 0xbfa60000
	lw	t7, 0(k0)
	nop
	ori	t7, t7, 1
	sw	t7, 0(k0)
	nop
#endif

	/* Arbiter configuration preserved from the EN7512 vendor stage. */
	li	v1, CR_ARB_BASE

	li	a0, 0x80071f1e
	li	a1, 0x00071f1f
	li	a2, 0x00040000

	sw	a0, 0x20(v1)
	sw	a1, 0x24(v1)
	nop

	/* SRAM rank 3/4/5. */
	li	a0, 0x00040808
	li	a1, 0xc0041010
	li	a2, 0x80050000

	sw	a2, 0x34(v1)
	nop

	/* SMC. */
	li	v1, CR_SMC_BASE

	li	a0, 0x102d1040
	li	a2, 0x10aa8100
	li	a3, 0x200028d0

	sw	a0, 0x00(v1)
	sw	a3, 0x14(v1)
	nop

	/* Disable all legacy interrupt routing/masks. */
	li	t0, CR_INTC_BASE
	sw	zero, 0x00(t0)
	sw	zero, 0x04(t0)
	nop

	/*
	 * Preserve the two legacy boot interval words expected by the old
	 * loader ABI.
	 */
	li	t1, SRAM_DESC_ADDR

	lw	t2, 0x18(t1)
	nop
	li	t3, 0x80010000
	sw	t2, 0(t3)
	nop

	lw	t2, 0x1c(t1)
	nop
	li	t3, 0x80010020
	sw	t2, 0(t3)
	nop

	/*
	 * Descriptor +0x10/+0x14 is the .lzma interval in the legacy ABI.
	 * In the new image it contains our small uncompressed flash loader.
	 */
	lw	s1, 0x10(t1)
	nop
	lw	s2, 0x14(t1)
	nop

	li	s3, 0x80000000
	la	s4, loading_bootcode_done

	li	k0, MOVE_DATA_UNCACHED
	jr	k0
	nop

loading_bootcode_done:
	nop

	/*
	 * The copied loader will execute from cached KSEG0.  Write the data
	 * back and invalidate the corresponding I-cache range exactly as
	 * the vendor boot2 does.
	 */
	li	t1, 0x80000000
	li	t2, 0x80200000
1:
	cache	DCACHE_ADDR_HIT_WRITEBACK, 0(t1)
	sync
	cache	ICACHE_ADDR_HIT_INVALIDATE, 0(t1)

	addiu	t1, t1, 32
	bne	t2, t1, 1b
	nop

	li	t0, 0xf200ff15
	mtc0	t0, $12
	nop

	li	t2, 0x80000000
	jr	t2
	nop

/*
 * L1 initialization.
 *
 * Config1:
 *   IS/DS = sets per way
 *   IL/DL = line-size encoding
 *   IA/DA = associativity - 1
 */
sys_init_cache:
	move	t4, ra

	jal	sys_determine_icache_linesize
	nop
	move	t2, v0

	jal	sys_determine_icache_lines
	nop
	multu	t2, v0
	mflo	t3

	jal	sys_determine_dcache_linesize
	nop
	move	t0, v0

	jal	sys_determine_dcache_lines
	nop
	multu	t0, v0
	mflo	t1

	move	a0, t3
	move	a1, t2
	jal	sys_init_icache
	nop

	move	a0, t1
	move	a1, t0
	jal	sys_init_dcache
	nop

	jr	t4
	move	v0, zero

sys_init_icache:
	/* Clear ITagLo. */
	mtc0	zero, $28, 0
	CP0_HAZARD

	li	a2, 0x80000000
	addu	a3, a2, a0
	subu	a3, a3, a1
1:
	cache	ICACHE_INDEX_STORE_TAG, 0(a2)
	bne	a2, a3, 1b
	addu	a2, a2, a1

	jr	ra
	nop

sys_init_dcache:
	/* Clear DTagLo. */
	mtc0	zero, $28, 2
	CP0_HAZARD

	li	a2, 0x80000000
	addu	a3, a2, a0
	subu	a3, a3, a1
1:
	cache	DCACHE_INDEX_STORE_TAG, 0(a2)
	bne	a2, a3, 1b
	addu	a2, a2, a1

	jr	ra
	nop

sys_determine_icache_linesize:
	mfc0	t9, $16, 1

	li	t8, CONFIG1_IL_MASK
	and	t8, t8, t9
	srl	t8, t8, CONFIG1_IL_SHIFT

	li	t7, 2
	sllv	v0, t7, t8

	jr	ra
	nop

sys_determine_icache_lines:
	mfc0	t9, $16, 1

	li	t8, CONFIG1_IA_MASK
	and	t8, t8, t9
	srl	t8, t8, CONFIG1_IA_SHIFT
	addiu	t8, t8, 1

	li	t7, CONFIG1_IS_MASK
	and	t7, t7, t9
	srl	t7, t7, CONFIG1_IS_SHIFT

	li	t9, 0x40
	sllv	t7, t9, t7

	multu	t8, t7
	mflo	v0

	jr	ra
	nop

sys_determine_dcache_linesize:
	mfc0	t9, $16, 1

	li	t8, CONFIG1_DL_MASK
	and	t8, t8, t9
	srl	t8, t8, CONFIG1_DL_SHIFT

	li	t7, 2
	sllv	v0, t7, t8

	jr	ra
	nop

sys_determine_dcache_lines:
	mfc0	t9, $16, 1

	li	t8, CONFIG1_DA_MASK
	and	t8, t8, t9
	srl	t8, t8, CONFIG1_DA_SHIFT
	addiu	t8, t8, 1

	li	t7, CONFIG1_DS_MASK
	and	t7, t7, t9
	srl	t7, t7, CONFIG1_DS_SHIFT

	li	t9, 0x40
	sllv	t7, t9, t7

	multu	t8, t7
	mflo	v0

	jr	ra
	nop

