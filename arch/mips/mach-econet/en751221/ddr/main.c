/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovered calibration entry point; hardware definitions survive in the BSP. */
#include "en751221_ddr.h"

extern void uart_init(void);
extern void serial_outc(char c);
extern void time_polling_init(void);
extern int main(void);

unsigned char init_task_union[2560];
unsigned long kernelsp;

void start_spram(void)
{
	void (*jump_addr)(void);
	uart_init();

	if (!isFPGA || !isEN7526c) {
		if (isEN7513 || isEN7513G || isEN7512)
			VPint(0xbfa20174) = (VPint(0xbfa20174) & ~0xe) | 8;

		if (isEN7521F || isEN7521S || isEN7512) {
			VPint(0xbfa20144) = 0x12;
			VPint(0xbfa2019c) = 0x05101b08;
			VPint(0xbfa201ac) = 0x02800000;
			VPint(0xbfa201a8) = 0x402;
			VPint(0xbfa201a8) = 0x406;
			VPint(0xbfb00284) = (VPint(0xbfb00284) & 0xff000fff) | 0x000af000;
		} else if (isEN7526F || isEN7526D || isEN7513 || isEN7526G ||
				   isEN7521G || isEN7513G || isEN7586) {
			VPint(0xbfa20144) = 0x12;
			VPint(0xbfa2019c) = 0x05102308;
			VPint(0xbfa201ac) = 0x03800000;
			VPint(0xbfa201a8) = 0x402;
			VPint(0xbfa201a8) = 0x406;
			VPint(0xbfb00284) = (VPint(0xbfb00284) & 0xff000fff) |
					    0x000e1000;
		} else {
			while (1);
		}

		if (isEN7521S || isEN7521F)
			VPint(0xbfaf200c) |= 1 << 16;
	}

	time_polling_init();
	main();
	change_cp0_status(ST0_IM, 0);
	VPint(CR_INTC_IMR) = 0;
	VPint(CR_TIMER_CTL) = 0;
	jump_addr = (void (*)(void))VPint(0xbfb00280);
	jump_addr();
}
