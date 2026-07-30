/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
	. = 0x81000000;

	.text : {
		*(.text.start)
		*(.text*)
		*(.rodata*)
	}

	/DISCARD/ : {
		*(.comment)
		*(.gnu.attributes)
		*(.MIPS.abiflags)
		*(.note*)
		*(.reginfo)
	}
}
