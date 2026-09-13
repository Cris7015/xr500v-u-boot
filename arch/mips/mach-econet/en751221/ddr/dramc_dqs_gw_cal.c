/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovered from dramc_dqs_gw_cal.o; see ../README.md for fidelity status. */
#include "en751221_ddr.h"
#include "calibration_bits.h"

extern void pause_polling(unsigned int count);

static int dqs_gw_coarse = 0;
static int dqs_gw_fine = 0;
static int dqs_gw_fine_cnt = 0;
static int score = 0;
unsigned int dqs_gw[DQS_GW_LEN];
int opt_gw_coarse_value;
int opt_gw_fine_value;

int dramc_ta2(unsigned int start, unsigned int len, void *ext_arg)
{
	unsigned int count = 0x7fffffff;
	unsigned int result;
	DRAMC_WRITE_REG(((start >> 8) & 0x1fffff) | 0x55000000, DRAMC_TEST2_1);
	DRAMC_WRITE_REG((len & 0xffffff) | 0xaa000000, DRAMC_TEST2_2);
	DRAMC_WRITE_SET(DRAMC_TA2_READ_ENABLE, DRAMC_CONF2);
	while (!(DRAMC_READ_REG(DRAMC_TESTRPT) & DRAMC_TEST_DM_CMP_CPT)) {
		if (--count == 0)
			break;
	}
	delay_a_while(1);
	result = DRAMC_READ_REG(DRAMC_DQSGNWCNT0);
	DRAMC_WRITE_CLEAR(DRAMC_TA2_READ_ENABLE, DRAMC_CONF2);
	return result == HW_DQS_GW_COUNTER ? 0 : -1;
}

void dqsi_gw_dly_coarse_factor_handler(unsigned int value)
{
	/* DQSINCTL[26:24] plus R0DQSG_COARSE_DLY_COM0/1[1:0,5:4].
	 * The EN7512 object uses three DQSINCTL bits; the newer MT6739 header
	 * describes four. Preserve this binary's mask. See MEDIATEK_REFERENCE.md.
	 */
	DRAMC_WRITE_REG((((value >> 2) & 7) << 24) |
				(DRAMC_READ_REG(DRAMC_DQSCTL1) & ~0x07000000),
			DRAMC_DQSCTL1);
	DRAMC_WRITE_REG((DRAMC_READ_REG(DRAMC_DQSGCTL) & ~0x33) | (value & 3) |
				((value & 3) << 4),
			DRAMC_DQSGCTL);
}

void dqsi_gw_dly_fine_factor_handler(unsigned int value)
{
	/* Replicate the fine delay into the four seven-bit DQSxIEN fields. */
	DRAMC_WRITE_REG((DRAMC_READ_REG(DRAMC_R0DQSIEN) & 0x80000000) |
				(value & 0x7f) | ((value & 0x7f) << 8) |
				((value & 0x7f) << 16) | ((value & 0x7f) << 24),
			DRAMC_R0DQSIEN);
}

void dqs_gw_counter_reset(void)
{
	DRAMC_WRITE_SET(0x200, DRAMC_SPCMD);
	DRAMC_WRITE_CLEAR(0x200, DRAMC_SPCMD);
	DDR_PHY_RESET_NEW();
}

test_case dqs_gw_test_cases_1[] = { { dramc_ta2, DRAM_START, 0xff,
				      (void *)-1 } };

int dqs_gw_calib(void)
{
	int coarse, fine, pass;
	for (coarse = DQS_GW_COARSE_START; coarse <= DQS_GW_COARSE_END;
	     coarse++) {
		dqsi_gw_dly_coarse_factor_handler(coarse);
		for (fine = DQS_GW_FINE_START; fine <= DQS_GW_FINE_END;
		     fine++) {
			dqsi_gw_dly_fine_factor_handler(fine);
			dqs_gw_counter_reset();
			dqs_gw_test_cases_1[0].start = DRAM_START;
			if (dqs_gw_test_cases_1[0].test_case(
				    dqs_gw_test_cases_1[0].start,
				    dqs_gw_test_cases_1[0].range,
				    dqs_gw_test_cases_1[0].ext_arg) >= 0)
				goto passed;
			pass = 0;
resume:;
			/* Empty constraints preserve the observed temporary registers. */
			register int f __asm__("$3") = ++dqs_gw_fine;
			__asm__ volatile("" : "+r"(f));
			dqs_gw_fine_cnt++;
			if (f >= DQS_GW_LEN_PER_COARSE_ELEMENT ||
				dqs_gw_fine_cnt >= DQS_GW_FINE_MAX) {
				dqs_gw_coarse++;
				dqs_gw_fine = 0;
				if (dqs_gw_fine_cnt >= DQS_GW_FINE_MAX)
					dqs_gw_fine_cnt = 0;
			}
			register int sum __asm__("$2") = pass + score;
			__asm__ volatile("" : "+r"(sum) : "r"(pass));
			score = sum;
		}
		dqs_gw_fine = 0;
		dqs_gw_fine_cnt = 0;
	}
	/* Keep the passing path after the scan to preserve the branch layout. */
	goto finish;
passed:
	dqs_gw[dqs_gw_coarse] |= 1 << dqs_gw_fine;
	pass = 1;
	goto resume;
finish:
	dqs_gw_coarse = 0;
	dqs_gw_counter_reset();
	return 1;
}

static inline int set_bits_count(unsigned int word)
{
	int bit, count = 0;
	for (bit = 0; bit < 32; bit++)
		if (word & (1 << bit))
			count++;
	return count;
}

static inline int first_set_bit(unsigned int word)
{
	int bit;
	for (bit = 0; bit < 32; bit++)
		if (word & (1 << bit))
			return bit;
	return -1;
}

int do_dqs_gw_calib_1(void)
{
	int best_coarse, best_fine, bit, result;
	int i, j, count, max_count = 0;
	dqs_gw_coarse = 0;
	dqs_gw_fine = 0;
	dqs_gw_fine_cnt = 0;
	for (i = 0; i < DQS_GW_LEN; i++)
		dqs_gw[i] = 0;
	DRAMC_WRITE_CLEAR(0x8000, DRAMC_TEST2_4);
	DRAMC_WRITE_SET(1 << 28, DRAMC_DQSCTL1);
	DRAMC_WRITE_SET(0x100, DRAMC_SPCMD);
	dqs_gw_calib();
	for (i = 0; i < DQS_GW_COARSE_MAX; i++) {
		count = 0;
		for (j = 0; j < DQS_GW_LEN_PER_COARSE; j++)
			count += set_bits_count(
				dqs_gw[i * DQS_GW_LEN_PER_COARSE + j]);
		if (count >= max_count) {
			best_coarse = i;
			max_count = count;
		}
	}
	if (max_count) {
		for (j = 0; j < DQS_GW_LEN_PER_COARSE; j++) {
			bit = first_set_bit(
				dqs_gw[best_coarse * DQS_GW_LEN_PER_COARSE + j]);
			if (bit != -1) {
				best_fine = j * DQS_GW_LEN_PER_COARSE_ELEMENT +
					    bit + max_count / 2;
				break;
			}
		}
		opt_gw_coarse_value = best_coarse;
		opt_gw_fine_value = best_fine;
		dqsi_gw_dly_coarse_factor_handler(opt_gw_coarse_value);
		dqsi_gw_dly_fine_factor_handler(opt_gw_fine_value);
		result = 0;
	} else {
		result = -1;
	}
	DRAMC_WRITE_CLEAR(0x100, DRAMC_SPCMD);
	DRAMC_WRITE_SET(0x8000, DRAMC_TEST2_4);
	return result;
}
