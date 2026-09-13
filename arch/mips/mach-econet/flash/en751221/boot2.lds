/* SPDX-License-Identifier: GPL-2.0-or-later */
OUTPUT_ARCH(mips)
ENTRY(boot2_entry)

SECTIONS
{
	/*
	 * boot2 executes from FE SRAM at the vendor SDK address.
	 * The DDR calibration payload is copied to 0x9fa32800, therefore
	 * boot2 must stay entirely below that address.
	 */
	. = 0x9fa30800;

	.text : {
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata.*)
	}

	__stage_end = .;

	ASSERT(__stage_end <= 0x9fa32800,
	       "EN751221 boot2 overlaps DDR calibration SRAM window")

	/DISCARD/ : {
		*(.reginfo)
		*(.MIPS.abiflags)
		*(.pdr)
		*(.comment)
		*(.eh_frame)
		*(.note*)
	}
}
