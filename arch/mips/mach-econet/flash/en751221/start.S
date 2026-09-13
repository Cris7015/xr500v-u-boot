/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EN751221 bootrom reset vector, copied from the vendor SDK's
 * en7512_boot/start.S_7512_final verbatim (byte-for-byte text, including
 * its dead/never-stored register loads -- e.g. the SRAM-rank-3/4/5 and
 * SMC-bank1/4 setup whose sw's are commented out but whose li's are not)
 * instead of the raw .4byte-encoded disassembly this file used to carry.
 * Preserving even the seemingly-dead instructions matters: this is a
 * literal transcription, not a rewrite, and the previous .4byte
 * reconstruction (taken from objdump of the real shipped bootrom object)
 * confirms the real chip's ROM does contain every one of them.
 *
 * Two adjustments were needed to make the vendor text assemble against
 * modern U-Boot's asm/mipsregs.h instead of the legacy EcoNet one it was
 * written against:
 *
 *  1. A handful of CP0 Status/Cause/Config macro names changed between
 *     the two headers (S_StatusNMI/M_StatusIE/M_StatusKSU/M_CauseIV/
 *     C0_Config/C0_Status): see the #define block below, which maps each
 *     old name onto its modern equivalent. These bits are standard
 *     MIPS32 CP0 fields, not EcoNet-specific, so the mapping is exact.
 *
 *  2. sys_init_processor's only definition in the vendor file lives
 *     inside "#ifdef TC3262", which also needs several more undefined
 *     legacy macros (S_ConfigK23, S_ConfigKU, M_StatusBEV, M_StatusFR,
 *     S_ConfigK0) with no confirmed modern equivalent. Rather than guess
 *     at CP0 Config/Status bit setup that runs once at reset before any
 *     UART or debug output exists, sys_init_processor below is kept as
 *     the exact raw instruction words captured from objdump of the real
 *     shipped bootrom object -- unchanged from the previous .4byte
 *     reconstruction of this file, which was verified against real
 *     hardware.
 *
 * A third, smaller discrepancy: the archived vendor text is missing one
 * nop (between the NMI handler's "lw k1, 0x0(k0)" and "jr k1") that the
 * real shipped object has. Added back below with a comment at that exact
 * spot, since the real dumped ROM is the higher-fidelity source whenever
 * the two disagree.
 *
 * EN751221 is big endian. build-econet-flash.sh must assemble this file
 * with -EB.
 */

#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/mipsregs.h>

#define S_StatusNMI	19
#define M_StatusIE	ST0_IE
#define M_StatusKSU	ST0_KSU
#define M_CauseIV	CAUSEF_IV
#define C0_Config	CP0_CONFIG
#define C0_Status	CP0_STATUS

#define	CR_ARB_BASE			0xBFB00000
#define	CR_TIMER_BASE			0xBFBF0100
#define	GPIOBASE			0xBFBF0200
#define	CR_SMC_BASE			0xBFB10000
#define	CR_DMC_BASE			0xBFB20000
#define CR_CHIP_SCU_BASE	0xBFA20000  //YMC_20131127
#define CR_AFE_BASE			0xBF900000

#define PAGE_SIZE_BIT_SRAM_ADDR	0XBFA30020

#define UNCACHE_MODE
//#define DEBUG 1
					 
        .text
        .set noreorder
		.globl __start              
__start:
		j	boot_start
		nop

/*
 * This board's real dumped tcboot header is the smaller, 16-word (64
 * byte) revision -- boot_start at +0x48, patched in by
 * econet_flash_image.py at offsets 0x10/0x14/0x18/0x1c (matching this
 * file's own "for lzma_start/end/boot_start/boot_end address" comments)
 * -- not the larger 22-word revision the rest of this archived vendor
 * text describes (which reserves +0x20..+0x5c for en7526c-only backdoor/
 * bypass fields and a *second*, later set of move_data/boot2/spram
 * addresses). Truncated to match the real, hardware-verified header;
 * econet_flash_image.py's pack() asserts word 0 is "j 0xbfc00048" for
 * en751221, which only holds with this shorter header.
 */
header_start:
		nop		// 0x08
		nop		// 0x0c
		nop		// 0x10, for lzma_start address
		nop		// 0x14, for lzma_end address
		nop		// 0x18, for boot_start address
		nop		// 0x1c, for boot_end	address
		nop		// 0x20
		nop		// 0x24
		nop		// 0x28
		nop		// 0x2c
		nop		// 0x30
		nop		// 0x34
		nop		// 0x38
		nop		// 0x3c
		nop		// 0x40
		nop		// 0x44

boot_start:
#ifdef DEBUG

		jal sys_uart_init
                nop


                li t7, 'x'
                jal disp_character
                nop
#endif
#ifndef DDR_CALI_NAND

		/* Clear watch registers */
		mtc0	zero, CP0_WATCHLO
		mtc0	zero, CP0_WATCHHI

		/* Check whether NMI happen */
		mfc0   	k0, CP0_STATUS
		srl		k1, k0, (S_StatusNMI)   
		andi		k1, 0x1
		beqz		k1, no_nmi
		nop

		la		k0,	0xbfb00244	/*NMI happen, read NMI handler address, then jump to it*/
		lw		k1, 0x0(k0)
		/*
		 * Load-delay safety nop present in the real shipped bootrom object
		 * but missing from the archived vendor text (see file header).
		 */
		nop
		jr		k1
		nop
		
no_nmi:
		/* Disable interrupts and KSU field (power up issue) */
		mfc0   	k0, CP0_STATUS
		li		k1, ~(M_StatusIE | M_StatusKSU)
		and		k0, k1
		mtc0	k0, CP0_STATUS

		/* Setup cause register (clear cause code)  */
		li		k0, M_CauseIV
		mtc0	k0, CP0_CAUSE
#ifdef DEBUG
		li t7, 'a'
                jal disp_character
                nop

#endif


		/* Perform CPU specific initialisation */
		jal     sys_init_processor
		nop
#ifdef DEBUG
                li t7, 'c'
                jal disp_character
                nop

#endif


		/* configure arbiter */
	    la	v1,	CR_ARB_BASE

		/* configure SMC */
		/* SRAM address: */
		/*	1. FLASH	 16bit begin at 0x1fc00000 to 0x1fffffff  size:	  4MB */
		/*	2. SRAM    	 8bit begin at 0x1fe00000 to 0x1fe3ffff	size: 256KB   */
		li	a0,	0x80071f1e
		li	a1,	0x00071f1f
		li	a2,	0x00040000

		sw	a0,	0x20(v1)
		sw	a1,	0x24(v1)
		nop

// SRAM rank 3,4,5
		li	a0,	0x00040808
		li	a1,	0xc0041010	// with bridge				
		li	a2,	0x80050000

		sw	a2,	0x34(v1)
		nop

    	li	v1,	CR_SMC_BASE

		li	a0, 0x102d1040    		

 		li	a2,	0x10aa8100				/* 16-bit hpi8  configure  */
 		li  a3,	0x200028d0				/* 32-bit dmt   configure  */

		sw	a0,	0x0(v1)      			/* CR_SMC_BANK0     */
//		sw	a1,	0x4(v1)      			/* CR_SMC_BANK1     */
// 		sw	a2,	0x10(v1)     			/* CR_SMC_BANK4     */
 		sw	a3,	0x14(v1)     			/* CR_SMC_BANK5     */
		nop

		/* stop interrupt */
		li		t0, 	0xbfb40000
		nop
		sw		zero, 	0x0(t0)			/* all interrupt belong to IRQ */
		sw		zero, 	0x04(t0)		/* set all interrupt mask to 0 .. disable all interrupt */
		nop

#ifdef DEBUG
                li t7, 'e'
                jal disp_character
                nop

#endif

#define	SHARE_FEMEM_SEL	0xBFB00958
#define	SHARE_FEMEM_PA	0x1FA30000
#define	SHARE_FEMEM_VA	0x9FA30000
#define BOOTROM_START_ADDR	0xBFA30040
#define BOOTROM_END_ADDR	0xBFA3C020
#define SRAM_START_ADDR	  0xBFA30000

		//Enable pbus access FE memory
		la      v0, SHARE_FEMEM_SEL
		lw      t6, 0x0(v0)
		nop
		ori	t6, t6, 0x1
		sw	t6, 0x0(v0)
		nop

		la  k0, __move_data_start
		la  k1, (__move_data_end + 4)
		la  t1, BOOTROM_START_ADDR

/*
 * The archived vendor text copies each word without checking it landed
 * correctly. The real shipped bootrom object (previous .4byte
 * reconstruction, verified on real hardware) reads back and compares
 * every word it just wrote, hanging in ram_error on mismatch instead of
 * silently running move_data from a corrupted copy -- kept here for the
 * same reason as the NMI load-delay nop above (see file header).
 */
1:
		lw	t0, 0(k0)
		nop
		sw	t0, 0(t1)
		nop
		lw	t5, 0(t1)
		nop
		bne	t0, t5, ram_error
		nop

		addiu k0, k0, 4
		addiu t1, t1, 4
		bne	k1, k0, 1b
		nop
		nop
		nop
		b	ram_pass
		nop
ram_error:
		b	ram_error
		nop
ram_pass:
		nop

#ifdef DEBUG
		li t7, 'f'
		jal disp_character
		nop
#endif

		la      t1, SRAM_START_ADDR
		
		la		t2, __boot2_start
		sw		t2, 0x0(t1)
		la		t2, __boot2_end
		sw		t2, 0x4(t1)
		la      t2, __spram_start
		sw		t2, 0x8(t1)
		la      t2, __spram_end
		sw		t2, 0xc(t1)

		la      t2, __lzma_start
		sw		t2, 0x10(t1)
		la      t2, __lzma_end
		sw		t2, 0x14(t1)
		la      t2, __boot_start
		sw		t2, 0x18(t1)
		la      t2, __boot_end
		sw		t2, 0x1c(t1)

		/*
		 * The archived vendor text also clears PAGE_SIZE_BIT_SRAM_ADDR
		 * here ("for auto-sizing"), but the real shipped bootrom object
		 * (previous .4byte reconstruction, verified on real hardware)
		 * does not touch it -- omitted to match, see file header.
		 */

		la		s1, __boot2_start
		la		s2, __boot2_end
		la		s3, 0xbfa30800
		la		s4, 0xbfa30800

  	la	k0, BOOTROM_START_ADDR
		jr	k0
		nop
	/*=================================start.S End===========================*/
error:
	/*  Error handling. Display error code (if device for this is available) and
	 *  enter an infinite loop (from init.h)
	 */
		b error
		nop
#endif /* DDR_CALI_NAND */

/*
 * sys_init_processor: verbatim raw instruction words captured from the
 * real shipped EN751221 bootrom object (objdump), not recompiled from
 * the vendor's TC3262-gated C macros -- see the file header comment.
 * Sets up MIPS34K-specific Config0/Status fields (cache attribute, BEV)
 * once at reset, before any UART is available to verify it went right.
 */
sys_init_processor:
	.4byte	0x40088000
	.4byte	0x3c098000
	.4byte	0x3529ffff
	.4byte	0x01094024
	.4byte	0x3c093604
	.4byte	0x01094025
	.4byte	0x40888000

	.4byte	0x40086000
	.4byte	0x3c09fffc
	.4byte	0x3529ffff
	.4byte	0x01094024
	.4byte	0x24090000
	.4byte	0x01094025
	.4byte	0x40886000

	.4byte	0x40086000
	.4byte	0x3c090003
	.4byte	0x01094024
	.4byte	0x3c090440
	.4byte	0x01094025
	.4byte	0x40886000

	.4byte	0x40088000
	.4byte	0x3c097fff
	.4byte	0x01094024
	.4byte	0x24090003
	.4byte	0x01094025
	.4byte	0x40888000

	.4byte	0x03e00008
	.4byte	0x00001021
