// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on the Linux clk-en7523.c but majorly reworked
 * for U-Boot that doesn't require CCF subsystem.
 *
 * Major modification, support for set_rate, realtime
 * get_rate and split for reset part to a different driver.
 *
 * Author: Lorenzo Bianconi <lorenzo@kernel.org> (original driver)
 *	   Christian Marangi <ansuelsmth@gmail.com>
 */

#include <clk-uclass.h>
#include <dm.h>
#include <dm/devres.h>
#include <dm/device_compat.h>
#include <dm/lists.h>
#include <linux/delay.h>
#include <regmap.h>
#include <soc/airoha/scu-regmap.h>
#include <soc/airoha/pkgids.h>

#include <dt-bindings/clock/en7523-clk.h>
#include <dt-bindings/clock/econet,en751221-scu.h>
#include <dt-bindings/clock/econet,en7528-scu.h>

#define REG_GSW_CLK_DIV_SEL		0x1b4
#define REG_EMI_CLK_DIV_SEL		0x1b8
#define REG_BUS_CLK_DIV_SEL		0x1bc
#define REG_SPI_CLK_DIV_SEL		0x1c4
#define REG_SPI_CLK_FREQ_SEL		0x1c8
#define REG_NPU_CLK_DIV_SEL		0x1fc
#define REG_CRYPTO_CLKSRC		0x200

/* EN751221 / EN7528 NP-SCU and CHIP-SCU clock registers. */
#define REG_PCI_CONTROL			0x088
#define   REG_PCI_CONTROL_PERSTOUT	BIT(29)
#define   REG_PCI_CONTROL_PERSTOUT1	BIT(26)
#define   REG_PCI_CONTROL_REFCLK_EN1	BIT(22)
#define REG_RESET_CONTROL1		0x834
#define   REG_RESET_CONTROL_PCIEHB	BIT(29)
#define   REG_RESET_CONTROL_PCIE1	BIT(27)
#define   REG_RESET_CONTROL_PCIE0	BIT(26)
#define REG_HIR				0x064
#define   REG_HIR_MASK			GENMASK(31, 16)
#define EN751221_REG_SPI_DIV		0x0cc
#define EN751221_REG_SPI_DIV_MASK	GENMASK(15, 8)
#define EN751221_SPI_BASE		500000000
#define EN751221_SPI_BASE_EN7526C	400000000
#define EN751221_SPI_DIV_DEFAULT	40
#define EN751221_REG_BUS		0x284
#define EN751221_REG_BUS_MASK		GENMASK(21, 12)
#define EN751221_REG_SSR3		0x094
#define EN751221_REG_SSR3_GSW_MASK	GENMASK(9, 8)
#define EN751221_REG_NP_PER_DOM_CLK_GAT_1 0x0e4
#define   EN751221_XPON_TOD_CLK_EN	BIT(8)
#define EN751221_REG_TOD_DIVIDER_ENABLE 0x0ec
#define   EN751221_XPON_TOD_DIV_EN	BIT(1)
#define EN751221_MAX_CLKS		5

#define EN7528_REG_SPI_DIV		0x0cc
#define EN7528_REG_SPI_DIV_MASK	GENMASK(15, 8)
#define EN7528_SPI_BASE		400000000
#define EN7528_SPI_DIV_DEFAULT		10
#define EN7528_REG_NP_PER_DOM_CLK_GAT_1 0x0e4
#define   EN7528_XPON_TOD_CLK_EN	BIT(8)
#define EN7528_REG_TOD_DIVIDER_ENABLE	0x0ec
#define   EN7528_XPON_TOD_DIV_EN	BIT(1)
#define EN7528_MAX_CLKS		5

#define REG_NP_SCU_PCIC			0x88
#define REG_NP_SCU_SSTR			0x9c
#define REG_PCIE_XSI0_SEL_MASK		GENMASK(14, 13)
#define REG_PCIE_XSI1_SEL_MASK		GENMASK(12, 11)
#define REG_CRYPTO_CLKSRC2		0x20c

