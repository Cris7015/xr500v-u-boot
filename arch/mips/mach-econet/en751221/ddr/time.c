/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "en751221_ddr.h"

extern int get_SYS_HCLK(void);

static unsigned int timer_ldv_reg(unsigned int timer)
{
	return CR_TIMER_BASE + 0x04 + timer * 8;
}

void timer_Configure(unsigned char n, unsigned char en, unsigned char mode,
		     unsigned char halt)
{
	unsigned int w = MMIO32(CR_TIMER_CTL);
	w |= ((unsigned int)en << n) | ((unsigned int)mode << (n + 8)) |
	     ((unsigned int)halt << (n + 26));
	MMIO32(CR_TIMER_CTL) = w;
}

void timerSet(unsigned int n, unsigned int time, unsigned int en,
	      unsigned int mode, unsigned int halt)
{
	unsigned int w = (unsigned int)get_SYS_HCLK() * 1000U / 2U;
	MMIO32(timer_ldv_reg(n)) = w * time;
	timer_Configure(n, en, mode, halt);
}

void time_polling_init(void)
{
	timerSet(1, TIMERTICKS_10MS, ENABLE, TIMER_TOGGLEMODE,
		 TIMER_HALTDISABLE);
}

void pause_polling(unsigned int us)
{
	volatile unsigned int now, last, ticks, ldv;
	unsigned int unit = (unsigned int)get_SYS_HCLK() * 500U,
		     wait = us * (unit / 1000U);
	ldv = MMIO32(CR_TIMER1_LDV);
	ticks = 0;
	last = MMIO32(CR_TIMER1_VLR);
	do {
		now = MMIO32(CR_TIMER1_VLR);
		ticks += last >= now ? last - now : ldv - now + last;
		last = now;
	} while (ticks < wait);
}
