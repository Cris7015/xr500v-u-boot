/* SPDX-License-Identifier: GPL-2.0-or-later */
OUTPUT_ARCH(mips)
ENTRY(move_data_entry)

SECTIONS
{
	/*
	 * start.S copies this image to FE SRAM and enters it through the
	 * uncached 0xbfa30040 alias.  Resolve all internal addresses for
	 * the corresponding cached mapping, as done by the vendor SDK.
	 */
	. = 0x9fa30040;

	.text : {
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata.*)
	}

	__stage_end = .;

	ASSERT(__stage_end <= 0x9fa30800,
	       "EN751221 move_data overlaps boot2 SRAM window")

	/DISCARD/ : {
		*(.reginfo)
		*(.MIPS.abiflags)
		*(.pdr)
		*(.comment)
		*(.eh_frame)
		*(.note*)
	}
}