#define EN7523_MAX_CLKS			8
#define EN7581_MAX_CLKS			9
#define EN7583_MAX_CLKS			11

struct airoha_clk_desc {
	int id;
	const char *name;
	u32 base_reg;
	u8 base_bits;
	u8 base_shift;
	union {
		const unsigned int *base_values;
		unsigned int base_value;
	};
	size_t n_base_values;

	u16 div_reg;
	u8 div_bits;
	u8 div_shift;
	u16 div_val0;
	u8 div_step;
	u8 div_offset;
};

struct airoha_econet_clk_data {
	u32 spi_base;
	u32 spi_alt_base;
	u32 spi_alt_pkg;
	u16 spi_div_reg;
	u32 spi_div_mask;
	u32 spi_div_default;
	u16 xpon_tod_clk_reg;
	u32 xpon_tod_clk_mask;
	u16 xpon_tod_div_reg;
	u32 xpon_tod_div_mask;
};

struct airoha_clk_priv {
	struct regmap *chip_scu_map;
	struct regmap *scu_map;
	struct airoha_clk_soc_data *data;
};

struct airoha_clk_soc_data {
	u32 num_clocks;
	const struct airoha_clk_desc *descs;
	const struct airoha_econet_clk_data *econet;
};

static const u32 gsw_base[] = { 400000000, 500000000 };
static const u32 emi_base[] = { 333000000, 400000000 };
static const u32 bus_base[] = { 500000000, 540000000 };
static const u32 slic_base[] = { 100000000, 3125000 };
static const u32 npu_base[] = { 333000000, 400000000, 500000000 };
/* EN7581 */
static const u32 emi7581_base[] = { 540000000, 480000000, 400000000, 300000000 };
static const u32 bus7581_base[] = { 600000000, 540000000 };
static const u32 npu7581_base[] = { 800000000, 750000000, 720000000, 600000000 };
static const u32 crypto_base[] = { 540000000, 480000000 };
static const u32 emmc7581_base[] = { 200000000, 150000000 };
/* EN751221 / EN7528 */
static const u32 gsw751221_base[] = { 500000000, 250000000, 400000000, 200000000 };
/* AN7583 */
static const u32 gsw7583_base[] = { 540672000, 270336000, 400000000, 200000000 };
static const u32 emi7583_base[] = { 540672000, 480000000, 400000000, 300000000 };
static const u32 bus7583_base[] = { 600000000, 540672000, 480000000, 400000000 };
static const u32 spi7583_base[] = { 400000000, 12500000 };
static const u32 npu7583_base[] = { 666000000, 800000000, 720000000, 600000000 };
static const u32 crypto7583_base[] = { 540672000, 400000000 };
static const u32 emmc7583_base[] = { 150000000, 200000000 };

