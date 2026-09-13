/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovered from dramc_dq_dqs_cal.o; see ../README.md for byte fidelity.
 * Record layout and algorithm terminology are corroborated by the related
 * MT6739 dramc_calib.c, commit 73d33cc801ec3ab49c886529ca2f19a6b11f2d03.
 * See ../MEDIATEK_REFERENCE.md. This is the EN7512 16-bit/96-tap variant;
 * the related source is not evidence for absent EN7512 DWARF or line numbers.
 */
#include "en751221_ddr.h"
#include "calibration_bits.h"

extern void pause_polling(unsigned int count);

struct rx_perbit_delay {
	signed char min_cur, max_cur, min_best, max_best;
	unsigned char center, dq_dly_last;
};

/* Ten-byte stride agrees with the related TX record. The total_* and final
 * dq/dqs slots have no surviving accesses in the EN7512 object; their names
 * come from that correspondence, not independently recoverable debug types.
 */
struct tx_perbit_delay {
	signed char first_dqdly_pass, last_dqdly_pass, total_dqdly_pass;
	signed char first_dqsdly_pass, last_dqsdly_pass, total_dqsdly_pass;
	unsigned char best_dqdly, best_dqsdly, dq, dqs;
};

unsigned int opt_tx_dqm;
unsigned int opt_tx_dq[2];
unsigned int opt_tx_dqs;

static inline unsigned int rx_test(void)
{
	unsigned int count = 0x7fffffff, errors;
	DRAMC_WRITE_SET(DRAMC_TA2_RW_ENABLE, DRAMC_CONF2);
	while (!(DRAMC_READ_REG(DRAMC_TESTRPT) & DRAMC_TEST_DM_CMP_CPT))
		if (--count == 0)
			break;
	delay_a_while(400);
	errors = DRAMC_READ_REG(DRAMC_CMP_ERR);
	DRAMC_WRITE_CLEAR(DRAMC_TA2_RW_ENABLE, DRAMC_CONF2);
	return errors;
}

