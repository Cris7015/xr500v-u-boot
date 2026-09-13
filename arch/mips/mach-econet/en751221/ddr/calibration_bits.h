/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bit meanings corroborated by MT6739's legacy dramc drivers and emi_hw.h.
 * Values are independently checked against the EN7512 instructions.
 * See ../MEDIATEK_REFERENCE.md for source revision and platform differences.
 */
#ifndef RECOVERED_CALIBRATION_BITS_H
#define RECOVERED_CALIBRATION_BITS_H

#define DRAMC_TA1_ENABLE		(1U << 29)
#define DRAMC_TA2_READ_ENABLE	(1U << 30)
#define DRAMC_TA2_WRITE_ENABLE	(1U << 31)
#define DRAMC_TA2_RW_ENABLE		(DRAMC_TA2_READ_ENABLE | DRAMC_TA2_WRITE_ENABLE)
#define DRAMC_TEST_DM_CMP_CPT	(1U << 10)
#define DRAMC_TEST_DM_CMP_ERR	(1U << 14)

#endif
