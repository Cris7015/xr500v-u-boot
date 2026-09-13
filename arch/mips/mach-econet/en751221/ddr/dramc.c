/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovered from dramc.o; complete-file comparison is the acceptance test. */

#include <version.h>
#include "en751221_ddr.h"

extern void prom_puts(const char *s);
extern void prom_print_hex(unsigned int value, int digits);
extern void prom_print_dec(unsigned int value);
extern void pause_polling(unsigned int count);
extern int do_dqs_gw_calib_1(void);
extern void dle_factor_handler(unsigned int value);
extern int do_sw_rx_dq_dqs_calib(void);
extern int do_dle_calib(void);
extern int do_sw_tx_dq_dqs_calib(void);
extern int en7512_dramc_init(void);
extern int opt_gw_coarse_value, opt_gw_fine_value, opt_dle_value;

int dram_type = 0;
int dram_size = 0;
int PKG_type;

int prom_printf_s(const char *fmt, ...)
{
	prom_puts(fmt);
	return 0;
}

static inline void print_probe_error(unsigned int address, unsigned int value)
{
	prom_puts("addr=0x");
	prom_print_hex(address, 8);
	prom_puts("\n");
	prom_print_hex(value, 8);
	prom_puts("\n");
}

int check_column_bank(void)
{
	/* Preserve the binary's separate column and bank probes. Column reads
     * compile as ordinary loads; the bank probe requires volatile ordering.
     * Empty constraints retain observed lifetimes and branch layout without
     * supplying opcodes. No MT6739 geometry or EMI settings are imported.
     */
	int col_bits, bank_bits;
	register int next_col __asm__("$22");
	unsigned int value;
	if (dram_type == DDR3)
		goto already_configured;
	for (col_bits = 8; col_bits != max_col_bits; col_bits = next_col) {
		DRAMC_WRITE_REG((DRAMC_READ_REG(DRAMC_CONF1) & ~0x300) |
					((col_bits - 7) << 8),
				DRAMC_CONF1);
		delay_a_while(2);
		{
			unsigned int readback;
			ADDR_WRITE_REG(0x12345678, DRAM_BASE_ADDR);
			delay_a_while(2);
			next_col = col_bits + 1;
			__asm__ volatile("" : "+r"(next_col));
			unsigned int address = DRAM_BASE_ADDR + (1 << next_col);
			ADDR_WRITE_REG(0x87654321, address);
			delay_a_while(2);
			readback = *(unsigned int *)address;
			value = *(unsigned int *)DRAM_BASE_ADDR;
			while (readback != 0x87654321) {
				ADDR_WRITE_REG(0x87654321, address);
				readback = *(unsigned int *)address;
				value = *(unsigned int *)DRAM_BASE_ADDR;
			}
		}

		if (value == 0x12345678)
			goto next_column;
		if (value == 0x87654321) {
			goto column_found;
		} else {
			prom_puts("Checking col num. dram r/w error!\n");
			print_probe_error(DRAM_BASE_ADDR, value);
			return -1;
		}
next_column:
		__asm__ volatile("" : : "r"(10));
	}
	goto maximum_column;
check_bank_config:
	ADDR_WRITE_REG((ADDR_READ_REG(0xbfb00074) & ~3) | 1, 0xbfb00074);
	delay_a_while(2);
check_bank:
	DRAMC_WRITE_SET(1 << 24, DRAMC_GDDR3CTL1);
	delay_a_while(2);
	{
		unsigned int readback;
		ADDR_WRITE_REG(0x12345678, DRAM_BASE_ADDR);
		delay_a_while(2);
		unsigned int address = DRAM_BASE_ADDR + 0x2000;
		ADDR_WRITE_REG(0x87654321, address);
		delay_a_while(2);
		readback = *(volatile unsigned int *)address;
		value = *(volatile unsigned int *)DRAM_BASE_ADDR;
		if (readback != 0x87654321) {
			register unsigned int base __asm__("$4") =
				DRAM_BASE_ADDR;
			__asm__ volatile("" : "+r"(base));
			register unsigned int retry_addr __asm__("$2") =
				base + 0x2000;
			__asm__ volatile("" : "+r"(retry_addr));
			register unsigned int expected __asm__("$5") =
				0x87654321;
			__asm__ volatile("" : "+r"(expected));
			do {
				ADDR_WRITE_REG(0x87654321, retry_addr);
				readback = *(volatile unsigned int *)retry_addr;
				value = *(volatile unsigned int *)base;
			} while (readback != expected);
		}
	}

	if (value == 0x12345678)
		bank_bits = 3;
	else if (value == 0x87654321)
		bank_bits = 2;
	else {
		prom_puts("Checking bank num. dram r/w error!\n");
		print_probe_error(DRAM_BASE_ADDR, value);
		return -1;
	}
	{
		register unsigned int old __asm__("$3") =
			DRAMC_READ_REG(DRAMC_GDDR3CTL1);
		__asm__ volatile("" : "+r"(old));
		register unsigned int bits __asm__("$2") = (bank_bits - 2)
							   << 24;
		DRAMC_WRITE_REG(bits | (old & ~(1 << 24)), DRAMC_GDDR3CTL1);
	}
	delay_a_while(2);
done:
	__asm__ volatile("" ::: "memory");
	return 0;
already_configured: {
	/* Keep the observed explicit zero in the unframed return. */
	__asm__ volatile("" ::: "$2");
	register int result __asm__("$2") = 0;
	__asm__ volatile("" : "+r"(result));
	return result;
}
maximum_column:
	DRAMC_WRITE_REG((DRAMC_READ_REG(DRAMC_CONF1) & ~0x300) |
				((col_bits - 8) << 8),
			DRAMC_CONF1);
	delay_a_while(2);
	if (dram_type == DDR2)
		goto check_bank_config;
	goto check_bank;
column_found:
	__asm__ volatile("" : "+r"(col_bits));
	{
		register unsigned int old __asm__("$3") =
			DRAMC_READ_REG(DRAMC_CONF1);
		__asm__ volatile("" : "+r"(old));
		register int bits __asm__("$2") = (col_bits - 8) << 8;
		__asm__ volatile("" : "+r"(bits));
		DRAMC_WRITE_REG(bits | (old & ~0x300), DRAMC_CONF1);
	}
	delay_a_while(2);
	__asm__ volatile("" : : "r"(col_bits));
	goto done;
}