int do_sw_rx_dq_dqs_calib(void)
{
	signed char dqs_delay[DQS_NUMBER];
	signed char dq_delay_per_bit[DQ_DATA_WIDTH];
	int dq_delay_done[DQ_DATA_WIDTH];
	struct rx_perbit_delay dqs_perbit_dly[DQ_DATA_WIDTH];
	unsigned int bit, tap, group;
	register unsigned int dq_tap __asm__("$17");
	unsigned int errors, value, maximum;

	DRAMC_WRITE_REG(0, DRAMC_R0DELDLY);
	DRAMC_WRITE_REG(0, DRAMC_DQIDLY1);
	DRAMC_WRITE_REG(0, DRAMC_DQIDLY2);
	DRAMC_WRITE_REG(0, DRAMC_DQIDLY3);
	DRAMC_WRITE_REG(0, DRAMC_DQIDLY4);
	for (bit = 0; bit < DQ_DATA_WIDTH; bit++) {
		dq_delay_per_bit[bit] = 0;
		dq_delay_done[bit] = 0;
	}
	/* Freeze each DQ after its first failure with DQS held at zero. */
	for (dq_tap = 1; dq_tap != MAX_RX_DQDLY_TAPS + 1;) {
		DRAMC_WRITE_REG(0x55000800, DRAMC_TEST2_1);
		DRAMC_WRITE_REG(0xaa000100, DRAMC_TEST2_2);
		errors = rx_test();
		if (errors == 0xffff || dq_tap == MAX_RX_DQDLY_TAPS)
			break;
		for (bit = 0; bit < DQ_DATA_WIDTH; bit++) {
			if (!(errors & (1 << bit)) && !dq_delay_done[bit])
				dq_delay_per_bit[bit] = dq_tap;
			else
				dq_delay_done[bit] = 1;
		}
		for (group = 0; group < DQ_DATA_WIDTH; group += 4) {
			value = dq_delay_per_bit[group] +
				(dq_delay_per_bit[group + 1] << 8) +
				(dq_delay_per_bit[group + 2] << 16) +
				(dq_delay_per_bit[group + 3] << 24);
			DRAMC_WRITE_REG(value, DRAMC_DQIDLY1 + group);
		}
		dq_tap++;
		/* Retain the reference's counter register and post-increment lifetime.
		 * The empty constraint emits no assembly instruction or binary data.
		 */
		__asm__ volatile("" : "+r"(dq_tap));
	}
	for (bit = 0; bit < DQ_DATA_WIDTH; bit++) {
		dqs_perbit_dly[bit].min_cur = -1;
		dqs_perbit_dly[bit].max_cur = -1;
		dqs_perbit_dly[bit].min_best = -1;
		dqs_perbit_dly[bit].max_best = -1;
		dqs_perbit_dly[bit].center = 0;
		dqs_perbit_dly[bit].dq_dly_last = dq_delay_per_bit[bit];
	}
	/* Record every pass interval and retain the widest one for each bit. */
	for (tap = 0; tap < MAX_RX_DQSDLY_TAPS; tap++) {
		DRAMC_WRITE_REG((tap << 8) + (tap << 16) + tap + (tap << 24),
				DRAMC_R0DELDLY);
		DRAMC_WRITE_REG((DRAMC_READ_REG(DRAMC_TEST2_2) & 0xff000000) |
					0x100,
				DRAMC_TEST2_2);
		errors = rx_test();
		for (bit = 0; bit < DQ_DATA_WIDTH; bit++) {
			if (dqs_perbit_dly[bit].min_cur == -1 &&
			    !((errors >> bit) & 1))
				dqs_perbit_dly[bit].min_cur = tap;
			if (dqs_perbit_dly[bit].min_cur != -1 &&
			    dqs_perbit_dly[bit].max_cur == -1 &&
			    (((errors >> bit) & 1) ||
			     tap == MAX_RX_DQSDLY_TAPS - 1)) {
				if (tap == MAX_RX_DQSDLY_TAPS - 1 &&
				    !((errors >> bit) & 1))
					dqs_perbit_dly[bit].max_cur =
						MAX_RX_DQSDLY_TAPS - 1;
				else
					dqs_perbit_dly[bit].max_cur = tap - 1;
				if (dqs_perbit_dly[bit].max_cur -
					    dqs_perbit_dly[bit].min_cur >
				    dqs_perbit_dly[bit].max_best -
					    dqs_perbit_dly[bit].min_best) {
					dqs_perbit_dly[bit].max_best =
						dqs_perbit_dly[bit].max_cur;
					dqs_perbit_dly[bit].min_best =
						dqs_perbit_dly[bit].min_cur;
				}
				dqs_perbit_dly[bit].max_cur = -1;
				dqs_perbit_dly[bit].min_cur = -1;
			}
		}
	}
	for (bit = 0; bit < DQ_DATA_WIDTH; bit++)
		if (dqs_perbit_dly[bit].max_best != -1 &&
		    dqs_perbit_dly[bit].min_best != -1)
			dqs_perbit_dly[bit].center =
				(dqs_perbit_dly[bit].max_best +
				 dqs_perbit_dly[bit].min_best) /
				2;
	/* One DQS serves eight bits: choose the largest per-bit center. */
	for (group = 0; group < DQS_NUMBER; group++) {
		maximum = 0;
		for (bit = 0; bit < DQS_BIT_NUMBER; bit++)
			if (dqs_perbit_dly[group * DQS_BIT_NUMBER + bit].center >
			    maximum)
				maximum =
					dqs_perbit_dly[group * DQS_BIT_NUMBER +
						       bit]
						.center;
		dqs_delay[group] = maximum;
	}
	DRAMC_WRITE_REG((dqs_delay[1] << 8) + dqs_delay[0], DRAMC_R0DELDLY);
	for (group = 0; group < DQ_DATA_WIDTH; group += 4) {
		for (bit = 0; bit < 4; bit++) {
			value = dqs_delay[group / 8] -
				dqs_perbit_dly[group + bit].center +
				dqs_perbit_dly[group + bit].dq_dly_last;
			if (value > MAX_RX_DQDLY_TAPS - 1)
				value = MAX_RX_DQDLY_TAPS - 1;
			dqs_perbit_dly[group + bit].dq_dly_last = value;
		}
		value = (dqs_perbit_dly[group + 1].dq_dly_last << 8) +
			(dqs_perbit_dly[group + 2].dq_dly_last << 16) +
			dqs_perbit_dly[group].dq_dly_last +
			(dqs_perbit_dly[group + 3].dq_dly_last << 24);
		DRAMC_WRITE_REG(value, DRAMC_DQIDLY1 + group);
	}
	return 0;
}

