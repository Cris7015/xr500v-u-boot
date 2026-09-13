/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovered DDR2/DDR3 initialization; offsets and values come from the object. */

#include "en751221_ddr.h"

extern int PKG_type, dram_type;
extern void prom_puts(const char *s);
extern void prom_print_dec(unsigned int value);
extern void pause_polling(unsigned int count);

#define WRITE(offset, value)		DRAMC_WRITE_REG(value, offset)
#define READ(offset)				DRAMC_READ_REG(offset)
#define SET(offset, value)			DRAMC_WRITE_SET(value, offset)
#define UPDATE(offset, mask, value)	WRITE(offset, (READ(offset) & (mask)) | (value))
#define DELAY(count)				delay_a_while(count)
#define PACKAGE_WRITE(offset, kgd, bga) \
	do { \
		if (PKG_type == KGD) \
			WRITE(offset, kgd); \
		else \
			WRITE(offset, bga); \
	} while (0)

/* The reference retains the adjusted timeout at selected command boundaries.
 * Empty constraints preserve those otherwise-unused results without opcodes.
 */
#define WAIT_COMMAND(mask, keep_count)                         \
	do {                                                   \
		timeout = 0xffff;                              \
		while (!(READ(DRAMC_SPCMDRESP) & (mask)))         \
			if (--timeout == 0)                    \
				goto failed;                   \
		if (keep_count)                                \
			__asm__ volatile("" : : "r"(timeout)); \
	} while (0)

#define COMMAND(mask, keep_count, delay)        \
	do {                                    \
		WRITE(DRAMC_SPCMD, mask);           \
		WAIT_COMMAND(mask, keep_count); \
		DELAY(delay);                   \
		WRITE(DRAMC_SPCMD, 0);              \
	} while (0)

#define PLL_FINISH(feedback, phase)          \
	do {                                     \
		SET(0x604, 0x100);                   \
		UPDATE(0x618, 0xf3cf0000, feedback); \
		UPDATE(0x624, 0xf3cf0000, feedback); \
		UPDATE(0x630, 0xf3cf0000, feedback); \
		WRITE(0x648, 0xc0004ab2);                \
		DELAY(1);                            \
		UPDATE(0x614, 0xf01fffff, phase);    \
		SET(0x618, 0x02000000);              \
		SET(0x624, 0x02000000);              \
		SET(0x630, 0x02000000);              \
		DELAY(20);                           \
		SET(0x640, 0x10);                    \
		DELAY(1);                            \
	} while (0)

/* The DDR3 40 MHz paths retain different arithmetic temporaries in the object. */
#define UPDATE_KGD40(offset, mask, value)                  \
	do {                                                   \
		register unsigned int v __asm__("$3") = READ(offset); \
		__asm__ volatile("" : "+r"(v));                    \
		WRITE(offset, (v & mask) | value);                     \
	} while (0)

#define UPDATE_BGA40(offset, mask, value)                   \
	do {                                                    \
		register unsigned int v __asm__("$4") = READ(offset);  \
		__asm__ volatile("" : "+r"(v));                     \
		register unsigned int m __asm__("$2") = mask;       \
		__asm__ volatile("" : : "r"(m));                    \
		register unsigned int masked __asm__("$3") = v & m; \
		__asm__ volatile("" : "+r"(masked));                \
		WRITE(offset, masked | value);                          \
	} while (0)

