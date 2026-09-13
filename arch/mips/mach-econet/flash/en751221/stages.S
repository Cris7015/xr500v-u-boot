/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Empty ELF carrier.
 * move_data and boot2 are built from source by build-econet-flash.sh
 * and added to this object after being linked at their SRAM VMAs.
 */

.section .note.GNU-stack, "", @progbits