unsigned int calculate_dram_size(void)
{
	unsigned int size;
	register unsigned int value __asm__("$19");
	register int found __asm__("$3");
	prom_puts("Calculate size.\n");
	for (size = 32 << 20; size < (512 << 20); size <<= 1) {
		unsigned int readback;
		register unsigned int address __asm__("$16") =
			DRAM_START + size;
		/* Preserve the reference's address calculation before the first write. */
		__asm__ volatile("" : "+r"(address));
		ADDR_WRITE_REG(0x12345678, DRAM_START);
		delay_a_while(2);
		ADDR_WRITE_REG(0x87654321, address);
		delay_a_while(2);
		readback = *(unsigned int *)address;
		value = *(unsigned int *)DRAM_START;
		while (readback != 0x87654321) {
			ADDR_WRITE_REG(0x87654321, address);
			readback = *(unsigned int *)address;
			value = *(unsigned int *)DRAM_START;
		}
		__asm__ volatile("" : "+r"(value));
		if (value == 0x12345678)
			continue;
		else if (value == 0x87654321) {
			found = 1;
			goto report_size;
		} else {
			prom_puts("dram r/w error!\n");
			print_probe_error(DRAM_START, value);
			return 0;
		}
	}
	found = 0;
report_size:
	/* Keep both result paths and their observed register allocation. */
	__asm__ volatile("" : "+r"(found));
	if (!found)
		prom_puts("DRAM size=512MB(supported max size)\n");
	else {
		prom_puts("DRAM size=");
		prom_print_dec(size >> 20);
		prom_puts("MB\n");
	}
	return size >> 20;
}

void set_TRFC(void)
{
	if (dram_type == DDR2) {
		if (dram_size == 32) {
			DRAMC_WRITE_REG(0xa80b0481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x600, DRAMC_ACTIM1);
		} else if (dram_size == 64) {
			DRAMC_WRITE_REG(0xa8030481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x610, DRAMC_ACTIM1);
		} else if (dram_size == 128) {
			DRAMC_WRITE_REG(0xa8080481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x610, DRAMC_ACTIM1);
		} else if (dram_size == 256) {
			DRAMC_WRITE_REG(0xa80b0481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x620, DRAMC_ACTIM1);
		} else if (dram_size == 512) {
			DRAMC_WRITE_REG(0xa80d0481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x640, DRAMC_ACTIM1);
		}
	} else if (dram_type == DDR3) {
		if (dram_size == 64) {
			DRAMC_WRITE_REG(0xa88d0481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x600, DRAMC_ACTIM1);
		} else if (dram_size == 128) {
			DRAMC_WRITE_REG(0xa8830481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x610, DRAMC_ACTIM1);
		} else if (dram_size == 256) {
			DRAMC_WRITE_REG(0xa8800481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x620, DRAMC_ACTIM1);
		} else if (dram_size == 512) {
			DRAMC_WRITE_REG(0xa8850481, DRAMC_TEST2_3);
			DRAMC_WRITE_REG(0x640, DRAMC_ACTIM1);
		}
	}

	prom_puts("Set new TRFC.\n");
}