#define PLL_CONFIG(update_kgd40, update_bga40)               \
	do {                                                     \
		if (PKG_type == KGD) {                               \
			UPDATE(0x640, ~0x230, 0x220);                    \
			UPDATE(0x604, ~0x1e, 0x12);                      \
			if (xtal)                                        \
				UPDATE(0x600, 0xfffc0808, 0x131);            \
			else                                             \
				update_kgd40(0x600, 0xfffc0808, 0x181);      \
			DELAY(1);                                        \
			SET(0x600, 0x02000000);                          \
			DELAY(20);                                       \
			PLL_FINISH(0x088a, 0x02800000);                  \
		} else {                                             \
			UPDATE(0x640, ~0x230, 0x220);                    \
			UPDATE(0x604, ~0x1e, 0x12);                      \
			if (xtal)                                        \
				UPDATE(0x600, 0xfbfc0808, 0x04000135);       \
			else                                             \
				update_bga40(0x600, 0xfbfc0808, 0x04000185); \
			UPDATE(0x608, 0x1ffff, 0x2b020000);              \
			UPDATE(0x60c, 0xffff0000, 0x687);                \
			DELAY(1);                                        \
			SET(0x600, 0x02000000);                          \
			DELAY(20);                                       \
			SET(0x60c, 0x00080000);                          \
			DELAY(1);                                        \
			SET(0x60c, 0x00010000);                          \
			DELAY(20);                                       \
			PLL_FINISH(0x08aa, 0x04000000);                  \
		}                                                    \
	} while (0)