static const struct airoha_clk_desc en7523_base_clks[EN7523_MAX_CLKS] = {
	[EN7523_CLK_GSW] = {
		.id = EN7523_CLK_GSW,
		.name = "gsw",

		.base_reg = REG_GSW_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = gsw_base,
		.n_base_values = ARRAY_SIZE(gsw_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_EMI] = {
		.id = EN7523_CLK_EMI,
		.name = "emi",

		.base_reg = REG_EMI_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = emi_base,
		.n_base_values = ARRAY_SIZE(emi_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_BUS] = {
		.id = EN7523_CLK_BUS,
		.name = "bus",

		.base_reg = REG_BUS_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = bus_base,
		.n_base_values = ARRAY_SIZE(bus_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_SLIC] = {
		.id = EN7523_CLK_SLIC,
		.name = "slic",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = slic_base,
		.n_base_values = ARRAY_SIZE(slic_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 24,
		.div_val0 = 20,
		.div_step = 2,
	},
	[EN7523_CLK_SPI] = {
		.id = EN7523_CLK_SPI,
		.name = "spi",

		.base_reg = REG_SPI_CLK_DIV_SEL,

		.base_value = 400000000,

		.div_bits = 5,
		.div_shift = 8,
		.div_val0 = 40,
		.div_step = 2,
	},
	[EN7523_CLK_NPU] = {
		.id = EN7523_CLK_NPU,
		.name = "npu",

		.base_reg = REG_NPU_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = npu_base,
		.n_base_values = ARRAY_SIZE(npu_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_CRYPTO] = {
		.id = EN7523_CLK_CRYPTO,
		.name = "crypto",

		.base_reg = REG_CRYPTO_CLKSRC,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = emi_base,
		.n_base_values = ARRAY_SIZE(emi_base),
	}
};

static const struct airoha_clk_desc en7581_base_clks[EN7581_MAX_CLKS] = {
	[EN7523_CLK_GSW] = {
		.id = EN7523_CLK_GSW,
		.name = "gsw",

		.base_reg = REG_GSW_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = gsw_base,
		.n_base_values = ARRAY_SIZE(gsw_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_EMI] = {
		.id = EN7523_CLK_EMI,
		.name = "emi",

		.base_reg = REG_EMI_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = emi7581_base,
		.n_base_values = ARRAY_SIZE(emi7581_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_BUS] = {
		.id = EN7523_CLK_BUS,
		.name = "bus",

		.base_reg = REG_BUS_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = bus7581_base,
		.n_base_values = ARRAY_SIZE(bus7581_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_SLIC] = {
		.id = EN7523_CLK_SLIC,
		.name = "slic",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = slic_base,
		.n_base_values = ARRAY_SIZE(slic_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 24,
		.div_val0 = 20,
		.div_step = 2,
	},
	[EN7523_CLK_SPI] = {
		.id = EN7523_CLK_SPI,
		.name = "spi",

		.base_reg = REG_SPI_CLK_DIV_SEL,

		.base_value = 400000000,

		.div_bits = 5,
		.div_shift = 8,
		.div_val0 = 40,
		.div_step = 2,
	},
	[EN7523_CLK_NPU] = {
		.id = EN7523_CLK_NPU,
		.name = "npu",

		.base_reg = REG_NPU_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = npu7581_base,
		.n_base_values = ARRAY_SIZE(npu7581_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_CRYPTO] = {
		.id = EN7523_CLK_CRYPTO,
		.name = "crypto",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = crypto_base,
		.n_base_values = ARRAY_SIZE(crypto_base),
	},
	[EN7581_CLK_EMMC] = {
		.id = EN7581_CLK_EMMC,
		.name = "emmc",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 12,
		.base_values = emmc7581_base,
		.n_base_values = ARRAY_SIZE(emmc7581_base),
	}
};

static const struct airoha_clk_desc an7583_base_clks[EN7583_MAX_CLKS] = {
	[EN7523_CLK_GSW] = {
		.id = EN7523_CLK_GSW,
		.name = "gsw",

		.base_reg = REG_GSW_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = gsw7583_base,
		.n_base_values = ARRAY_SIZE(gsw7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_EMI] = {
		.id = EN7523_CLK_EMI,
		.name = "emi",

		.base_reg = REG_EMI_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = emi7583_base,
		.n_base_values = ARRAY_SIZE(emi7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_BUS] = {
		.id = EN7523_CLK_BUS,
		.name = "bus",

		.base_reg = REG_BUS_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = bus7583_base,
		.n_base_values = ARRAY_SIZE(bus7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_SLIC] = {
		.id = EN7523_CLK_SLIC,
		.name = "slic",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 1,
		.base_values = slic_base,
		.n_base_values = ARRAY_SIZE(slic_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 24,
		.div_val0 = 20,
		.div_step = 2,
	},
	[EN7523_CLK_SPI] = {
		.id = EN7523_CLK_SPI,
		.name = "spi",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = spi7583_base,
		.n_base_values = ARRAY_SIZE(spi7583_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 8,
		.div_val0 = 40,
		.div_step = 2,
	},
	[EN7523_CLK_NPU] = {
		.id = EN7523_CLK_NPU,
		.name = "npu",

		.base_reg = REG_NPU_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 9,
		.base_values = npu7583_base,
		.n_base_values = ARRAY_SIZE(npu7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	},
	[EN7523_CLK_CRYPTO] = {
		.id = EN7523_CLK_CRYPTO,
		.name = "crypto",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = crypto7583_base,
		.n_base_values = ARRAY_SIZE(crypto7583_base),
	},
	[EN7581_CLK_EMMC] = {
		.id = EN7581_CLK_EMMC,
		.name = "emmc",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 13,
		.base_values = emmc7583_base,
		.n_base_values = ARRAY_SIZE(emmc7583_base),
	},
	[AN7583_CLK_MDIO0] = {
		.id = AN7583_CLK_MDIO0,
		.name = "mdio0",

		.base_reg = REG_CRYPTO_CLKSRC2,

		.base_value = 25000000,

		.div_bits = 4,
		.div_shift = 15,
		.div_step = 1,
		.div_offset = 1,
	},
	[AN7583_CLK_MDIO1] = {
		.id = AN7583_CLK_MDIO1,
		.name = "mdio1",

		.base_reg = REG_CRYPTO_CLKSRC2,

		.base_value = 25000000,

		.div_bits = 4,
		.div_shift = 19,
		.div_step = 1,
		.div_offset = 1,
	}
};

static u32 airoha_clk_get_base_rate(const struct airoha_clk_desc *desc, u32 val)
{
	if (!desc->base_bits)
		return desc->base_value;

	val >>= desc->base_shift;
	val &= (1 << desc->base_bits) - 1;

	if (val >= desc->n_base_values)
		return 0;

	return desc->base_values[val];
}

static u32 airoha_clk_get_div(const struct airoha_clk_desc *desc, u32 val)
{
	if (!desc->div_bits)
		return 1;

	val >>= desc->div_shift;
	val &= (1 << desc->div_bits) - 1;

	if (!val && desc->div_val0)
		return desc->div_val0;

	return (val + desc->div_offset) * desc->div_step;
}

static ulong airoha_econet_clk_get_rate(struct airoha_clk_priv *priv,
					unsigned int id)
{
	const struct airoha_econet_clk_data *data = priv->data->econet;
	u32 val, rate, div, pkg;
	int ret;

	switch (id) {
	case EN751221_CLK_PCIE:
		/* Linux models this as a gate without a rate parent. */
		return 0;
	case EN751221_CLK_SPI:
		rate = data->spi_base;
		if (data->spi_alt_base) {
			ret = regmap_read(priv->scu_map, REG_HIR, &pkg);
			if (!ret) {
				pkg = (pkg & REG_HIR_MASK) >> 16;
				if (pkg == data->spi_alt_pkg)
					rate = data->spi_alt_base;
			}
		}

		ret = regmap_read(priv->chip_scu_map, data->spi_div_reg, &val);
		if (ret)
			return 0;
		div = (val & data->spi_div_mask) >> __ffs(data->spi_div_mask);
		div *= 2;
		if (!div)
			div = data->spi_div_default;
		return rate / div;
	case EN751221_CLK_BUS:
		ret = regmap_read(priv->scu_map, EN751221_REG_BUS, &val);
		if (ret)
			return 0;
		return ((val & EN751221_REG_BUS_MASK) >> 12) * 1000000UL;
	case EN751221_CLK_CPU:
		rate = airoha_econet_clk_get_rate(priv, EN751221_CLK_BUS);
		return rate * 4;
	case EN751221_CLK_GSW:
		ret = regmap_read(priv->scu_map, EN751221_REG_SSR3, &val);
		if (ret)
			return 0;
		val = (val & EN751221_REG_SSR3_GSW_MASK) >> 8;
		if (val >= ARRAY_SIZE(gsw751221_base))
			return 0;
		return gsw751221_base[val];
	default:
		return 0;
	}
}

static int airoha_econet_pcie_enable(struct airoha_clk_priv *priv)
{
	u32 mask;
	int ret;

	ret = regmap_clear_bits(priv->scu_map, REG_PCI_CONTROL,
				REG_PCI_CONTROL_PERSTOUT1 |
				REG_PCI_CONTROL_PERSTOUT);
	if (ret)
		return ret;

	udelay(1000);
	ret = regmap_set_bits(priv->scu_map, REG_PCI_CONTROL,
			      REG_PCI_CONTROL_REFCLK_EN1);
	if (ret)
		return ret;
	udelay(1000);

	mask = REG_RESET_CONTROL_PCIE1 | REG_RESET_CONTROL_PCIE0 |
	       REG_RESET_CONTROL_PCIEHB;
	ret = regmap_clear_bits(priv->scu_map, REG_RESET_CONTROL1, mask);
	if (ret)
		return ret;

	udelay(1000);
	ret = regmap_set_bits(priv->scu_map, REG_RESET_CONTROL1, mask);
	if (ret)
		return ret;

	mdelay(100);
	ret = regmap_clear_bits(priv->scu_map, REG_RESET_CONTROL1, mask);
	if (ret)
		return ret;

	udelay(5000);
	mask = REG_PCI_CONTROL_PERSTOUT1 | REG_PCI_CONTROL_PERSTOUT;
	ret = regmap_clear_bits(priv->scu_map, REG_PCI_CONTROL, mask);
	if (ret)
		return ret;

	udelay(1000);
	ret = regmap_set_bits(priv->scu_map, REG_PCI_CONTROL, mask);
	if (ret)
		return ret;

	mdelay(250);
	return 0;
}

static int airoha_clk_enable(struct clk *clk)
{
	struct airoha_clk_priv *priv = dev_get_priv(clk->dev);
	struct airoha_clk_soc_data *data = priv->data;
	int id = clk->id;

	if (id >= data->num_clocks)
		return -EINVAL;

	if (data->econet && id == EN751221_CLK_PCIE)
		return airoha_econet_pcie_enable(priv);

	return 0;
}

static int airoha_clk_disable(struct clk *clk)
{
	struct airoha_clk_priv *priv = dev_get_priv(clk->dev);

	if (priv->data->econet && clk->id == EN751221_CLK_PCIE)
		return regmap_clear_bits(priv->scu_map, REG_PCI_CONTROL,
					 REG_PCI_CONTROL_REFCLK_EN1);

	return 0;
}

static ulong airoha_clk_get_rate(struct clk *clk)
{
	struct airoha_clk_priv *priv = dev_get_priv(clk->dev);
	struct airoha_clk_soc_data *data = priv->data;
	const struct airoha_clk_desc *desc;
	struct regmap *map = priv->chip_scu_map;
	int id = clk->id;
	u32 reg, val;
	ulong rate;
	int ret;

	if (id >= data->num_clocks) {
		dev_err(clk->dev, "Invalid clk ID %d\n", id);
		return 0;
	}

	if (data->econet)
		return airoha_econet_clk_get_rate(priv, id);

	desc = &data->descs[id];

	ret = regmap_read(map, desc->base_reg, &val);
	if (ret) {
		dev_err(clk->dev, "Failed to read reg for clock %s\n",
			desc->name);
		return 0;
	}

	rate = airoha_clk_get_base_rate(desc, val);

	reg = desc->div_reg ? desc->div_reg : desc->base_reg;
	ret = regmap_read(map, reg, &val);
	if (ret) {
		dev_err(clk->dev, "Failed to read reg for clock %s\n",
			desc->name);
		return 0;
	}

	rate /= airoha_clk_get_div(desc, val);

	return rate;
}

static int airoha_clk_search_rate(const struct airoha_clk_desc *desc, int div,
				  ulong rate)
{
	int i;

	/* Single base rate */
	if (!desc->base_bits) {
		if (rate != desc->base_value / div)
			goto err;

		return 0;
	}

	/* Check every base rate with provided divisor */
	for (i = 0; i < desc->n_base_values; i++)
		if (rate == desc->base_values[i] / div)
			return i;

err:
	return -EINVAL;
}

static ulong airoha_clk_set_rate(struct clk *clk, ulong rate)
{
	struct airoha_clk_priv *priv = dev_get_priv(clk->dev);
	struct airoha_clk_soc_data *data = priv->data;
	const struct airoha_clk_desc *desc;
	struct regmap *map = priv->chip_scu_map;
	int div_val, base_val;
	u32 reg, val, mask;
	int id = clk->id;
	int div;
	int ret;

	if (id >= data->num_clocks) {
		dev_err(clk->dev, "Invalid clk ID %d\n", id);
		return 0;
	}

	if (data->econet) {
		ulong current = airoha_econet_clk_get_rate(priv, id);

		/* Linux exposes these clocks as fixed-rate after strap/divider setup. */
		return current == rate ? current : 0;
	}

	desc = &data->descs[id];

	if (!desc->base_bits && !desc->div_bits) {
		dev_err(clk->dev, "Can't set rate for fixed clock %s\n",
			desc->name);
		return 0;
	}

	if (!desc->div_bits) {
		/* Divisor not supported, just search in base rate */
		div_val = 0;
		base_val = airoha_clk_search_rate(desc, 1, rate);
		if (base_val < 0) {
			dev_err(clk->dev, "Invalid rate for clock %s\n",
				desc->name);
			return 0;
		}
	} else {
		div_val = 0;

		/* Check if div0 satisfy the request */
		if (desc->div_val0) {
			base_val = airoha_clk_search_rate(desc, desc->div_val0,
							  rate);
			if (base_val >= 0) {
				div_val = 0;
				goto apply;
			}

			/* Skip checking first divisor val */
			div_val = 1;
		}

		/* Simulate rate with every divisor supported */
		for (div_val = div_val + desc->div_offset;
		     div_val < BIT(desc->div_bits) - 1; div_val++) {
			div = div_val * desc->div_step;

			base_val = airoha_clk_search_rate(desc, div, rate);
			if (base_val >= 0)
				break;
		}

		if (div_val == BIT(desc->div_bits) - 1) {
			dev_err(clk->dev, "Invalid rate for clock %s\n",
				desc->name);
			return 0;
		}
	}

apply:
	if (desc->div_bits) {
		reg = desc->div_reg ? desc->div_reg : desc->base_reg;

		mask = (BIT(desc->div_bits) - 1) << desc->div_shift;
		val = div_val << desc->div_shift;

		ret = regmap_update_bits(map, reg, mask, val);
		if (ret) {
			dev_err(clk->dev, "Failed to update div reg for clock %s\n",
				desc->name);
			return 0;
		}
	}

	if (desc->base_bits) {
		mask = (BIT(desc->base_bits) - 1) << desc->base_shift;
		val = base_val << desc->base_shift;

		ret = regmap_update_bits(map, desc->base_reg, mask, val);
		if (ret) {
			dev_err(clk->dev, "Failed to update reg for clock %s\n",
				desc->name);
			return 0;
		}
	}

	return rate;
}

const struct clk_ops airoha_clk_ops = {
	.enable = airoha_clk_enable,
	.disable = airoha_clk_disable,
	.get_rate = airoha_clk_get_rate,
	.set_rate = airoha_clk_set_rate,
};

static int airoha_clk_probe(struct udevice *dev)
{
	struct airoha_clk_priv *priv = dev_get_priv(dev);

	priv->chip_scu_map = airoha_get_chip_scu_regmap();
	if (IS_ERR(priv->chip_scu_map))
		return PTR_ERR(priv->chip_scu_map);

	priv->scu_map = airoha_get_scu_regmap();
	if (IS_ERR(priv->scu_map))
		return PTR_ERR(priv->scu_map);

	priv->data = (void *)dev_get_driver_data(dev);
	if (priv->data->econet) {
		const struct airoha_econet_clk_data *data = priv->data->econet;
		int ret;

		ret = regmap_set_bits(priv->chip_scu_map, data->xpon_tod_clk_reg,
				      data->xpon_tod_clk_mask);
		if (ret)
			return ret;
		ret = regmap_set_bits(priv->chip_scu_map, data->xpon_tod_div_reg,
				      data->xpon_tod_div_mask);
		if (ret)
			return ret;
	}

	return 0;
}

static int airoha_clk_bind(struct udevice *dev)
{
	struct udevice *rst_dev;
	int ret = 0;

	if (CONFIG_IS_ENABLED(RESET_AIROHA)) {
		ret = device_bind_driver_to_node(dev, "airoha-reset", "reset",
						 dev_ofnode(dev), &rst_dev);
		if (ret)
			debug("Warning: failed to bind reset controller\n");
	}

	return ret;
}

static const struct airoha_clk_soc_data en7523_data = {
	.num_clocks = ARRAY_SIZE(en7523_base_clks),
	.descs = en7523_base_clks,
};

static const struct airoha_clk_soc_data en7581_data = {
	.num_clocks = ARRAY_SIZE(en7581_base_clks),
	.descs = en7581_base_clks,
};

static const struct airoha_clk_soc_data an7583_data = {
	.num_clocks = ARRAY_SIZE(an7583_base_clks),
	.descs = an7583_base_clks,
};

static const struct airoha_econet_clk_data en751221_econet_data = {
	.spi_base = EN751221_SPI_BASE,
	.spi_alt_base = EN751221_SPI_BASE_EN7526C,
	.spi_alt_pkg = EN7526C_PKG,
	.spi_div_reg = EN751221_REG_SPI_DIV,
	.spi_div_mask = EN751221_REG_SPI_DIV_MASK,
	.spi_div_default = EN751221_SPI_DIV_DEFAULT,
	.xpon_tod_clk_reg = EN751221_REG_NP_PER_DOM_CLK_GAT_1,
	.xpon_tod_clk_mask = EN751221_XPON_TOD_CLK_EN,
	.xpon_tod_div_reg = EN751221_REG_TOD_DIVIDER_ENABLE,
	.xpon_tod_div_mask = EN751221_XPON_TOD_DIV_EN,
};

static const struct airoha_clk_soc_data en751221_data = {
	.num_clocks = EN751221_MAX_CLKS,
	.econet = &en751221_econet_data,
};

static const struct airoha_econet_clk_data en7528_econet_data = {
	.spi_base = EN7528_SPI_BASE,
	.spi_div_reg = EN7528_REG_SPI_DIV,
	.spi_div_mask = EN7528_REG_SPI_DIV_MASK,
	.spi_div_default = EN7528_SPI_DIV_DEFAULT,
	.xpon_tod_clk_reg = EN7528_REG_NP_PER_DOM_CLK_GAT_1,
	.xpon_tod_clk_mask = EN7528_XPON_TOD_CLK_EN,
	.xpon_tod_div_reg = EN7528_REG_TOD_DIVIDER_ENABLE,
	.xpon_tod_div_mask = EN7528_XPON_TOD_DIV_EN,
};

static const struct airoha_clk_soc_data en7528_data = {
	.num_clocks = EN7528_MAX_CLKS,
	.econet = &en7528_econet_data,
};

static const struct udevice_id airoha_clk_ids[] = {
	{ .compatible = "airoha,en751221-scu",
	  .data = (ulong)&en751221_data,
	},
	{ .compatible = "airoha,en7528-scu",
	  .data = (ulong)&en7528_data,
	},
	{ .compatible = "airoha,en7523-scu",
	  .data = (ulong)&en7523_data,
	},
	{ .compatible = "airoha,en7581-scu",
	  .data = (ulong)&en7581_data,
	},
	{ .compatible = "airoha,an7583-scu",
	  .data = (ulong)&an7583_data,
	},
	{ }
};

U_BOOT_DRIVER(airoha_clk) = {
	.name = "clk-airoha",
	.id = UCLASS_CLK,
	.of_match = airoha_clk_ids,
	.probe = airoha_clk_probe,
	.bind = airoha_clk_bind,
	.priv_auto = sizeof(struct airoha_clk_priv),
	.ops = &airoha_clk_ops,
};