static inline unsigned int tx_test(void)
{
	unsigned int errors, value;
	DRAMC_WRITE_REG(0x55000800, DRAMC_TEST2_1);
	value = DRAMC_READ_REG(DRAMC_TEST2_2) & 0xffffff;
	/* Keep the observed mask before OR; this constraint emits no opcode. */
	__asm__ volatile("" : "+r"(value));
	DRAMC_WRITE_REG(value | 0xaa0003ff, DRAMC_TEST2_2);
	DRAMC_WRITE_SET(DRAMC_TA2_WRITE_ENABLE, DRAMC_CONF2);
	DRAMC_WRITE_SET(DRAMC_TA2_RW_ENABLE, DRAMC_CONF2);
	while (!(DRAMC_READ_REG(DRAMC_TESTRPT) & DRAMC_TEST_DM_CMP_CPT))
		;
	delay_a_while(400);
	errors = DRAMC_READ_REG(DRAMC_CMP_ERR);
	DRAMC_WRITE_CLEAR(DRAMC_TA2_RW_ENABLE, DRAMC_CONF2);
	return errors;
}

static inline unsigned int pack_tx_dq(struct tx_perbit_delay *p)
{
	return (p[0].best_dqdly & 0xf) | (p[7].best_dqdly << 28) |
	       ((p[1].best_dqdly << 4) & 0xff) |
	       ((p[2].best_dqdly & 0xf) << 8) |
	       ((p[3].best_dqdly << 12) & 0xffff) |
	       ((p[4].best_dqdly & 0xf) << 16) |
	       ((p[5].best_dqdly & 0xf) << 20) |
	       ((p[6].best_dqdly & 0xf) << 24);
}