int en7512_dramc_init(void)
{
	unsigned int xtal, timeout;

	if (PKG_type == KGD)
		prom_puts("KGD IC\n");
	else
		prom_puts("BGA IC\n");

	xtal = VPint(CR_AHB_HWCONF) & 1;
	prom_puts("Xtal: ");
	if (xtal == 1)
		prom_puts("25Mhz");
	else
		prom_puts("20Mhz");
	prom_puts("\n");

	if (dram_type == DDR2) {
		prom_puts("DDR2 init.\n");
		VPint(0xbfb00040) = 0;
		DELAY(200);
		WRITE(0xfc, 0x07110000);
		if (PKG_type == KGD) {
			WRITE(0xb8, 0x88228822);
			WRITE(0xbc, 0x88228822);
		}
		PACKAGE_WRITE(0x7c, 0xc2031263, 0xc203126f);
		PLL_CONFIG(UPDATE, UPDATE);
		WRITE(0x48, 0xd10d);
		PACKAGE_WRITE(0xd8, 0x00100900, 0xc0100900);
		WRITE(0xe4, 1);
		WRITE(0x8c, 1);
		WRITE(0x90, 0);
		WRITE(0x94, 0x10101010);
		WRITE(0xdc, 0x83080080);
		WRITE(0xe0, 0x12080080);
		PACKAGE_WRITE(0xf0, 0, 0x80000000);
		WRITE(0xf4, 0);
		WRITE(0x168, 0x80);
		WRITE(0x130, 0x30000000);
		PACKAGE_WRITE(0xd8, 0x00300900, 0xc0300900);
		DELAY(1);
		PACKAGE_WRITE(0x04, 0xf0040442, 0xf0040482);
		WRITE(0x124, 0x80000011);
		WRITE(0x94, 0x10101010);
		WRITE(0x1c0, 0xc8b8);
		PACKAGE_WRITE(0x7c, 0xc2031263, 0xc203126f);
		WRITE(0x28, 0xf1200f01);
		WRITE(0x1e0, 0x84000000);
		WRITE(0x158, 0);
		WRITE(0x110, 0x00111190);
		PACKAGE_WRITE(0x04, 0xf0740042, 0xf0740082);
		WRITE(0xe4, 7);
		DELAY(1);
		COMMAND(4, 1, 1);
		WRITE(0x88, 0x4000);
		COMMAND(1, 1, 1);
		WRITE(0x88, 0x6000);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x88, 0x2002, 0x2040);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x88, 0x0b63, 0x0f63);
		COMMAND(1, 1, 1);
		COMMAND(4, 1, 1);
		COMMAND(8, 1, 1);
		COMMAND(8, 0, 1);
		PACKAGE_WRITE(0x88, 0x0a63, 0x0e63);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x88, 0x2382, 0x23c0);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x88, 0x2002, 0x2040);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x7c, 0xc2031263, 0xc203126f);
		WRITE(0xe4, 0x2205);
		WRITE(0x88, 0xffff);
		COMMAND(0x20, 0, 1);
		WRITE(0x1dc, 0x10606442);
		PACKAGE_WRITE(0x04, 0xf0740442, 0xf0740482);
		DELAY(1);
		WRITE(0x0c, 0);
		PACKAGE_WRITE(0x00, 0x22174441, 0x33484484);
		DELAY(1);
		WRITE(0x44, 0xa80d0481);
		WRITE(0x1e8, 0x640);
		PACKAGE_WRITE(0x08, 0x000f5f2f, 0x000f5f3f);
		WRITE(0x10, 0);
		WRITE(0xf8, 0xedcb000f);
		WRITE(0x1d8, 0x00c80008);
		WRITE(0x18, 0x31313131);
		VPint(0xbfb00074) = 0x104;
	} else {
		prom_puts("DDR3 init.\n");
		VPint(0xbfb00040) = 0;
		DELAY(200);
		WRITE(0xfc, 0x07110000);
		if (PKG_type == KGD) {
			WRITE(0xb8, 0x88228822);
			WRITE(0xbc, 0x88228822);
		}
		PACKAGE_WRITE(0x7c, 0xa18711e1, 0xb48711ed);
		PLL_CONFIG(UPDATE_KGD40, UPDATE_BGA40);
		WRITE(0x48, 0x1e00d10d);
		PACKAGE_WRITE(0xd8, 0x00100900, 0x40100900);
		DELAY(200);
		WRITE(0xe4, 0xa3);
		DELAY(500);
		WRITE(0x8c, 1);
		WRITE(0x90, 0);
		WRITE(0x94, 0x10101010);
		WRITE(0xdc, 0x83080080);
		WRITE(0xe0, 0x12080080);
		PACKAGE_WRITE(0xf0, 0, 0x80000000);
		WRITE(0xf4, 0x01000000);
		WRITE(0x168, 0x80);
		WRITE(0x130, 0x30000000);
		PACKAGE_WRITE(0xd8, 0x00300900, 0x40300900);
		DELAY(1);
		WRITE(0x04, 0xf0740642);
		WRITE(0x124, 0x80000011);
		WRITE(0x94, 0x10101010);
		WRITE(0x1c0, 0xc8b8);
		PACKAGE_WRITE(0x7c, 0xa18711e1, 0xb48711e5);
		WRITE(0x28, 0xf1200f01);
		WRITE(0x1e0, 0x88000000);
		WRITE(0x158, 0);
		WRITE(0x110, 0x00111190);
		WRITE(0x04, 0xf0740642);
		WRITE(0xe4, 0xa7);
		DELAY(1);
		PACKAGE_WRITE(0x88, 0x4200, 0x4208);
		COMMAND(1, 1, 1);
		WRITE(0x88, 0x6000);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x88, 0x2000, 0x2004);
		COMMAND(1, 0, 1);
		PACKAGE_WRITE(0x88, 0x1521, 0x1931);
		COMMAND(1, 0, 1);
		if (PKG_type != KGD) {
			WRITE(0x88, 0x400);
			COMMAND(0x10, 1, 6);
		}
		WRITE(0x1e4, 0x1100);
		DELAY(1);
		WRITE(0xe4, 0x22a7);
		WRITE(0x1e0, 0x88000000);
		WRITE(0x88, 0xffff);
		COMMAND(0x20, 0, 1);
		WRITE(0x1dc, 0x10602842);
		WRITE(0x04, 0xf0740642);
		PACKAGE_WRITE(0x7c, 0xa18711e1, 0xb48711ed);
		DELAY(1);
		WRITE(0x0c, 0);
		PACKAGE_WRITE(0x00, 0x22274430, 0x33684552);
		DELAY(1);
		WRITE(0x44, 0xa8830481);
		WRITE(0x1e8, 0x650);
		PACKAGE_WRITE(0x08, 0x000f5f2f, 0x000f5f3f);
		WRITE(0x10, 0);
		WRITE(0xf8, 0xedcb000f);
		WRITE(0x1d8, 0x00c80008);
		WRITE(0x18, 0x31313131);
		VPint(0xbfb00074) = 0x105;
	}
	DELAY(2);
	prom_puts("DRAMC init done. \n");
	return 0;
failed:
	prom_puts("DRAMC Init Fail!\r\n");
	return -1;
}
