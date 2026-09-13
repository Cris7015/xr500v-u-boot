// SPDX-License-Identifier: GPL-2.0+

#include <compiler.h>
#include <linux/byteorder/generic.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include <mach/boot.h>

#define IH_MAGIC	0x27051956
#define IH_HDR_SIZE	64

struct econet_legacy_header {
	u32 magic;
	u32 hcrc;
	u32 time;
	u32 size;
	u32 load;
	u32 ep;
	u32 dcrc;
	u8 os;
	u8 arch;
	u8 type;
	u8 comp;
	u8 name[32];
};

static u32 get_be32(u32 val)
{
	return be32_to_cpu(val);
}

static void tpl_putc(u8 ch)
{
#ifdef CONFIG_TARGET_EN7528
	/*
	 * EN7528 LE UART registers use the byte lane at +3.
	 * The vendor boot code uses THR +0x03 and LSR +0x17.
	 */
	void __iomem *thr = (void __iomem *)(ECONET_UART0_BASE + 0x03);
	void __iomem *lsr = (void __iomem *)(ECONET_UART0_BASE + 0x17);

	while (!(__raw_readb(lsr) & 0x20))
		;
	__raw_writeb(ch, thr);
#else
	void __iomem *thr = (void __iomem *)(ECONET_UART0_BASE + 0x00);
	void __iomem *lsr = (void __iomem *)(ECONET_UART0_BASE + 0x14);

	while (!(__raw_readl(lsr) & 0x20))
		;
	__raw_writel(ch, thr);
#endif
}

static void tpl_hang(u8 code)
{
	tpl_putc(code);
	tpl_putc('\r');
	tpl_putc('\n');

	for (;;)
		;
}

static void tpl_uart_init(void)
{
	void __iomem *base = (void __iomem *)ECONET_UART0_BASE;

	__raw_writel(0x80, base + 0x0c);
	__raw_writel(0xea00fde8, base + 0x2c);
	__raw_writel(0x01, base + 0x00);
	__raw_writel(0x00, base + 0x04);
	__raw_writel(0x03, base + 0x0c);
	__raw_writel(0x0f, base + 0x08);
	__raw_writel(0x00, base + 0x10);
	__raw_writel(0x00, base + 0x24);
	__raw_writel(0x00, base + 0x04);
}

#ifdef CONFIG_TARGET_EN7528
/*
 * DRAM bring-up test for EN7528 SPI NOR.
 *
 * The BootROM exposes the beginning of the NOR through the reset/XIP alias
 * at CONFIG_TPL_TEXT_BASE (0xbfc00000).  Do not switch the SFC to manual
 * mode while executing from that mapping.  Copy the DDR payload directly
 * from XIP to FE SRAM instead.
 *
 * This is intentionally a bring-up path only.  It stops after DDR training
 * and does not load SPL.
 */
static void tpl_en7528_copy_ddr_xip(void)
{
	const volatile u8 *src =
		(const volatile u8 *)(CONFIG_TPL_TEXT_BASE +
				      ECONET_DDR_BLOB_OFFSET);
	volatile u8 *dst = (volatile u8 *)ECONET_DDR_BLOB_ADDR;
	u32 i;

	for (i = 0; i < ECONET_DDR_BLOB_SIZE; i++)
		dst[i] = src[i];

	__asm__ volatile("sync" : : : "memory");
}
#endif

#ifdef CONFIG_TARGET_EN751221
/*
 * EN751221 DRAM does not tolerate byte-wide stores during SPL load: the
 * chainloader work on this SoC showed that byte-store writes to DRAM get
 * corrupted.  Stage each chunk through FE SRAM and flush it out with
 * aligned 32-bit stores instead.
 */
#define EN751221_TPL_COPY_CHUNK	0x400

static int tpl_en751221_read_dram(u32 offset, void *dst, u32 len)
{
	u8 *scratch = (u8 *)ECONET_DDR_BLOB_ADDR;
	u8 *out = dst;

	if ((uintptr_t)out & 3)
		return -1;

	while (len) {
		u32 chunk = min_t(u32, len, EN751221_TPL_COPY_CHUNK);
		u32 padded = ALIGN(chunk, 4);
		u32 i;
		int ret;

		ret = econet_sfc_read(offset, scratch, chunk);
		if (ret)
			return ret;

		for (i = chunk; i < padded; i++)
			scratch[i] = 0;

		for (i = 0; i < padded; i += 4)
			*(volatile u32 *)(out + i) =
				*(u32 *)(scratch + i);

		__asm__ volatile("sync" : : : "memory");

		offset += chunk;
		out += chunk;
		len -= chunk;
	}

	return 0;
}
#endif

void __noreturn tpl_main(void)
{
	struct econet_legacy_header *hdr =
		(struct econet_legacy_header *)ECONET_SPL_HEADER_ADDR;
	void (*entry)(void);
	u32 load, size, ep;
	int ret;

#ifdef CONFIG_TARGET_EN7528
	/*
	 * Do not call econet_sfc_init() here.
	 *
	 * We are still executing from the SPI NOR XIP mapping.  Entering
	 * SF_MANUAL_EN at this point would remove the backing store for the
	 * following instruction fetches.
	 */
	tpl_en7528_copy_ddr_xip();

	/*
	 * The DDR payload initializes its own UART and should produce the
	 * familiar:
	 *
	 *   QFP IC
	 *   DDR3 init.
	 *   ...
	 *   7528DRAMC ...
	 */
	econet_run_ddr_blob();

	/* DDR returned successfully. UART has now been initialized by it. */
	tpl_putc('D');
	tpl_putc('\r');
	tpl_putc('\n');

	for (;;)
		;
#endif

	tpl_uart_init();
	if (econet_sfc_init())
		tpl_hang('I');

	ret = econet_sfc_read(ECONET_DDR_BLOB_OFFSET,
			      (void *)ECONET_DDR_BLOB_ADDR,
			      ECONET_DDR_BLOB_SIZE);
	if (ret)
		tpl_hang('F');

	econet_run_ddr_blob();

	ret = econet_sfc_read(ECONET_SPL_IMAGE_OFFSET, hdr, IH_HDR_SIZE);
	if (ret || get_be32(hdr->magic) != IH_MAGIC)
		tpl_hang('H');

	size = get_be32(hdr->size);
	load = get_be32(hdr->load);
	ep = get_be32(hdr->ep);

	if (!size || size > ECONET_SPL_IMAGE_LIMIT -
			   ECONET_SPL_IMAGE_OFFSET - IH_HDR_SIZE ||
	    load != CONFIG_SPL_TEXT_BASE || ep != load ||
	    load + size < load)
		tpl_hang('L');

#ifdef CONFIG_TARGET_EN751221
	ret = tpl_en751221_read_dram(ECONET_SPL_IMAGE_OFFSET + IH_HDR_SIZE,
				      (void *)load, size);
#else
	/* Write through KSEG1 so no dirty cache lines hide the SPL image. */
	ret = econet_sfc_read(ECONET_SPL_IMAGE_OFFSET + IH_HDR_SIZE,
			      (void *)(load | 0x20000000), size);
#endif
	if (ret)
		tpl_hang('S');

	__asm__ volatile("sync" : : : "memory");
	entry = (void (*)(void))ep;
	entry();
	__builtin_unreachable();
}