void dramc_reg_dump(void)
{
	int i;
	for (i = 0; i < 0x400; i += 4) {
		prom_puts("0x");
		prom_print_hex(i, 3);
		prom_puts(":");
		prom_print_hex(DRAMC_READ_REG(i), 8);
		prom_puts("\n");
	}
	for (i = 0x600; i < 0x650; i += 4) {
		prom_puts("0x");
		prom_print_hex(i, 3);
		prom_puts(":");
		prom_print_hex(DRAMC_READ_REG(i), 8);
		prom_puts("\n");
	}
}

int dramc_calib(void)
{
	int result = do_dqs_gw_calib_1();
	if (result < 0)
		return result;
	dle_factor_handler(8);
	result = do_sw_rx_dq_dqs_calib();
	if (result < 0)
		return result;
	result = do_dle_calib();
	if (result < 0)
		return result;
	return do_sw_tx_dq_dqs_calib();
}

int main(void)
{
	prom_puts("EN751221 DRAMC v1.2.2 - u-boot " PLAIN_VERSION "\r\n\r\n");

	int fail = 0, i;
	if ((VPint(0xbfb0005c) & 0xffff) == 1) {
		if (isEN7526D || isEN7513) {
			dram_type = DDR3;
			PKG_type = BGA1;
		} else if (!((VPint(CR_AHB_HWCONF) >> 4) & 1)) {
			dram_type = DDR3;
			PKG_type = BGA2;
		} else {
			PKG_type = KGD;
			if (EFUSE_IS_DDR3)
				dram_type = DDR3;
			else
				dram_type = DDR2;
		}
	} else {
		if (isLQFP) {
			PKG_type = KGD;
			if (EFUSE_IS_DDR3)
				dram_type = DDR3;
			else
				dram_type = DDR2;
		} else {
			if (isEN7526D || isEN7513)
				PKG_type = BGA1;
			else
				PKG_type = BGA2;
			dram_type = (VPint(CR_AHB_HWCONF) >> 4) & 1;
		}
	}

	delay_a_while(200);
	if (en7512_dramc_init() < 0)
		return 0;

	if (dramc_calib() < 0) {
		prom_puts("%dqs_gw (coarse/fine): ");
		prom_print_dec(opt_gw_coarse_value);
		prom_puts("/");
		prom_print_dec(opt_gw_fine_value);
		prom_puts("\n");
		prom_puts("%DQS input dly:\n");
		prom_print_hex(DRAMC_READ_REG(DRAMC_R0DELDLY), 8);
		prom_puts("\n");
		prom_puts("%DQ input dly:\n");
		for (i = 0x210; i < 0x220; i += 4) {
			prom_print_hex(DRAMC_READ_REG(i), 8);
			prom_puts("\n");
		}
		prom_puts("%dle: ");
		prom_print_dec(opt_dle_value);
		prom_puts("\n");
		prom_puts("%DRAMC calibration fail\n\r");
		VPint(0xbfb00040) |= 1 << 31;
		fail = 1;
	}
	if (PKG_type == KGD && EFUSE_Fix32MB)
		DRAMC_WRITE_REG((DRAMC_READ_REG(DRAMC_CONF1) & ~0x300) | 0x100, DRAMC_CONF1);
	else
		check_column_bank();

	dram_size = calculate_dram_size();
	SET_DRAM_SIZE(dram_size);
	set_TRFC();

	if ((DRAMC_READ_REG(0x618) & 0xff) == 0xaa)
		prom_puts("ddr-1066\n");
	else if ((DRAMC_READ_REG(0x618) & 0xff) == 0x8a)
		prom_puts("ddr-800\n");

	DRAMC_WRITE_SET(1 << 31, DRAMC_DQSCAL0);
	DRAMC_WRITE_SET(0x710, DRAMC_PERFCTL0);

	prom_puts("\r\ncalibration status: ");
	prom_print_dec(fail);
	prom_puts("\r\n");

	return 0;
}
