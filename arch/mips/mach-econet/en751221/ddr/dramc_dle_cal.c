/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovered from dramc_dle_cal.o; see ../README.md for fidelity status. */
#include "en751221_ddr.h"
#include "calibration_bits.h"

extern void pause_polling(unsigned int count);
extern void prom_puts(const char *s);

static int global_dle_value = 0;
static int score = 0;
int opt_dle_idx = 0;
int opt_dle_value = 0;
int dle_result[DLE_MAX];

int dramc_dma_test(unsigned int start, unsigned int len, void *ext_arg)
{
	int i;
	volatile unsigned int count;
	volatile unsigned int timeout = 0x7fffffff;
	unsigned int *source = (unsigned int *)DRAM_START;
	unsigned int *destination = (unsigned int *)(DRAM_START + len);
	unsigned int destination_phys = 0x80000 + len;

	for (i = 0; i < len / sizeof(unsigned int); i++)
		destination[i] = 0;
	for (i = 0; i < len / sizeof(unsigned int); i++)
		source[i] = i;

	ADDR_WRITE_REG(0x80000, 0xbfb30000);
	ADDR_WRITE_REG(destination_phys, 0xbfb30004);
	ADDR_WRITE_REG(ADDR_READ_REG(0xbfb3000c) | 4, 0xbfb3000c);
	ADDR_WRITE_REG((ADDR_READ_REG(0xbfb30008) & 0xffc4) | 0x23 |
			       (len << 16),
		       0xbfb30008);
	count = 0;
	while (!(ADDR_READ_REG(0xbfb30204) & 1)) {
		count++;
		if (count == timeout)
			break;
	}
	ADDR_WRITE_REG(1, 0xbfb30204);
	for (i = 0; i < len / sizeof(unsigned int); i++)
		if (i != destination[i])
			return -1;
	return 0;
}

int dramc_ta1(unsigned int start, unsigned int len, void *ext_arg)
{
	unsigned int count = 0x7fffffff;
	int result = 0;
	/* The reference retains both copies of ext_arg and the final -1 constant.
     * Empty GCC asm constraints emit no instructions themselves; they preserve
     * those otherwise-dead lifetimes and the observed register allocation.
     * These constraints are reconstruction aids, not claimed original source.
     */
	register void *check_result __asm__("$17") = ext_arg;
	__asm__ volatile("" : "+r"(check_result));

	register unsigned int base __asm__("$3") = DRAMC0_BASE;
	__asm__ volatile("" : "+r"(base));
	unsigned int value = *(volatile unsigned int *)(base + DRAMC_TEST2_2) &
			     0xff000000;
	__asm__ volatile("" : "+r"(len));
	*(volatile unsigned int *)(base + DRAMC_TEST2_2) = len | value;
	*(volatile unsigned int *)(base + DRAMC_CONF2) |= DRAMC_TA1_ENABLE;
	while (!(DRAMC_READ_REG(DRAMC_TESTRPT) & DRAMC_TEST_DM_CMP_CPT)) {
		if (--count == 0)
			break;
	}
	delay_a_while(1);
	if (check_result)
		result = (DRAMC_READ_REG(DRAMC_TESTRPT) &
			  DRAMC_TEST_DM_CMP_ERR) ?
				 -1 :
				 0;
	DRAMC_WRITE_CLEAR(DRAMC_TA1_ENABLE, DRAMC_CONF2);
	__asm__ volatile("" : : "r"(ext_arg), "r"(-1));
	return result;
}

void dle_factor_handler(unsigned int value)
{
	/* DATLAT[2:0] is DDR2CTL[6:4]; DATLAT3 is PADCTL4[4]. The related
	 * driver's PADCTL1 comment conflicts with its own 0xe4 register access.
	 * Use the access and the EN7512 object as evidence; see the reference note.
	 */
	DRAMC_WRITE_REG(((value & 7) << 4) |
				(DRAMC_READ_REG(DRAMC_DDR2CTL) & ~0x70),
			DRAMC_DDR2CTL);
	DRAMC_WRITE_REG((((value >> 3) & 1) << 4) |
				(DRAMC_READ_REG(DRAMC_PADCTL4) & ~0x10),
			DRAMC_PADCTL4);
}

test_case dle_test_cases[] = { { dramc_dma_test, DRAM_BASE_ADDR, 0x1000,
				 (void *)-1 } };

int dle_test(void)
{
	if (dle_test_cases[0].test_case(dle_test_cases[0].start,
					dle_test_cases[0].range,
					dle_test_cases[0].ext_arg) == 0) {
		dle_result[global_dle_value++] = 1;
		return 1;
	} else {
		dle_result[global_dle_value++] = 0;
		return 0;
	}
}

void dle_calib_reset(void)
{
}

int dle_calib(void)
{
	int i;
	for (i = DLE_START; i <= DLE_END; i += DLE_STEP) {
		dle_factor_handler(i);
		score += dle_test();
	}
	return 1;
}

int do_dle_calib(void)
{
	int i;
	global_dle_value = 0;
	dle_calib();
	/* Select the second tap of the first adjacent passing pair. This matches
	 * the legacy DLE search, not the newer MT6739 DATLAT window algorithms.
	 */
	for (i = 1; i < DLE_MAX; i++) {
		if (dle_result[i - 1] == 1 && dle_result[i] == 1) {
			if (opt_dle_idx < i)
				opt_dle_idx = i;
			break;
		}
	}
	opt_dle_value = opt_dle_idx;
	if (opt_dle_value) {
		dle_factor_handler(opt_dle_value);
		return 0;
	}
	prom_puts("%cannot find opt_dle value\n");
	return -1;
}