int do_sw_tx_dq_dqs_calib(void)
{
	struct tx_perbit_delay tx_perbit_dly[TX_DQ_DATA_WIDTH];
	unsigned int dqs_delay[TX_DQS_NUMBER], dqm_delay[TX_DQS_NUMBER];
	int tap, found;
	int bit, group;
	unsigned int first, last, value, errors;
	signed char dq_width, dqs_width;
	unsigned char delay;

	for (bit = 0; bit < TX_DQ_DATA_WIDTH; bit++) {
		tx_perbit_dly[bit].first_dqdly_pass = 0;
		tx_perbit_dly[bit].last_dqdly_pass = -1;
		tx_perbit_dly[bit].first_dqsdly_pass = 0;
		tx_perbit_dly[bit].last_dqsdly_pass = -1;
		tx_perbit_dly[bit].best_dqdly = 0;
		tx_perbit_dly[bit].best_dqsdly = 0;
	}
	DRAMC_WRITE_REG(0, DRAMC_PADCTL2);
	DRAMC_WRITE_REG(0, DRAMC_DQODLY1);
	DRAMC_WRITE_REG(0, DRAMC_DQODLY2);
	DRAMC_WRITE_REG(0, DRAMC_DQODLY3);
	DRAMC_WRITE_REG(0, DRAMC_DQODLY4);
	found = 0;
	for (tap = MAX_TX_DQSDLY_TAPS - 1; tap >= 0; tap--) {
		value = DRAMC_READ_REG(DRAMC_PADCTL3);
		value &= 0xffff0000;
		value |= (tap << 4) | (tap << 8) | tap | (tap << 12);
		DRAMC_WRITE_REG(value, DRAMC_PADCTL3);
		errors = tx_test();
		for (bit = 0; bit < TX_DQ_DATA_WIDTH; bit++)
			if (tx_perbit_dly[bit].last_dqsdly_pass == -1 &&
			    !(errors & (1 << bit))) {
				tx_perbit_dly[bit].last_dqsdly_pass = tap;
				found++;
			}
		if (found == TX_DQ_DATA_WIDTH)
			break;
	}
	DRAMC_WRITE_CLEAR(0xffff, DRAMC_PADCTL3);
	found = 0;
	for (tap = MAX_TX_DQDLY_TAPS - 1; tap >= 0; tap--) {
		value = ((tap << 4) & 0xff) | ((tap & 0xf) << 8) | (tap & 0xf) |
			((tap << 12) & 0xffff);
		DRAMC_WRITE_REG(value, DRAMC_PADCTL2);
		value |= ((tap & 0xf) << 16) | ((tap & 0xf) << 20) |
			 (tap << 28) | ((tap & 0xf) << 24);
		DRAMC_WRITE_REG(value, DRAMC_DQODLY1);
		DRAMC_WRITE_REG(value, DRAMC_DQODLY2);
		DRAMC_WRITE_REG(value, DRAMC_DQODLY3);
		DRAMC_WRITE_REG(value, DRAMC_DQODLY4);
		errors = tx_test();
		for (bit = 0; bit < TX_DQ_DATA_WIDTH; bit++)
			if (tx_perbit_dly[bit].last_dqdly_pass == -1 &&
			    !(errors & (1 << bit))) {
				tx_perbit_dly[bit].last_dqdly_pass = tap;
				found++;
			}
		if (found == TX_DQ_DATA_WIDTH)
			break;
	}
	/* Center DQ versus DQS, then align eight bits to their shared DQS. */
	for (group = 0; group < TX_DQS_NUMBER; group++) {
		first = group * 8;
		last = first + 7;
		dqs_delay[group] = 0;
		dqm_delay[group] = 0;
		for (bit = first; bit <= last; bit++) {
			dqs_width = tx_perbit_dly[bit].last_dqsdly_pass + 1 -
				    tx_perbit_dly[bit].first_dqsdly_pass;
			dq_width = tx_perbit_dly[bit].last_dqdly_pass + 1 -
				   tx_perbit_dly[bit].first_dqdly_pass;
			if (dqs_width == dq_width) {
				tx_perbit_dly[bit].best_dqsdly = 0;
				tx_perbit_dly[bit].best_dqdly = 0;
			} else if (dqs_width < dq_width) {
				tx_perbit_dly[bit].best_dqsdly = 0;
				delay = tx_perbit_dly[bit].best_dqdly +
					(dq_width - dqs_width) / 2;
				if (delay < MAX_TX_DQDLY_TAPS)
					tx_perbit_dly[bit].best_dqdly = delay;
				else
					tx_perbit_dly[bit].best_dqdly =
						MAX_TX_DQDLY_TAPS - 1;
			} else {
				delay = tx_perbit_dly[bit].best_dqsdly +
					(dqs_width - dq_width) / 2;
				if (delay < MAX_TX_DQSDLY_TAPS)
					tx_perbit_dly[bit].best_dqsdly = delay;
				else
					tx_perbit_dly[bit].best_dqsdly =
						MAX_TX_DQSDLY_TAPS - 1;
				tx_perbit_dly[bit].best_dqdly = 0;
				if (dqs_delay[group] <
				    tx_perbit_dly[bit].best_dqsdly)
					dqs_delay[group] =
						tx_perbit_dly[bit].best_dqsdly;
			}
		}
		for (bit = first; bit <= last; bit++) {
			if (tx_perbit_dly[bit].best_dqsdly < dqs_delay[group]) {
				tx_perbit_dly[bit].best_dqdly +=
					dqs_delay[group] -
					tx_perbit_dly[bit].best_dqsdly;
				tx_perbit_dly[bit].best_dqdly =
					tx_perbit_dly[bit].best_dqdly <
							MAX_TX_DQDLY_TAPS ?
						tx_perbit_dly[bit].best_dqdly :
						MAX_TX_DQDLY_TAPS - 1;
			}
			dqm_delay[group] += tx_perbit_dly[bit].best_dqdly;
		}
		/* DQM follows the average of the eight corrected DQ delays. */
		dqm_delay[group] /= 8;
	}
	value = ((dqm_delay[1] << 4) & 0xff) | (dqm_delay[0] & 0xf);
	DRAMC_WRITE_REG(value, DRAMC_PADCTL2);
	opt_tx_dqm = value;
	value = pack_tx_dq(tx_perbit_dly);
	DRAMC_WRITE_REG(value, DRAMC_DQODLY1);
	opt_tx_dq[0] = value;
	value = pack_tx_dq(tx_perbit_dly + 8);
	DRAMC_WRITE_REG(value, DRAMC_DQODLY2);
	opt_tx_dq[1] = value;
	value = (dqs_delay[1] << 4) | dqs_delay[0];
	DRAMC_WRITE_REG(value, DRAMC_PADCTL3);
	opt_tx_dqs = value;
	return 0;
}
