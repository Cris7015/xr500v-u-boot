// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal EcoNet serial-flash reader used before driver model.
 * Reconstructed from the vendor TCBoot move_data stage.
 */

#ifdef ECONET_STANDALONE_BOOT
#include "flash/loader.h"
#else
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>
#include <mach/boot.h>
#endif

#define SF_READ_IDLE_EN		0x004
#define SF_MTX_MODE_TOG		0x014
#define SF_RDCTL_FSM			0x018
#define SF_MACMUX_SEL			0x01c
#define SF_MANUAL_EN			0x020
#define SF_MANUAL_OPFIFO_EMPTY		0x024
#define SF_MANUAL_OPFIFO_WDATA		0x028
#define SF_MANUAL_OPFIFO_FULL		0x02c
#define SF_MANUAL_OPFIFO_WR		0x030
#define SF_MANUAL_DFIFO_FULL		0x034
#define SF_MANUAL_DFIFO_WDATA		0x038
#define SF_MANUAL_DFIFO_EMPTY		0x03c
#define SF_MANUAL_DFIFO_RD		0x040
#define SF_MANUAL_DFIFO_RDATA		0x044
#define SF_SI_CK_SEL			0x09c
#define SF_STRAP			0x114

#define SF_STRAP_ADDR_4B		BIT(0)
#define SF_STRAP_SPI_NAND		BIT(1)
#define SF_STRAP_DUMMY_APPEND		BIT(2)

#define OP_CSH				0x00
#define OP_CSL				0x01
#define OP_CK				0x02
#define OP_OUTS				0x08
#define OP_INS				0x0c

#define OP_SHIFT			9
#define OP_CMD_MASK			0x1f
#define OP_LEN_MASK			0x1ff

#define SPIN_LIMIT			1000000
#define NAND_PAGE_SIZE			2048
#define NOR_READ_CHUNK			1024

static inline void __iomem *sf_reg(u32 reg)
{
	return (void __iomem *)(ECONET_SFC_BASE + reg);
}

static int sf_wait_eq(u32 reg, u32 expected)
{
	unsigned int timeout = SPIN_LIMIT;

	while (timeout--) {
		if (__raw_readl(sf_reg(reg)) == expected)
			return 0;
	}

	return -ETIMEDOUT;
}

static int sf_op(u32 op, u32 len)
{
	u32 val = ((op & OP_CMD_MASK) << OP_SHIFT) | (len & OP_LEN_MASK);
	int ret;

	/*
	 * Order matches the vendor's move_data.S (send_opfifo_write_cmd):
	 * WDATA is staged *before* the FULL check, not after. Checking FULL
	 * first (as this used to) hung on real EN751221 hardware -- OPFIFO
	 * _FULL apparently never deasserts until something has been written
	 * to WDATA.
	 */
	__raw_writel(val, sf_reg(SF_MANUAL_OPFIFO_WDATA));

	ret = sf_wait_eq(SF_MANUAL_OPFIFO_FULL, 0);
	if (ret)
		return ret;

	__raw_writel(1, sf_reg(SF_MANUAL_OPFIFO_WR));

	return sf_wait_eq(SF_MANUAL_OPFIFO_EMPTY, 1);
}

static int sf_put_byte(u8 val)
{
	int ret = sf_wait_eq(SF_MANUAL_DFIFO_FULL, 0);

	if (ret)
		return ret;

	__raw_writel(val, sf_reg(SF_MANUAL_DFIFO_WDATA));
	return 0;
}

