/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * FE SRAM copy of the chainloader (-DSRAM_STAGE), run from boot2's slot
 * after the DRAM copy has checked the whole download. It must end below
 * its stack, which sits under the DDR stage at 0x9fa32800.
 */
OUTPUT_ARCH(mips)
ENTRY(_start)
SECTIONS
{
	. = 0x9fa30800;
	__image_start = .;
	.text : ALIGN(4) { KEEP(*(.text.start)) *(.text .text.*) }
	.rodata : ALIGN(4) { *(.rodata .rodata.*) }
	.data : ALIGN(4) { *(.data .data.*) }
	.got : ALIGN(4) { *(.got .got.*) }
	__image_load_end = .;
	.bss (NOLOAD) : ALIGN(16) { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(16); __bss_end = .; }
	__image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.gnu.attributes) }
}
ASSERT(__image_end <= 0x9fa32800 - 0x400, "FE SRAM chainloader overlaps its stack or the DDR stage")
