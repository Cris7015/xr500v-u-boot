/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "en751221_ddr.h"

#define UCLK_20M_115200 59904U

void uart_init(void)
{
	unsigned int word;
	UART8(CR_UART_FCR) = UART_FCR;
	UART8(CR_UART_MCR) = UART_MCR;
	UART8(CR_UART_MISCC) = UART_MISCC;
	UART8(CR_UART_IER) = UART_IER;
	UART8(CR_UART_LCR) = UART_BRD_ACCESS;
	word = (UCLK_20M_115200 << 16) | UART_XYD_Y;
	MMIO32(CR_UART_XYD) = word;
	UART8(CR_UART_BRDL) = UART_BRDL_20M;
	UART8(CR_UART_BRDH) = UART_BRDH_20M;
	UART8(CR_UART_LCR) = UART_LCR;
}

void serial_outc(char c)
{
	while (!(UART8(CR_UART_LSR) & LSR_THRE));
	UART8(CR_UART_THR) = (unsigned char)c;
}

char serial_inc(void)
{
	while (!(UART8(CR_UART_LSR) & 1))
		;
	return UART8(CR_UART_RBR);
}

int serial_tstc(void)
{
	return !!(UART8(CR_UART_LSR) & 1);
}

int get_SYS_HCLK(void)
{
	return EN7512_SYS_HCLK;
}

void prom_puts(const char *s)
{
	while (*s) {
		char c = *s++;
		if (c == '\n')
			serial_outc('\r');
		serial_outc(c);
	}
}

void prom_printf(const char *s, ...)
{
	prom_puts(s);
}

void prom_print_hex(unsigned int value, int digits)
{
	int i;
	for (i = digits - 1; i >= 0; i--) {
		unsigned int n = (value >> (i * 4)) & 15;
		serial_outc(n < 10 ? '0' + n : 'a' + n - 10);
	}
}

void prom_print_dec(unsigned int value)
{
	unsigned int divisor, remainder = value;
	int leading_zero = 1;
	for (divisor = 1000000000U; divisor; divisor /= 10) {
		unsigned int digit = remainder / divisor;
		remainder %= divisor;
		if (digit || divisor == 1)
			leading_zero = 0;
		if (!leading_zero)
			serial_outc('0' + digit);
	}
}