static int sf_put_bytes(const u8 *buf, size_t len)
{
	size_t i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = sf_put_byte(buf[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int sf_get_byte(u8 *val)
{
	int ret = sf_wait_eq(SF_MANUAL_DFIFO_EMPTY, 0);

	if (ret)
		return ret;

	*val = __raw_readl(sf_reg(SF_MANUAL_DFIFO_RDATA)) & 0xff;
	__raw_writel(1, sf_reg(SF_MANUAL_DFIFO_RD));
	return 0;
}

static int sf_finish(void)
{
	int ret;

	ret = sf_op(OP_CSH, 1);
	if (ret)
		return ret;

	return sf_op(OP_CK, 5);
}

int econet_sfc_init(void)
{
	int ret;

	__raw_writel(0x9, sf_reg(SF_SI_CK_SEL));
	__raw_writel(0, sf_reg(SF_READ_IDLE_EN));

	ret = sf_wait_eq(SF_RDCTL_FSM, 0);
	if (ret)
		return ret;

	__raw_writel(0x9, sf_reg(SF_MTX_MODE_TOG));
	__raw_writel(1, sf_reg(SF_MACMUX_SEL));
	__raw_writel(1, sf_reg(SF_MANUAL_EN));

	return 0;
}

static int sf_nand_load_page(u32 page)
{
	u8 page_addr[] = { page >> 16, page >> 8, page };
	u8 status;
	unsigned int timeout = SPIN_LIMIT;
	int ret;

	ret = sf_op(OP_CSL, 1);
	if (ret)
		return ret;
	ret = sf_op(OP_OUTS, 1);
	if (ret)
		return ret;
	ret = sf_put_byte(0x13);
	if (ret)
		return ret;
	ret = sf_op(OP_OUTS, ARRAY_SIZE(page_addr));
	if (ret)
		return ret;
	ret = sf_put_bytes(page_addr, ARRAY_SIZE(page_addr));
	if (ret)
		return ret;
	ret = sf_finish();
	if (ret)
		return ret;

	while (timeout--) {
		ret = sf_op(OP_CSL, 1);
		if (ret)
			return ret;
		ret = sf_op(OP_OUTS, 1);
		if (ret)
			return ret;
		ret = sf_put_byte(0x0f);
		if (ret)
			return ret;
		ret = sf_op(OP_OUTS, 1);
		if (ret)
			return ret;
		ret = sf_put_byte(0xc0);
		if (ret)
			return ret;
		ret = sf_op(OP_INS, 1);
		if (ret)
			return ret;
		ret = sf_get_byte(&status);
		if (ret)
			return ret;
		ret = sf_finish();
		if (ret)
			return ret;
		if (!(status & BIT(0)))
			return 0;
	}

	return -ETIMEDOUT;
}

static int sf_nand_read_cache(u32 column, u8 *dst, size_t len,
			      bool dummy_append)
{
	u8 address[3];
	size_t i;
	int ret;

	if (dummy_append) {
		address[0] = column >> 8;
		address[1] = column;
		address[2] = 0;
	} else {
		address[0] = 0;
		address[1] = column >> 8;
		address[2] = column;
	}

	ret = sf_op(OP_CSL, 1);
	if (ret)
		return ret;
	ret = sf_op(OP_OUTS, 1);
	if (ret)
		return ret;
	ret = sf_put_byte(0x03);
	if (ret)
		return ret;
	ret = sf_op(OP_OUTS, ARRAY_SIZE(address));
	if (ret)
		return ret;
	ret = sf_put_bytes(address, ARRAY_SIZE(address));
	if (ret)
		return ret;

	for (i = 0; i < len; i++) {
		ret = sf_op(OP_INS, 1);
		if (ret)
			return ret;
		ret = sf_get_byte(&dst[i]);
		if (ret)
			return ret;
	}

	return sf_finish();
}

static int sf_nor_read_once(u32 offset, u8 *dst, size_t len, bool addr4b,
			    bool dummy_append)
{
	u8 address[4];
	size_t addr_len = 0;
	size_t i;
	int ret;

	if (addr4b)
		address[addr_len++] = offset >> 24;
	if (dummy_append) {
		address[addr_len++] = offset >> 8;
		address[addr_len++] = offset;
		address[addr_len++] = 0;
	} else {
		address[addr_len++] = offset >> 16;
		address[addr_len++] = offset >> 8;
		address[addr_len++] = offset;
	}

	ret = sf_op(OP_CSL, 1);
	if (ret)
		return ret;
	ret = sf_op(OP_OUTS, 1);
	if (ret)
		return ret;
	ret = sf_put_byte(0x03);
	if (ret)
		return ret;

	ret = sf_op(OP_OUTS, addr_len);
	if (ret)
		return ret;
	ret = sf_put_bytes(address, addr_len);
	if (ret)
		return ret;

	for (i = 0; i < len; i++) {
		ret = sf_op(OP_INS, 1);
		if (ret)
			return ret;
		ret = sf_get_byte(&dst[i]);
		if (ret)
			return ret;
	}

	return sf_finish();
}

static int sf_nor_read(u32 offset, u8 *dst, size_t len, bool addr4b,
		       bool dummy_append)
{
	while (len) {
		size_t chunk = NOR_READ_CHUNK - (offset % NOR_READ_CHUNK);
		int ret;

		if (chunk > len)
			chunk = len;
		ret = sf_nor_read_once(offset, dst, chunk, addr4b,
				       dummy_append);
		if (ret)
			return ret;
		offset += chunk;
		dst += chunk;
		len -= chunk;
	}

	return 0;
}

int econet_sfc_read(u32 offset, void *dst, size_t len)
{
	u32 strap = __raw_readl(sf_reg(SF_STRAP));
	u32 page_size = NAND_PAGE_SIZE;
#ifdef ECONET_STANDALONE_BOOT
#ifndef ECONET_NAND_PAGE_SHIFT
	u32 shift;
#endif
#endif
	u8 *buf = dst;
	int ret;

	if (!(strap & SF_STRAP_SPI_NAND))
		return sf_nor_read(offset, buf, len, strap & SF_STRAP_ADDR_4B,
				   strap & SF_STRAP_DUMMY_APPEND);

	/* move_data detects the NAND page shift during cold boot. */
#ifdef ECONET_STANDALONE_BOOT
#ifdef ECONET_NAND_PAGE_SHIFT
	page_size = 1U << ECONET_NAND_PAGE_SHIFT;
#else
	shift = __raw_readl((void *)0xbfa40020);

	if (shift < 11 || shift > 13)
		return -EINVAL;
	page_size = 1U << shift;
#endif
#endif
	while (len) {
		u32 page = offset / page_size;
		u32 column = offset % page_size;
		size_t chunk = page_size - column;

		if (chunk > len)
			chunk = len;

		ret = sf_nand_load_page(page);
		if (ret)
			return ret;
		ret = sf_nand_read_cache(column, buf, chunk,
					 strap & SF_STRAP_DUMMY_APPEND);
		if (ret)
			return ret;

		offset += chunk;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}
