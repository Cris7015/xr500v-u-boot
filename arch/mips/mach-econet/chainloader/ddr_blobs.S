/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Payloads the DRAM-resident chainloader copies into FE SRAM when built
 * with CONFIG_ECONET_BOOTROM_CHAINLOADER_DDR: the chainloader linked for
 * 0x9fa30800 and the EN751221 V1.2.2 DDR stage linked for 0x9fa32800.
 * Both lie inside the image checked by self_check().
 */
	.section .rodata.blobs, "a"
	.align	4
	.globl	sram_stage_start, sram_stage_end, ddr_stage_start, ddr_stage_end
sram_stage_start:
	.incbin	SRAM_STAGE_BIN
	.align	2
sram_stage_end:
	.align	4
ddr_stage_start:
	.incbin	DDR_STAGE_BIN
	.align	2
ddr_stage_end:
