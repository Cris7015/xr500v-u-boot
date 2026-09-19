// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Based on Linux airoha_eth.c majorly rewritten
 * and simplified for U-Boot usage for single TX/RX ring.
 *
 * Copyright (c) 2024 AIROHA Inc
 * EN751221 support based on econet_eth by Caleb James DeLisle.
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 *         Christian Marangi <ansuelsmth@gmail.org>
 */

#include <dm.h>
#include <dm/device-internal.h>
#include <dm/devres.h>
#include <dm/lists.h>
#include <hexdump.h>
#include <mapmem.h>
#include <malloc.h>
#include <miiphy.h>
#include <net.h>
#include <regmap.h>
#include <reset.h>
#include <syscon.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mii.h>
#include <linux/mdio.h>
#include <linux/time.h>
#include <asm/system.h>
#include <soc/airoha/pkgids.h>
#include <soc/airoha/scu-regmap.h>
#if IS_ENABLED(CONFIG_ARCH_ECONET)
#include <asm/addrspace.h>
#define EN7528_RX_UNCACHED(_p) \
	((uchar *)KSEG1ADDR((uintptr_t)(_p)))
#else
#define EN7528_RX_UNCACHED(_p)	(_p)
#endif

#include "airoha/pcs-airoha.h"

#define AIROHA_MAX_NUM_GDM_PORTS	4
#define AIROHA_MAX_NUM_QDMA		1
#define AIROHA_MAX_NUM_RSTS		4
#define AIROHA_MAX_NUM_XSI_RSTS		4

#define AIROHA_MAX_NUM_SWITCH_PORT	4
#define AIROHA_MAX_PBUS_TRY		10
#define AIROHA_PBUS_SLEEP		100
#define AIROHA_PBUS_C22_MASK		0x800000

#define AIROHA_MAX_PACKET_SIZE		2048
#define AIROHA_NUM_TX_RING		1
#define AIROHA_NUM_RX_RING		1
#define AIROHA_NUM_TX_IRQ		1
#define HW_DSCP_NUM			32
#define IRQ_QUEUE_LEN			1
#define TX_DSCP_NUM			16
#define RX_DSCP_NUM			PKTBUFSRX

#define AIROHA_GDM_PORT_STRING_LEN	sizeof("airoha-gdmX")

/* SCU */
#define SCU_SHARE_FEMEM_SEL		0x958

/* SWITCH */
#define SWITCH_MFC			0x10
#define   SWITCH_BC_FFP			GENMASK(31, 24)
#define   SWITCH_UNM_FFP		GENMASK(23, 16)
#define   SWITCH_UNU_FFP		GENMASK(15, 8)
#define   SWITCH_CPU_EN			BIT(7)
#define   SWITCH_CPU_PORT		GENMASK(6, 4)
#define SWITCH_APC			0x20
#define SWITCH_ATC			0x80
#define   SWITCH_ATC_CMD_MASK		GENMASK(2, 0)
#define   SWITCH_ATC_SEARCH_HIT	BIT(13)
#define   SWITCH_ATC_SEARCH_END	BIT(14)
#define   SWITCH_ATC_BUSY		BIT(15)
#define   SWITCH_ATC_ADDR_MASK		GENMASK(27, 16)
#define SWITCH_TSRA1			0x84
#define SWITCH_TSRA2			0x88
#define SWITCH_ATRD			0x8c
#define   SWITCH_ATRD_PORT_MASK	GENMASK(11, 4)
#define   SWITCH_APC_ARP_PORT_FW	GENMASK(2, 0)
#define   SWITCH_APC_ARP_MANG_FR	BIT(11)
#define   SWITCH_APC_PPP_PORT_FW	GENMASK(18, 16)
#define   SWITCH_APC_PPP_MANG_FR	BIT(27)
#define SWITCH_PCR(_n)			0x2004 + ((_n) * 0x100)
#define   SWITCH_PCR_MATRIX		GENMASK(23, 16)
#define   SWITCH_PCR_PORT_VLAN	GENMASK(1, 0)
#define     SWITCH_PORT_FALLBACK_MODE	FIELD_PREP(SWITCH_PCR_PORT_VLAN, 1)
#define SWITCH_PSC(_n)			0x200c + ((_n) * 0x100)
#define SWITCH_PVC(_n)			0x2010 + ((_n) * 0x100)
#define SWITCH_PPBV1(_n)		0x2014 + ((_n) * 0x100)
#define   SWITCH_PVC_STAG_EN		BIT(5)
#define SWITCH_PMCR(_n)			0x3000 + ((_n) * 0x100)
#define SWITCH_PMSR(_n)			0x3008 + ((_n) * 0x100)
#define   SWITCH_IPG_CFG		GENMASK(19, 18)
#define     SWITCH_IPG_CFG_NORMAL	FIELD_PREP(SWITCH_IPG_CFG, 0x0)
#define     SWITCH_IPG_CFG_SHORT	FIELD_PREP(SWITCH_IPG_CFG, 0x1)
#define     SWITCH_IPG_CFG_SHRINK	FIELD_PREP(SWITCH_IPG_CFG, 0x2)
#define   SWITCH_MAC_MODE		BIT(16)
#define   SWITCH_FORCE_MODE		BIT(15)
#define   SWITCH_MAC_TX_EN		BIT(14)
#define   SWITCH_MAC_RX_EN		BIT(13)
#define   SWITCH_BKOFF_EN		BIT(9)
#define   SWITCH_BKPR_EN		BIT(8)
#define   SWITCH_FORCE_RX_FC		BIT(5)
#define   SWITCH_FORCE_TX_FC		BIT(4)
#define   SWITCH_FORCE_SPD		GENMASK(3, 2)
#define     SWITCH_FORCE_SPD_10		FIELD_PREP(SWITCH_FORCE_SPD, 0x0)
#define     SWITCH_FORCE_SPD_100	FIELD_PREP(SWITCH_FORCE_SPD, 0x1)
#define     SWITCH_FORCE_SPD_1000	FIELD_PREP(SWITCH_FORCE_SPD, 0x2)
#define   SWITCH_FORCE_DPX		BIT(1)
#define   SWITCH_FORCE_LNK		BIT(0)
#define SWITCH_MIB_CCR			0x4fe0
#define   SWITCH_MIB_ENABLE		BIT(31)
#define   SWITCH_MIB_OCT_CNT_MASK	GENMASK(7, 4)
#define SWITCH_MIB_PORT_BASE(_p)	(0x4000 + ((_p) * 0x100))
#define SWITCH_TX_DROC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x00)
#define SWITCH_TX_UNIC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x08)
#define SWITCH_TX_MULC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x0c)
#define SWITCH_TX_BROC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x10)
#define SWITCH_RX_DROC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x60)
#define SWITCH_RX_UNIC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x68)
#define SWITCH_RX_MULC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x6c)
#define SWITCH_RX_BROC(_p)		(SWITCH_MIB_PORT_BASE(_p) + 0x70)
#define SWITCH_SMACCR0			0x30e4
#define   SMACCR0_MAC2			GENMASK(31, 24)
#define   SMACCR0_MAC3			GENMASK(23, 16)
#define   SMACCR0_MAC4			GENMASK(15, 8)
#define   SMACCR0_MAC5			GENMASK(7, 0)
#define SWITCH_SMACCR1			0x30e8
#define   SMACCR1_MAC0			GENMASK(15, 8)
#define   SMACCR1_MAC1			GENMASK(7, 0)
#define SWITCH_SYS_CTRL			0x7000
#define SWITCH_PHY_POLL			0x7018
#define SWITCH_PHY_IAC			0x701c
#define   SWITCH_PHY_ACS_ST		BIT(31)
#define   SWITCH_MDIO_REG_ADDR		GENMASK(29, 25)
#define   SWITCH_MDIO_PHY_ADDR		GENMASK(24, 20)
#define   SWITCH_MDIO_CMD		GENMASK(19, 18)
#define     SWITCH_MDIO_CMD_WRITE	FIELD_PREP_CONST(SWITCH_MDIO_CMD, 1)
#define     SWITCH_MDIO_CMD_READ	FIELD_PREP_CONST(SWITCH_MDIO_CMD, 2)
#define   SWITCH_MDIO_ST		GENMASK(17, 16)
#define     SWITCH_MDIO_ST_CL22	FIELD_PREP_CONST(SWITCH_MDIO_ST, 1)
#define   SWITCH_MDIO_DATA		GENMASK(15, 0)
#define   SWITCH_PHY_AP_EN		GENMASK(30, 24)
#define   SWITCH_EEE_POLL_EN		GENMASK(22, 16)
#define   SWITCH_PHY_PRE_EN		BIT(15)
#define   SWITCH_PHY_END_ADDR		GENMASK(12, 8)
#define   SWITCH_PHY_ST_ADDR		GENMASK(4, 0)
#define SWITCH_GEPHY_CONN_CFG		0x7c14
#define   SWITCH_DPHY_CKIN_SEL		BIT(31)
#define   SWITCH_PHY_CORE_REG_CLK_SEL	BIT(30)
#define   SWITCH_ETHER_AFE_PWD		GENMASK(28, 24)
#define SWITCH_PBUS_PHY_IAC		0x7c20
#define   SWITCH_PBUS_PHY_START		BIT(31)
#define   SWITCH_PBUS_PHY_CMD		BIT(30)
#define   SWITCH_PBUS_PHY_CMD_READ	FIELD_PREP(SWITCH_PBUS_PHY_CMD, 0x0)
#define   SWITCH_PBUS_PHY_CMD_WRITE	FIELD_PREP(SWITCH_PBUS_PHY_CMD, 0x1)
#define   SWITCH_PBUS_PHY_PORTADDR	GENMASK(28, 24)
#define   SWITCH_PBUS_PHY_REGADDR	GENMASK(23, 0)
#define SWITCH_PBUS_PHY_IAWD		0x7c24
#define SWITCH_PBUS_PHY_IARD		0x7c28

/* EN7528 TCBoot switch state captured at the boot command prompt. */
#define EN7528_TCBOOT_MFC		0x404040e0
#define EN7528_VENDOR_APC		0x08100810
#define EN7528_TCBOOT_PCR		0x00ff0000
#define EN7528_TCBOOT_PSC		0x000fff00
#define EN7528_TCBOOT_PVC		0x810000c0
#define EN7528_TCBOOT_PPBV1		0x00010001
#define EN7528_TCBOOT_PMCR_USER	0x00056300
#define EN7528_TCBOOT_PMCR6		0x0005e30b
#define EN7528_TCBOOT_GEPHY_CONN_CFG	0x800000f0

/* FE */
#define PSE_BASE			0x0100
#define CSR_IFC_BASE			0x0200
#define CDM1_BASE			0x0400
#define GDM1_BASE			0x0500
#define PPE1_BASE			0x0c00

#define CDM2_BASE			0x1400
#define GDM2_BASE			0x1500

#define GDM3_BASE			0x1100
#define GDM4_BASE			0x2500

#define GDM_BASE(_n)			\
	((_n) == 4 ? GDM4_BASE :	\
	 (_n) == 3 ? GDM3_BASE :	\
	 (_n) == 2 ? GDM2_BASE : GDM1_BASE)

#define REG_GDM_FWD_CFG(_n)		GDM_BASE(_n)
#define REG_GDM_VLAN_GEN(_n)		(GDM_BASE(_n) + 0x10)
#define REG_FE_CPORT_CFG		(GDM1_BASE + 0x40)
#define   FE_CPORT_PAD_EN		BIT(26)
#define   FE_CPORT_MODE_MASK		GENMASK(25, 24)
#define   FE_CPORT_MODE_EN751627	FIELD_PREP(FE_CPORT_MODE_MASK, 2)
#define REG_FE_CPORT_CHN_MAP		(GDM1_BASE + 0x44)
#define EN7528_TCBOOT_GDM_FWD_CFG	0xc0000000
#define EN7528_TCBOOT_CDM_CSG_CFG	0x81000000
#define EN7528_TCBOOT_GDM_VLAN_GEN	0x81000000
#define EN7528_TCBOOT_GDM_RX_LEN_THR	0x05ea003c
#define EN7528_TCBOOT_CPORT_CFG	0x70000a02
#define EN7528_TCBOOT_CPORT_CHN_MAP	0x76543210
#define REG_CDM_CSG_CFG		CDM1_BASE
#define   CDM_STAG_EN			BIT(0)
#define REG_GDM_MAC_LSB(_n)		(GDM_BASE(_n) + 0x08)
#define REG_GDM_MAC_MSB(_n)		(GDM_BASE(_n) + 0x0c)
#define REG_GDM_RX_LEN_THR(_n)		(GDM_BASE(_n) + 0x14)
#define REG_GDM_TXCHN_EN(_n)		(GDM_BASE(_n) + 0x24)
#define REG_GDM_RXCHN_EN(_n)		(GDM_BASE(_n) + 0x28)
#define REG_GDM_TX_CHN_VLD(_n)		(GDM_BASE(_n) + 0x70)
#define REG_GDM_RX_CHN_VLD(_n)		(GDM_BASE(_n) + 0x74)
#define REG_CDM1_HWF_CHN_EN		(CDM1_BASE + 0x0c)
#define REG_CDM2_HWF_CHN_EN		(CDM2_BASE + 0x0c)
#define REG_CDMA1_RXCPU_OK_CNT		(GDM1_BASE + 0x90)
#define REG_CDMA1_RXHWF_OK_CNT		(GDM1_BASE + 0x94)
#define REG_CDMA1_RXCPU_DROP_CNT	(GDM1_BASE + 0xa0)
#define REG_CDMA1_RXHWF_DROP_CNT	(GDM1_BASE + 0xa4)
#define REG_GDM1_TX_GET_CNT		0x0600
#define REG_GDM1_TX_OK_CNT		0x0604
#define REG_GDM1_TX_DROP_CNT		0x0608
#define REG_GDM1_TX_OK_BYTE_CNT	0x060c
#define GDM_PAD_EN			BIT(28)
#define GDM_DROP_CRC_ERR		BIT(23)
#define GDM_IP4_CKSUM			BIT(22)
#define GDM_TCP_CKSUM			BIT(21)
#define GDM_UDP_CKSUM			BIT(20)
#define GDM_UCFQ_MASK			GENMASK(15, 12)
#define GDM_BCFQ_MASK			GENMASK(11, 8)
#define GDM_MCFQ_MASK			GENMASK(7, 4)
#define GDM_OCFQ_MASK			GENMASK(3, 0)

/* EN7512/EN7521 GDM forwarding configuration */
#define EN751221_GDM_JMB_LEN		GENMASK(31, 28)
#define EN751221_GDM_STAG_EN		BIT(24)
#define EN751221_GDM_DROP_OVERSIZE	BIT(25)
#define EN751221_GDM_MYMAC_FPORT	GENMASK(15, 12)
#define EN751221_GDM_BCAST_FPORT	GENMASK(11, 8)
#define EN751221_GDM_MCAST_FPORT	GENMASK(7, 4)
#define EN751221_GDM_DEFAULT_FPORT	GENMASK(3, 0)
#define EN751221_GDM_OVERSIZE_LEN	GENMASK(31, 16)
#define EN751221_GDM_RUNT_LEN		GENMASK(15, 0)

/* EN7512/EN7521 QDMA register layout */
#define EN751221_REG_TX_RING_BASE	0x0008
#define EN751221_REG_RX_RING_BASE	0x000c
#define EN751221_REG_TX_CPU_IDX		0x0010
#define EN751221_REG_TX_DMA_IDX		0x0014
#define EN751221_REG_RX_CPU_IDX		0x0018
#define EN751221_REG_RX_DMA_IDX		0x001c
#define EN751221_REG_FWD_DSCP_BASE	0x0020
#define EN751221_REG_FWD_BUF_BASE	0x0024
#define EN751221_REG_FWD_DSCP_CFG	0x0028
#define EN751221_REG_LMGR_INIT_CFG	0x0030
#define EN751221_REG_INT_STATUS		0x0050
#define EN751221_REG_INT_ENABLE		0x0054
#define EN751221_REG_RX_DELAY_INT	0x005c
#define EN751221_REG_IRQ_BASE		0x0060
#define EN751221_REG_IRQ_CFG		0x0064
#define EN751221_REG_IRQ_CLEAR_LEN	0x0068
#define EN751221_REG_IRQ_STATUS		0x006c
#define EN751221_REG_DBG_LMGR_STATUS	0x00f0
#define EN751221_REG_DBG_HWF_BUF_USAGE	0x00f4
#define EN751221_REG_TXQ_CNGST_CFG	0x00a0
#define   EN751221_TXQ_DROP_EN		BIT(31)
#define   EN751221_TXQ_DEI_DROP_EN	BIT(30)
#define   EN751221_TXQ_DYN_THR_EN	BIT(29)
#define   EN751221_TXQ_DYN_ALPHA_MASK	GENMASK(23, 22)
#define   EN751221_TXQ_CNGST_MODE_MASK	GENMASK(21, 20)
#define   EN751221_TXQ_CNGST_RSV_MASK	GENMASK(27, 24)
#define   EN751221_TXQ_CNGST_EN_MASK	GENMASK(18, 16)
#define   EN751221_TXQ_CNGST_THR_MASK	GENMASK(15, 0)
/*
 * qdmaSetQueueClose_sw()/qdmaSetQueueOpen_sw() in qdma_dev.c: eight registers,
 * one byte per channel, one bit per queue.  A set bit closes that queue.
 * 32 channels x 8 queues is exactly the 256-descriptor pool U-Boot allocates.
 */
#define EN751221_REG_TXQ_DIS_CFG(_n)	(0x01a0 + ((_n) << 2))
#define EN751221_NUM_TXQ_DIS_CFG	8
#define EN751221_REG_TXQ_DYN_TOTALTHR	0x00a4
#define EN751221_REG_TXQ_DYN_CHNLTHR	0x00a8
#define EN751221_REG_TXQ_DYN_QUEUETHR	0x00ac
#define   EN751221_TXQ_THR_MAX_MASK	GENMASK(31, 16)
#define   EN751221_TXQ_THR_MIN_MASK	GENMASK(15, 0)
/*
 * qdma_set_txq_cngst_auto_config() picks these when hwDscpNum <= 256, which is
 * the pool size U-Boot uses.  Larger pools get 0x500 / num/28 / num/12 instead.
 */
#define EN751221_TXQ_TOTAL_MAX		0x3c00
#define EN751221_TXQ_TOTAL_MIN		0x30
#define EN751221_TXQ_CHNL_MAX		0x3c00
#define EN751221_TXQ_CHNL_MIN		2
#define EN751221_TXQ_QUEUE_MAX		0x30
#define EN751221_TXQ_QUEUE_MIN		2
#define EN751221_TXQ_CNGST_THR		250
#define EN751221_REG_RX_RING_SIZE	0x0100
#define EN751221_REG_RX_RING_THR		0x0104
#define EN751221_REG_RX_PROTECT_CFG	0x0120
#define   EN751221_RX_PROTECT_EN	BIT(31)

#define EN751221_RING_IDX_MASK		GENMASK(11, 0)
#define EN751221_RING_SIZE_MASK		GENMASK(11, 0)
#define EN751221_RING_THR_MASK		GENMASK(11, 0)
#define EN751221_IRQ_DEPTH_MASK		GENMASK(11, 0)
#define EN751221_IRQ_THRESHOLD_MASK	GENMASK(27, 16)
#define EN751221_IRQ_HEAD_IDX_MASK	GENMASK(15, 0)
#define EN751221_IRQ_ENTRY_LEN_MASK	GENMASK(31, 16)
#define EN751221_IRQ_CLEAR_LEN_MASK	GENMASK(7, 0)
#define EN751221_HWFWD_LOW_THR_MASK	GENMASK(12, 0)
#define EN751221_HWFWD_DESC_NUM_MASK	GENMASK(12, 0)
#define EN751221_LMGR_FREE_MASK		GENMASK(12, 0)
#define EN751221_HWFWD_OVERHEAD_MASK	GENMASK(23, 16)
#define EN751221_HWFWD_OVERHEAD_EN	BIT(24)
#define EN751221_GLOBAL_CFG_MSG_WORD_SWAP BIT(28)
#define EN7528_GLOBAL_CFG_TX_IMMEDIATE_DONE BIT(20)

#define EN751221_TXMSG_CHANNEL_MASK	GENMASK(10, 3)
#define EN751221_TXMSG_QUEUE_MASK	GENMASK(2, 0)
#define EN751221_TXMSG_FPORT_MASK	GENMASK(21, 19)
#define EN751221_FPORT_GDM1		1

/*
 * EN7528 is grouped with EN751627 by the vendor SDK.  Their little-endian
 * LAN TX word places the three-bit destination port at bits [21:19].
 */
#define EN751221_HW_DSCP_NUM		256
#define EN751221_HWFWD_LOW_THRESHOLD	32
#define EN751221_HWFWD_OVERHEAD	0x14
#define EN751221_IRQ_QUEUE_LEN		256
#define EN751221_IRQ_THRESHOLD		1

/* EN7528 TCBoot QDMA0 profile captured before Linux is started. */
#define EN7528_RX_DSCP_NUM		16
#define EN7528_RX_BUF_LEN		0x5ee
#define EN7528_RX_PKT_OFFSET		2
#define EN7528_HW_DSCP_NUM		32
#define EN7528_HWFWD_LOW_THRESHOLD	1
#define EN7528_HWFWD_OVERHEAD		0x18
#define EN7528_IRQ_QUEUE_LEN		32
#define EN7528_IRQ_THRESHOLD		0
#define EN7528_INT_ENABLE		0x0000378a
#define EN7528_TXQ_CNGST_CFG		0xa10700fa
#define EN7528_TXQ_TOTAL_THR		0xffff02c0
#define EN7528_TXQ_CHNL_THR		0xffff0016
#define EN7528_TXQ_QUEUE_THR		0xffff0002
#define EN7528_RX_PROTECT_CFG		0x2000007d
#define EN751221_RESET_CONTROL2		0xbfb00834
#define EN751221_FE_SRAM_SEL		0xbfb00958
#define MT7530_PHY_ECO_REV_REG		0x0000
#define MT7530_PHY_ECO_REV_MASK		GENMASK(7, 4)
#define MT7530_CHIP_REV			0x7ffc
#define MT7530_CHIP_ID_MASK		GENMASK(31, 16)
#define MT7530_CHIP_ID			0x75300000
#define MT7530_SYS_CTRL			0x7000
#define MT7530_FC_CTRL			0x1fe0
#define MT7530_HWTRAP			0x7800
#define MT7530_MHWTRAP			0x7804
#define MT7530_P6ECR			0x7830
#define MT7530_TRGMII_CLK_CG		0x7808
#define MT7530_TRGMII_CTRL		0x7a00
#define MT7530_TRGMII_RCK_CTRL		0x7a40
#define MT7530_TRGMII_TD_ODT(_n)	(0x7a54 + ((_n) * 8))
#define MT7530_PBUS_PHY			31
#define MT7530_PBUS_PAGE_REG		31
#define MT7530_PBUS_HIGH_REG		16
#define MT7530_EXT_PHY_NUM		5
#define MT7530_EXT_PHY_ID1		0x03a2
#define MT7530_EXT_PHY_ID2		0x9412

/*
 * U-Boot uses plain Ethernet frames across the EN751221 MCM cascade,
 * without an MTK DSA special tag. Flood broadcast and unknown traffic
 * from the upstream P6 to all five LAN ports.
 */
#define EN751221_EXT_MFC		0x5f5f5fe0
#define EN751221_RST_QDMA0		BIT(1)
#define EN751221_RST_QDMA1		BIT(2)
#define EN751221_RST_FE			BIT(21)
#define EN751221_RST_GSW		BIT(23)

/* QDMA */
#define REG_QDMA_GLOBAL_CFG			0x0004
#define GLOBAL_CFG_RX_2B_OFFSET_MASK		BIT(31)
#define GLOBAL_CFG_DMA_PREFERENCE_MASK		GENMASK(30, 29)
#define GLOBAL_CFG_CPU_TXR_RR_MASK		BIT(28)
#define GLOBAL_CFG_DSCP_BYTE_SWAP_MASK		BIT(27)
#define GLOBAL_CFG_PAYLOAD_BYTE_SWAP_MASK	BIT(26)
#define GLOBAL_CFG_MULTICAST_MODIFY_FP_MASK	BIT(25)
#define GLOBAL_CFG_OAM_MODIFY_MASK		BIT(24)
#define GLOBAL_CFG_RESET_MASK			BIT(23)
#define GLOBAL_CFG_RESET_DONE_MASK		BIT(22)
#define GLOBAL_CFG_MULTICAST_EN_MASK		BIT(21)
#define GLOBAL_CFG_IRQ1_EN_MASK			BIT(20)
#define GLOBAL_CFG_IRQ0_EN_MASK			BIT(19)
#define GLOBAL_CFG_LOOPCNT_EN_MASK		BIT(18)
#define GLOBAL_CFG_RD_BYPASS_WR_MASK		BIT(17)
#define GLOBAL_CFG_QDMA_LOOPBACK_MASK		BIT(16)
#define GLOBAL_CFG_LPBK_RXQ_SEL_MASK		GENMASK(13, 8)
#define GLOBAL_CFG_CHECK_DONE_MASK		BIT(7)
#define GLOBAL_CFG_TX_WB_DONE_MASK		BIT(6)
#define GLOBAL_CFG_MAX_ISSUE_NUM_MASK		GENMASK(5, 4)
#define GLOBAL_CFG_RX_DMA_BUSY_MASK		BIT(3)
#define GLOBAL_CFG_RX_DMA_EN_MASK		BIT(2)
#define GLOBAL_CFG_TX_DMA_BUSY_MASK		BIT(1)
#define GLOBAL_CFG_TX_DMA_EN_MASK		BIT(0)

#define REG_FWD_DSCP_BASE			0x0010
#define REG_FWD_BUF_BASE			0x0014

#define REG_HW_FWD_DSCP_CFG			0x0018
#define HW_FWD_DSCP_PAYLOAD_SIZE_MASK		GENMASK(29, 28)
#define HW_FWD_DSCP_SCATTER_LEN_MASK		GENMASK(17, 16)
#define HW_FWD_DSCP_MIN_SCATTER_LEN_MASK	GENMASK(15, 0)

#define REG_INT_STATUS(_n)		\
	(((_n) == 4) ? 0x0730 :		\
	 ((_n) == 3) ? 0x0724 :		\
	 ((_n) == 2) ? 0x0720 :		\
	 ((_n) == 1) ? 0x0024 : 0x0020)

#define REG_TX_IRQ_BASE(_n)		((_n) ? 0x0048 : 0x0050)

#define REG_TX_IRQ_CFG(_n)		((_n) ? 0x004c : 0x0054)
#define TX_IRQ_THR_MASK			GENMASK(27, 16)
#define TX_IRQ_DEPTH_MASK		GENMASK(11, 0)

#define REG_IRQ_CLEAR_LEN(_n)		((_n) ? 0x0064 : 0x0058)
#define IRQ_CLEAR_LEN_MASK		GENMASK(7, 0)

#define REG_TX_RING_BASE(_n)	\
	(((_n) < 8) ? 0x0100 + ((_n) << 5) : 0x0b00 + (((_n) - 8) << 5))

#define REG_TX_CPU_IDX(_n)	\
	(((_n) < 8) ? 0x0108 + ((_n) << 5) : 0x0b08 + (((_n) - 8) << 5))

#define TX_RING_CPU_IDX_MASK		GENMASK(15, 0)

#define REG_TX_DMA_IDX(_n)	\
	(((_n) < 8) ? 0x010c + ((_n) << 5) : 0x0b0c + (((_n) - 8) << 5))

#define TX_RING_DMA_IDX_MASK		GENMASK(15, 0)

#define IRQ_RING_IDX_MASK		GENMASK(20, 16)
#define IRQ_DESC_IDX_MASK		GENMASK(15, 0)

#define REG_RX_RING_BASE(_n)	\
	(((_n) < 16) ? 0x0200 + ((_n) << 5) : 0x0e00 + (((_n) - 16) << 5))

#define REG_RX_RING_SIZE(_n)	\
	(((_n) < 16) ? 0x0204 + ((_n) << 5) : 0x0e04 + (((_n) - 16) << 5))

#define RX_RING_THR_MASK		GENMASK(31, 16)
#define RX_RING_SIZE_MASK		GENMASK(15, 0)

#define REG_RX_CPU_IDX(_n)	\
	(((_n) < 16) ? 0x0208 + ((_n) << 5) : 0x0e08 + (((_n) - 16) << 5))

#define RX_RING_CPU_IDX_MASK		GENMASK(15, 0)

#define REG_RX_DMA_IDX(_n)	\
	(((_n) < 16) ? 0x020c + ((_n) << 5) : 0x0e0c + (((_n) - 16) << 5))

#define REG_RX_DELAY_INT_IDX(_n)	\
	(((_n) < 16) ? 0x0210 + ((_n) << 5) : 0x0e10 + (((_n) - 16) << 5))

#define RX_DELAY_INT_MASK		GENMASK(15, 0)

#define RX_RING_DMA_IDX_MASK		GENMASK(15, 0)

#define REG_LMGR_INIT_CFG		0x1000
#define LMGR_INIT_START			BIT(31)
#define LMGR_SRAM_MODE_MASK		BIT(30)
#define HW_FWD_PKTSIZE_OVERHEAD_MASK	GENMASK(27, 20)
#define HW_FWD_DESC_NUM_MASK		GENMASK(16, 0)

/* CTRL */
#define QDMA_DESC_DONE_MASK		BIT(31)
#define QDMA_DESC_DROP_MASK		BIT(30) /* tx: drop - rx: overflow */
#define QDMA_DESC_MORE_MASK		BIT(29) /* more SG elements */
#define QDMA_DESC_DEI_MASK		BIT(25)
#define QDMA_DESC_NO_DROP_MASK		BIT(24)
#define QDMA_DESC_LEN_MASK		GENMASK(15, 0)
/* DATA */
#define QDMA_DESC_NEXT_ID_MASK		GENMASK(15, 0)
/* TX MSG0 */
#define QDMA_ETH_TXMSG_MIC_IDX_MASK	BIT(30)
#define QDMA_ETH_TXMSG_SP_TAG_MASK	GENMASK(29, 14)
#define QDMA_ETH_TXMSG_ICO_MASK		BIT(13)
#define QDMA_ETH_TXMSG_UCO_MASK		BIT(12)
#define QDMA_ETH_TXMSG_TCO_MASK		BIT(11)
#define QDMA_ETH_TXMSG_TSO_MASK		BIT(10)
#define QDMA_ETH_TXMSG_FAST_MASK	BIT(9)
#define QDMA_ETH_TXMSG_OAM_MASK		BIT(8)
#define QDMA_ETH_TXMSG_CHAN_MASK	GENMASK(7, 3)
#define QDMA_ETH_TXMSG_QUEUE_MASK	GENMASK(2, 0)
/* TX MSG1 */
#define QDMA_ETH_TXMSG_NO_DROP		BIT(31)
#define QDMA_ETH_TXMSG_METER_MASK	GENMASK(30, 24)	/* 0x7f no meters */
#define QDMA_ETH_TXMSG_FPORT_MASK	GENMASK(23, 20)
#define QDMA_ETH_TXMSG_NBOQ_MASK	GENMASK(19, 15)
#define QDMA_ETH_TXMSG_HWF_MASK		BIT(14)
#define QDMA_ETH_TXMSG_HOP_MASK		BIT(13)
#define QDMA_ETH_TXMSG_PTP_MASK		BIT(12)
#define QDMA_ETH_TXMSG_ACNT_G1_MASK	GENMASK(10, 6)	/* 0x1f do not count */
#define QDMA_ETH_TXMSG_ACNT_G0_MASK	GENMASK(5, 0)	/* 0x3f do not count */

/* RX MSG1 */
#define QDMA_ETH_RXMSG_DEI_MASK		BIT(31)
#define QDMA_ETH_RXMSG_IP6_MASK		BIT(30)
#define QDMA_ETH_RXMSG_IP4_MASK		BIT(29)
#define QDMA_ETH_RXMSG_IP4F_MASK	BIT(28)
#define QDMA_ETH_RXMSG_L4_VALID_MASK	BIT(27)
#define QDMA_ETH_RXMSG_L4F_MASK		BIT(26)
#define QDMA_ETH_RXMSG_SPORT_MASK	GENMASK(25, 21)
#define QDMA_ETH_RXMSG_CRSN_MASK	GENMASK(20, 16)
#define QDMA_ETH_RXMSG_PPE_ENTRY_MASK	GENMASK(15, 0)

enum {
	FE_PSE_PORT_CDM1,
	FE_PSE_PORT_GDM1,
	FE_PSE_PORT_GDM2,
	FE_PSE_PORT_GDM3,
	FE_PSE_PORT_PPE1,
	FE_PSE_PORT_CDM2,
	FE_PSE_PORT_CDM3,
	FE_PSE_PORT_CDM4,
	FE_PSE_PORT_PPE2,
	FE_PSE_PORT_GDM4,
	FE_PSE_PORT_CDM5,
	FE_PSE_PORT_DROP = 0xf,
};

struct airoha_qdma_desc {
	__le32 rsv;
	__le32 ctrl;
	__le32 addr;
	__le32 data;
	__le32 msg0;
	__le32 msg1;
	__le32 msg2;
	__le32 msg3;
};

struct airoha_qdma_fwd_desc {
	__le32 addr;
	__le32 ctrl0;
	__le32 ctrl1;
	__le32 ctrl2;
	__le32 msg0;
	__le32 msg1;
	__le32 rsv0;
	__le32 rsv1;
};

struct en751221_qdma_fwd_desc {
	u32 addr;
	u32 info;
	u32 msg0;
	u32 msg1;
};

struct airoha_queue {
	struct airoha_qdma_desc *desc;
	u16 head;
	u16 tail;
	uchar *rx_spare;

	int ndesc;
	int rx_buf_len;
	int rx_pkt_offset;
};

struct airoha_tx_irq_queue {
	struct airoha_qdma *qdma;

	int size;
	u32 *q;
};

struct airoha_qdma {
	struct airoha_eth *eth;
	void __iomem *regs;

	void *tx_bounce;
	unsigned long tx_bounce_dma;
	size_t tx_bounce_size;

	struct airoha_tx_irq_queue q_tx_irq[AIROHA_NUM_TX_IRQ];

	struct airoha_queue q_tx[AIROHA_NUM_TX_RING];
	struct airoha_queue q_rx[AIROHA_NUM_RX_RING];

	/* descriptor and packet buffers for qdma hw forward */
	struct {
		void *desc;
		void *q;
	} hfwd;
};

struct airoha_gdm_port {
	struct airoha_qdma *qdma;
	int id;

	struct udevice *pcs_dev;
	phy_interface_t mode;
	bool neg_mode;

	struct phy_device *phydev;
};

struct airoha_eth {
	void __iomem *fe_regs;
	void __iomem *switch_regs;
	struct udevice *switch_mdio_dev;

	struct reset_ctl_bulk rsts;
	struct reset_ctl_bulk xsi_rsts;

	struct airoha_eth_soc_data *soc;

	struct airoha_qdma qdma[AIROHA_MAX_NUM_QDMA];
	char gdm_port_str[AIROHA_MAX_NUM_GDM_PORTS + 1][AIROHA_GDM_PORT_STRING_LEN];
};

struct airoha_eth_soc_data {
	u32 version;
	bool econet;
	bool gen1;
	bool legacy_qdma;
	bool direct_reset;
	bool dscp_byte_swap;
	bool switch_mdio;
	int num_xsi_rsts;
	const char * const *xsi_rsts_names;
	const char *switch_compatible;
};

static const char * const en7523_xsi_rsts_names[] = {
	"hsi0-mac",
	"hsi1-mac",
	"hsi-mac",
};

static const char * const en7581_xsi_rsts_names[] = {
	"hsi0-mac",
	"hsi1-mac",
	"hsi-mac",
	"xfp-mac",
};

static const char * const an7583_xsi_rsts_names[] = {
	"hsi0-mac",
	"hsi1-mac",
	"xfp-mac",
};

static u32 airoha_rr(void __iomem *base, u32 offset)
{
	return readl(base + offset);
}

static void airoha_wr(void __iomem *base, u32 offset, u32 val)
{
	writel(val, base + offset);
}

static u32 airoha_rmw(void __iomem *base, u32 offset, u32 mask, u32 val)
{
	val |= (airoha_rr(base, offset) & ~mask);
	airoha_wr(base, offset, val);

	return val;
}

#define airoha_fe_rr(eth, offset)				\
	airoha_rr((eth)->fe_regs, (offset))
#define airoha_fe_wr(eth, offset, val)				\
	airoha_wr((eth)->fe_regs, (offset), (val))
#define airoha_fe_rmw(eth, offset, mask, val)			\
	airoha_rmw((eth)->fe_regs, (offset), (mask), (val))
#define airoha_fe_set(eth, offset, val)				\
	airoha_rmw((eth)->fe_regs, (offset), 0, (val))
#define airoha_fe_clear(eth, offset, val)			\
	airoha_rmw((eth)->fe_regs, (offset), (val), 0)

#define airoha_qdma_rr(qdma, offset)				\
	airoha_rr((qdma)->regs, (offset))
#define airoha_qdma_wr(qdma, offset, val)			\
	airoha_wr((qdma)->regs, (offset), (val))
#define airoha_qdma_rmw(qdma, offset, mask, val)		\
	airoha_rmw((qdma)->regs, (offset), (mask), (val))
#define airoha_qdma_set(qdma, offset, val)			\
	airoha_rmw((qdma)->regs, (offset), 0, (val))
#define airoha_qdma_clear(qdma, offset, val)			\
	airoha_rmw((qdma)->regs, (offset), (val), 0)

#define airoha_switch_rr(eth, offset)				\
	airoha_rr((eth)->switch_regs, (offset))
#define airoha_switch_wr(eth, offset, val)			\
	airoha_wr((eth)->switch_regs, (offset), (val))
#define airoha_switch_rmw(eth, offset, mask, val)		\
	airoha_rmw((eth)->switch_regs, (offset), (mask), (val))

#if IS_ENABLED(CONFIG_AIROHA_ETH_DEBUG)
#define eth_trace(fmt, args...)		printf("[airoha] " fmt, ##args)
#define eth_traced			1
#else
#define eth_trace(fmt, args...)		do { } while (0)
#define eth_traced			0
#endif

static bool airoha_dma_is_uncached(const void *vaddr)
{
#if IS_ENABLED(CONFIG_ARCH_ECONET)
	return KSEGX((uintptr_t)vaddr) == KSEG1;
#else
	return false;
#endif
}

static inline dma_addr_t dma_map_unaligned(void *vaddr, size_t len,
					   enum dma_data_direction dir)
{
	uintptr_t start, end;

	/* KSEG1 accesses bypass the MIPS caches and need no cache operation. */
	if (airoha_dma_is_uncached(vaddr)) {
		/* Order descriptor stores before publishing them to DMA. */
		wmb();
		return virt_to_phys(vaddr);
	}

	start = ALIGN_DOWN((uintptr_t)vaddr, ARCH_DMA_MINALIGN);
	end = ALIGN((uintptr_t)(vaddr + len), ARCH_DMA_MINALIGN);

	return dma_map_single((void *)start, end - start, dir);
}

static inline void dma_unmap_unaligned(dma_addr_t addr, size_t len,
				       enum dma_data_direction dir)
{
	uintptr_t start, end;

#if IS_ENABLED(CONFIG_ARCH_ECONET)
	if (KSEGX((uintptr_t)addr) == KSEG1) {
		/* Order DMA writes before the CPU consumes an uncached descriptor. */
		rmb();
		return;
	}
#endif

	start = ALIGN_DOWN((uintptr_t)addr, ARCH_DMA_MINALIGN);
	end = ALIGN((uintptr_t)(addr + len), ARCH_DMA_MINALIGN);
	dma_unmap_single(start, end - start, dir);
}

static void *airoha_dma_alloc_aligned(size_t size, size_t align,
				      unsigned long *dma_addr)
{
#if IS_ENABLED(CONFIG_ARCH_ECONET)
	void *buf;

	align = max_t(size_t, align, ARCH_DMA_MINALIGN);
	size = ALIGN(size, align);
	buf = memalign(align, size);
	if (!buf)
		return NULL;

	*dma_addr = virt_to_phys(buf);
	return buf;
#else
	return dma_alloc_coherent(size, dma_addr);
#endif
}

static void *airoha_dma_alloc_coherent(size_t size, unsigned long *dma_addr)
{
	return airoha_dma_alloc_aligned(size, ARCH_DMA_MINALIGN, dma_addr);
}

static void *airoha_dma_alloc_uncached(size_t size, unsigned long *dma_addr)
{
#if IS_ENABLED(CONFIG_ARCH_ECONET)
	void *buf;
	uintptr_t start, end;

	buf = airoha_dma_alloc_aligned(size, ARCH_DMA_MINALIGN, dma_addr);
	if (!buf)
		return NULL;

	/*
	 * Drop any cache lines created for the allocator's KSEG0 alias before
	 * permanently accessing this DMA object through KSEG1.  Generation-1
	 * EN75xx QDMA is not coherent with the CPU caches, and a recovery/
	 * chainload path may inherit an active secondary cache which U-Boot does
	 * not otherwise maintain.
	 */
	start = ALIGN_DOWN((uintptr_t)buf, ARCH_DMA_MINALIGN);
	end = ALIGN((uintptr_t)buf + size, ARCH_DMA_MINALIGN);
	flush_dcache_range(start, end);

	return (void *)CKSEG1ADDR(*dma_addr);
#else
	return airoha_dma_alloc_coherent(size, dma_addr);
#endif
}

static void *en751221_dma_alloc_control(struct airoha_qdma *qdma, size_t size,
					unsigned long *dma_addr)
{
	/*
	 * Keep all legacy-QDMA control memory uncached.  This was originally
	 * required only by EN7528 chainload testing, but EN751221 BootROM
	 * recovery leaves the same kind of cache state behind: QDMA can consume
	 * a TX descriptor and advance TX_DMA_IDX while the CPU keeps seeing a
	 * stale cached copy without the DONE writeback.
	 *
	 * Descriptor and IRQ-queue accesses are word-sized, so this deliberately
	 * does not expose packet buffers to KSEG1.  EN751221 has unsafe uncached
	 * byte stores, therefore payload buffers must keep using the normal DMA
	 * cache-maintenance path.
	 */
	return airoha_dma_alloc_uncached(size, dma_addr);
}

static bool airoha_is_gen1(struct airoha_eth *eth)
{
	return eth->soc->gen1;
}

static bool airoha_uses_legacy_qdma(struct airoha_eth *eth)
{
	return eth->soc->legacy_qdma;
}

static u32 en751221_qdma_lmgr_free(struct airoha_qdma *qdma);

/*
 * One-line snapshot of the whole generation-1 datapath state.  Every trace
 * point carries the LMGR counters so a descriptor leak can be attributed to
 * the exact step that caused it.
 */
static void en751221_qdma_trace(struct airoha_qdma *qdma, const char *tag)
{
	if (!eth_traced)
		return;

	printf("[airoha] %-16s cfg=%08x txcpu=%03x txdma=%03x rxcpu=%03x rxdma=%03x int=%08x irq=%08x free=%04x used=%08x\n",
	       tag,
	       airoha_qdma_rr(qdma, REG_QDMA_GLOBAL_CFG),
	       (u32)(airoha_qdma_rr(qdma, EN751221_REG_TX_CPU_IDX) &
			     EN751221_RING_IDX_MASK),
	       (u32)(airoha_qdma_rr(qdma, EN751221_REG_TX_DMA_IDX) &
			     EN751221_RING_IDX_MASK),
	       (u32)(airoha_qdma_rr(qdma, EN751221_REG_RX_CPU_IDX) &
			     EN751221_RING_IDX_MASK),
	       (u32)(airoha_qdma_rr(qdma, EN751221_REG_RX_DMA_IDX) &
			     EN751221_RING_IDX_MASK),
	       airoha_qdma_rr(qdma, EN751221_REG_INT_STATUS),
	       airoha_qdma_rr(qdma, EN751221_REG_IRQ_STATUS),
	       en751221_qdma_lmgr_free(qdma),
	       airoha_qdma_rr(qdma, EN751221_REG_DBG_HWF_BUF_USAGE));
}

static void en7528_qdma_trace(struct airoha_qdma *qdma, const char *tag)
{
	if (!eth_traced || qdma->eth->soc->version != 0x7528)
		return;
}

static void en751221_fe_trace(struct airoha_eth *eth, int gdm,
			      const char *tag)
{
	if (!eth_traced)
		return;

	printf("[airoha] %-16s gdm%d=%08x cport=%08x csg=%08x hwf1=%08x hwf2=%08x txchn=%08x rxchn=%08x txvld=%08x rxvld=%08x\n",
	       tag, gdm,
	       airoha_fe_rr(eth, REG_GDM_FWD_CFG(gdm)),
	       airoha_fe_rr(eth, REG_FE_CPORT_CFG),
	       airoha_fe_rr(eth, REG_CDM_CSG_CFG),
	       airoha_fe_rr(eth, REG_CDM1_HWF_CHN_EN),
	       airoha_fe_rr(eth, REG_CDM2_HWF_CHN_EN),
	       airoha_fe_rr(eth, REG_GDM_TXCHN_EN(gdm)),
	       airoha_fe_rr(eth, REG_GDM_RXCHN_EN(gdm)),
	       airoha_fe_rr(eth, REG_GDM_TX_CHN_VLD(gdm)),
	       airoha_fe_rr(eth, REG_GDM_RX_CHN_VLD(gdm)));

	if (eth->soc->version == 0x7528)
		printf("[airoha] %-16s cdma1: cpu_ok=%u hwf_ok=%u cpu_drop=%u hwf_drop=%u\n",
		       "",
		       airoha_fe_rr(eth, REG_CDMA1_RXCPU_OK_CNT),
		       airoha_fe_rr(eth, REG_CDMA1_RXHWF_OK_CNT),
		       airoha_fe_rr(eth, REG_CDMA1_RXCPU_DROP_CNT),
		       airoha_fe_rr(eth, REG_CDMA1_RXHWF_DROP_CNT));
}

static void en751221_desc_trace(const char *tag, int index,
				struct airoha_qdma_desc *desc)
{
	if (!eth_traced)
		return;

	printf("[airoha] %-16s [%02d]@%08lx rsv=%08x ctrl=%08x addr=%08x next=%08x msg0=%08x msg1=%08x\n",
	       tag, index, (ulong)virt_to_phys(desc),
	       READ_ONCE(desc->rsv), READ_ONCE(desc->ctrl),
	       READ_ONCE(desc->addr), READ_ONCE(desc->data),
	       READ_ONCE(desc->msg0), READ_ONCE(desc->msg1));
}

static void en751221_packet_trace(const char *dir, const void *packet,
				  size_t length)
{
	if (!eth_traced)
		return;

	printf("[airoha] %s packet: addr=%08lx len=%zu\n", dir,
	       (ulong)virt_to_phys((void *)packet), length);
	print_hex_dump_bytes("[airoha]   ", DUMP_PREFIX_OFFSET, packet, length);
}

static void en7528_tx_path_trace(struct airoha_eth *eth, const char *tag)
{
	eth_trace("%-20s gdm:get=%u ok=%u drop=%u bytes=%u\n", tag,
		  airoha_fe_rr(eth, REG_GDM1_TX_GET_CNT),
		  airoha_fe_rr(eth, REG_GDM1_TX_OK_CNT),
		  airoha_fe_rr(eth, REG_GDM1_TX_DROP_CNT),
		  airoha_fe_rr(eth, REG_GDM1_TX_OK_BYTE_CNT));
	eth_trace("                 p1:u=%u b=%u d=%u s=%08x p2:u=%u b=%u d=%u s=%08x\n",
		  airoha_switch_rr(eth, SWITCH_TX_UNIC(1)),
		  airoha_switch_rr(eth, SWITCH_TX_BROC(1)),
		  airoha_switch_rr(eth, SWITCH_TX_DROC(1)),
		  airoha_switch_rr(eth, SWITCH_PMSR(1)),
		  airoha_switch_rr(eth, SWITCH_TX_UNIC(2)),
		  airoha_switch_rr(eth, SWITCH_TX_BROC(2)),
		  airoha_switch_rr(eth, SWITCH_TX_DROC(2)),
		  airoha_switch_rr(eth, SWITCH_PMSR(2)));
	eth_trace("                 p3:u=%u b=%u d=%u s=%08x p4:u=%u b=%u d=%u s=%08x\n",
		  airoha_switch_rr(eth, SWITCH_TX_UNIC(3)),
		  airoha_switch_rr(eth, SWITCH_TX_BROC(3)),
		  airoha_switch_rr(eth, SWITCH_TX_DROC(3)),
		  airoha_switch_rr(eth, SWITCH_PMSR(3)),
		  airoha_switch_rr(eth, SWITCH_TX_UNIC(4)),
		  airoha_switch_rr(eth, SWITCH_TX_BROC(4)),
		  airoha_switch_rr(eth, SWITCH_TX_DROC(4)),
		  airoha_switch_rr(eth, SWITCH_PMSR(4)));
	eth_trace("                 p6:rxu=%u rxb=%u txu=%u txb=%u\n",
		  airoha_switch_rr(eth, SWITCH_RX_UNIC(6)),
		  airoha_switch_rr(eth, SWITCH_RX_BROC(6)),
		  airoha_switch_rr(eth, SWITCH_TX_UNIC(6)),
		  airoha_switch_rr(eth, SWITCH_TX_BROC(6)));
	eth_trace("                 cdma1: cpu=%u hwf=%u cpu_drop=%u hwf_drop=%u hwfen=%08x\n",
		  airoha_fe_rr(eth, REG_CDMA1_RXCPU_OK_CNT),
		  airoha_fe_rr(eth, REG_CDMA1_RXHWF_OK_CNT),
		  airoha_fe_rr(eth, REG_CDMA1_RXCPU_DROP_CNT),
		  airoha_fe_rr(eth, REG_CDMA1_RXHWF_DROP_CNT),
		  airoha_fe_rr(eth, REG_CDM1_HWF_CHN_EN));
}

static int en7528_switch_atc_wait(struct airoha_eth *eth)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(airoha_switch_rr(eth, SWITCH_ATC) & SWITCH_ATC_BUSY))
			return 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

static void en7528_switch_trace_fdb(struct airoha_eth *eth, const u8 *target)
{
	u32 atc, atrd, tsra1, tsra2;
	u8 mac[ETH_ALEN];
	int i;

	if (!eth_traced || en7528_switch_atc_wait(eth))
		return;

	/* Start search from the first dynamic/static address-table entry. */
	airoha_switch_wr(eth, SWITCH_ATC, SWITCH_ATC_BUSY | 4);
	if (en7528_switch_atc_wait(eth))
		return;

	for (i = 0; i < 2048; i++) {
		atc = airoha_switch_rr(eth, SWITCH_ATC);
		if (atc & SWITCH_ATC_SEARCH_HIT) {
			tsra1 = airoha_switch_rr(eth, SWITCH_TSRA1);
			tsra2 = airoha_switch_rr(eth, SWITCH_TSRA2);
			atrd = airoha_switch_rr(eth, SWITCH_ATRD);

			mac[0] = tsra1 >> 24;
			mac[1] = tsra1 >> 16;
			mac[2] = tsra1 >> 8;
			mac[3] = tsra1;
			mac[4] = tsra2 >> 24;
			mac[5] = tsra2 >> 16;

			if (!memcmp(mac, target, ETH_ALEN)) {
				eth_trace("switch: fdb %pM vid=%u ports=%02x atrd=%08x\n",
					  mac, tsra2 & 0xfff,
					  FIELD_GET(SWITCH_ATRD_PORT_MASK, atrd),
					  atrd);
				return;
			}
		}

		if ((atc & SWITCH_ATC_SEARCH_END) &&
		    FIELD_GET(SWITCH_ATC_ADDR_MASK, atc) == 0x7ff)
			break;

		airoha_switch_wr(eth, SWITCH_ATC, SWITCH_ATC_BUSY | 5);
		if (en7528_switch_atc_wait(eth))
			break;
	}

	eth_trace("switch: fdb %pM not found\n", target);
}

static void en7528_qdma_reap_tx_irq(struct airoha_qdma *qdma)
{
	u32 status, entries;

	status = airoha_qdma_rr(qdma, EN751221_REG_IRQ_STATUS);
	entries = FIELD_GET(EN751221_IRQ_ENTRY_LEN_MASK, status);
	if (!entries)
		return;

	/*
	 * The vendor bootloader treats the TX IRQ queue as an asynchronous
	 * descriptor-recycle queue.  TX submission itself does not wait for an
	 * IRQ entry; it drains completed entries opportunistically before later
	 * submissions.  Do the same here so a delayed IRQ entry cannot make the
	 * software reuse the previous null descriptor after TX_DMA_IDX advanced.
	 */
	while (entries) {
		u32 clear = min_t(u32, entries, 0x7f);

		airoha_qdma_rmw(qdma, EN751221_REG_IRQ_CLEAR_LEN,
				EN751221_IRQ_CLEAR_LEN_MASK, clear);
		entries -= clear;
	}

	eth_trace("tx: reaped irq_status=%08x\n", status);
}

static int airoha_get_fe_port(struct airoha_gdm_port *port)
{
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_eth *eth = qdma->eth;

	switch (eth->soc->version) {
	case 0x7512:
	case 0x7528:
		return EN751221_FPORT_GDM1;
	case 0x7523:
		/* FIXME: GDM1 is the only supported port */
		return FE_PSE_PORT_GDM1;
	case 0x7581:
	default:
		return port->id == 4 ? FE_PSE_PORT_GDM4 : port->id;
	}
}

static void airoha_fe_maccr_init(struct airoha_gdm_port *port)
{
	if (airoha_is_gen1(port->qdma->eth)) {
		u32 len_thr;

		if (port->qdma->eth->soc->version == 0x7528) {
			/*
			 * U-Boot has no PPE/hw_nat consumer for hardware-forwarded RX
			 * buffers.  Route every EN7528 channel to the CPU descriptor
			 * path, matching the existing EN751221 U-Boot policy.
			 */
			airoha_fe_wr(port->qdma->eth, REG_CDM_CSG_CFG,
				     EN7528_TCBOOT_CDM_CSG_CFG);
			airoha_fe_wr(port->qdma->eth, REG_CDM1_HWF_CHN_EN, 0);
			airoha_fe_wr(port->qdma->eth, REG_CDM2_HWF_CHN_EN, 0);
			airoha_fe_wr(port->qdma->eth, REG_GDM_FWD_CFG(port->id),
				     EN7528_TCBOOT_GDM_FWD_CFG);
			airoha_fe_wr(port->qdma->eth, REG_GDM_VLAN_GEN(port->id),
				     EN7528_TCBOOT_GDM_VLAN_GEN);
			airoha_fe_wr(port->qdma->eth, REG_GDM_RX_LEN_THR(port->id),
				     EN7528_TCBOOT_GDM_RX_LEN_THR);
			airoha_fe_wr(port->qdma->eth, REG_GDM_TXCHN_EN(port->id),
				     0xffffffff);
			airoha_fe_wr(port->qdma->eth, REG_GDM_RXCHN_EN(port->id),
				     0x0000ffff);
			airoha_fe_wr(port->qdma->eth, REG_FE_CPORT_CFG,
				     EN7528_TCBOOT_CPORT_CFG);
			/*
			 * EN751627/EN7528 runtime setup selects CPORT mode 2
			 * and enables short-frame padding after the boot snapshot.
			 */
			airoha_fe_rmw(port->qdma->eth, REG_FE_CPORT_CFG,
				      FE_CPORT_MODE_MASK | FE_CPORT_PAD_EN,
				      FE_CPORT_MODE_EN751627 | FE_CPORT_PAD_EN);
			airoha_fe_wr(port->qdma->eth, REG_FE_CPORT_CHN_MAP,
				     EN7528_TCBOOT_CPORT_CHN_MAP);
			return;
		}

		/*
		 * Match EN751221 TCBoot macSetMACCR() exactly.  After a real FE
		 * reset GDMA1_FWD_CFG no longer contains the state inherited from
		 * TCBoot, so leaving its upper nibble at reset value breaks the
		 * cold-boot path.  The vendor programs a jumbo-length selector of
		 * 12 and routes all four forwarding classes to CPU/QDMA0, yielding
		 * 0xc0000000.
		 */
		airoha_fe_wr(port->qdma->eth, REG_GDM_FWD_CFG(port->id),
			     FIELD_PREP(EN751221_GDM_JMB_LEN, 12));

		/*
		 * The EcoNet SDK enables short-frame padding through FE_CPORT_CFG,
		 * not through the newer GDM_PAD_EN bit.  This is mandatory for ARP
		 * and other sub-60-byte Ethernet frames generated by U-Boot.
		 *
		 * Keep the U-Boot CPU path deliberately untagged: unlike the vendor
		 * Linux driver we do not insert/strip the MT7530 special tag.
		 */
		airoha_fe_rmw(port->qdma->eth, REG_FE_CPORT_CFG,
			     FE_CPORT_PAD_EN, FE_CPORT_PAD_EN);
		airoha_fe_rmw(port->qdma->eth, REG_CDM_CSG_CFG, CDM_STAG_EN, 0);

		/*
		 * CDMx_HWF_CHN_EN comes out of reset as 0xffffffff, i.e. every
		 * channel is hardware-forwarded.  In the vendor firmware the
		 * PPE/hw_nat driver owns those buffers and returns them to
		 * LMGR; U-Boot has no such consumer, so each received frame
		 * permanently leaks one hardware-forwarding descriptor until
		 * the pool is empty and CPU TX can no longer be started.
		 * Force every channel onto the CPU descriptor path instead.
		 */
		airoha_fe_wr(port->qdma->eth, REG_CDM1_HWF_CHN_EN, 0);
		airoha_fe_wr(port->qdma->eth, REG_CDM2_HWF_CHN_EN, 0);
		airoha_fe_rmw(port->qdma->eth, REG_GDM_FWD_CFG(port->id),
			     EN751221_GDM_STAG_EN, 0);
		airoha_fe_wr(port->qdma->eth, REG_GDM_VLAN_GEN(port->id), 0);

		len_thr = FIELD_PREP(EN751221_GDM_OVERSIZE_LEN,
				     PKTSIZE_ALIGN) |
			  FIELD_PREP(EN751221_GDM_RUNT_LEN, 60);
		airoha_fe_wr(port->qdma->eth, REG_GDM_RX_LEN_THR(port->id),
			     len_thr);
		return;
	}

	/*
	 * Disable any kind of CRC drop or offload.
	 * Enable padding of short TX packets to 60 bytes.
	 */
	airoha_fe_wr(port->qdma->eth, REG_GDM_FWD_CFG(port->id), GDM_PAD_EN);
}

static int airoha_fe_init(struct airoha_gdm_port *port)
{
	if (airoha_is_gen1(port->qdma->eth))
		en751221_fe_trace(port->qdma->eth, port->id, "fe: before");

	airoha_fe_maccr_init(port);

	if (airoha_is_gen1(port->qdma->eth))
		en751221_fe_trace(port->qdma->eth, port->id, "fe: after");

	return 0;
}

static void en751221_qdma_reset_rx_desc(struct airoha_queue *q, int index)
{
	struct airoha_qdma_desc *desc = &q->desc[index];
	uchar *rx_packet = net_rx_packets[index];
	int dma_len = q->rx_buf_len + q->rx_pkt_offset;
	u32 next;

	dma_map_single(rx_packet, dma_len, DMA_FROM_DEVICE);
	next = (index + 1) % q->ndesc;

	WRITE_ONCE(desc->rsv, 0);
	WRITE_ONCE(desc->ctrl,
		   FIELD_PREP(QDMA_DESC_LEN_MASK, q->rx_buf_len));
	WRITE_ONCE(desc->addr, virt_to_phys(rx_packet));
	WRITE_ONCE(desc->data, next);
	WRITE_ONCE(desc->msg0, 0);
	WRITE_ONCE(desc->msg1, 0);
	WRITE_ONCE(desc->msg2, 0);
	WRITE_ONCE(desc->msg3, 0);

	dma_map_unaligned(desc, sizeof(*desc), DMA_TO_DEVICE);
}

static void en751221_qdma_rearm_rx_desc(struct airoha_qdma *qdma)
{
	struct airoha_queue *q = &qdma->q_rx[0];
	int i;

	for (i = 0; i < q->ndesc; i++)
		en751221_qdma_reset_rx_desc(q, i);
}

/*
 * EN751627/EN7528 RX uses a moving NULL descriptor.  RX_CPU_IDX points at
 * the NULL tail and RX_DMA_IDX points at the first descriptor owned by DMA.
 * When a packet is returned, its buffer is attached to the old NULL tail and
 * the descriptor that carried the packet becomes the new NULL descriptor.
 *
 * Re-arming the descriptor that was just consumed, as EN751221 can tolerate,
 * breaks this invariant on EN7528 and lets DMA concatenate/overwrite packets
 * while walking a stale chain.
 */
static void en7528_qdma_arm_rx_desc(struct airoha_queue *q, int index,
				    uchar *rx_packet, int next)
{
	struct airoha_qdma_desc *desc = &q->desc[index];
	int dma_len = q->rx_buf_len + q->rx_pkt_offset;

	/*
	 * EN7528 is chainloaded from TCBoot with the MIPS cache hierarchy
	 * already active.  U-Boot does not maintain the vendor-enabled
	 * secondary cache, so a cached net_rx_packets[] alias can expose
	 * payload data from the previous trip around the RX ring even after
	 * QDMA has updated the uncached descriptor.  KSEG1 avoids both cache
	 * levels and keeps DMA/CPU ownership unambiguous in the bootloader.
	 */
	if (!airoha_dma_is_uncached(rx_packet))
		dma_map_single(rx_packet, dma_len, DMA_FROM_DEVICE);

	WRITE_ONCE(desc->rsv, 0);
	WRITE_ONCE(desc->ctrl,
		   FIELD_PREP(QDMA_DESC_LEN_MASK, q->rx_buf_len));
	WRITE_ONCE(desc->addr, virt_to_phys(rx_packet));
	WRITE_ONCE(desc->data, next);
	WRITE_ONCE(desc->msg0, 0);
	WRITE_ONCE(desc->msg1, 0);
	WRITE_ONCE(desc->msg2, 0);
	WRITE_ONCE(desc->msg3, 0);

	dma_map_unaligned(desc, sizeof(*desc), DMA_TO_DEVICE);
}

static void en7528_qdma_init_rx_desc(struct airoha_qdma *qdma)
{
	struct airoha_queue *q = &qdma->q_rx[0];
	int first = q->ndesc - 1;
	int null = q->ndesc - 2;
	int i;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));

	/*
	 * Match the EN751627 vendor QDMA buffer-manager ordering exactly:
	 * start DMA on descriptor N-1, chain through 0..N-3 and leave N-2
	 * as the NULL descriptor named by RX_CPU_IDX.  One descriptor is
	 * therefore always reserved as the moving tail.
	 */
	en7528_qdma_arm_rx_desc(q, first,
				EN7528_RX_UNCACHED(net_rx_packets[0]), 0);
	for (i = 0; i < null; i++)
		en7528_qdma_arm_rx_desc(q, i,
					EN7528_RX_UNCACHED(net_rx_packets[i + 1]), i + 1);

	q->head = null;
	q->tail = null;
	/*
	 * Keep one buffer outside the DMA chain.  The vendor EN751627 buffer
	 * manager allocates a fresh skb when recycling a descriptor; rotating
	 * a spare U-Boot RX buffer provides the same ownership separation
	 * without dynamic allocation.
	 */
	q->rx_spare = EN7528_RX_UNCACHED(net_rx_packets[q->ndesc - 1]);

	airoha_qdma_wr(qdma, EN751221_REG_RX_CPU_IDX, null);
	airoha_qdma_wr(qdma, EN751221_REG_RX_DMA_IDX, first);
	eth_trace("rx null: first=%d null=%d cpu=%03x dma=%03x\n",
		  first, null,
		  airoha_qdma_rr(qdma, EN751221_REG_RX_CPU_IDX) &
		  EN751221_RING_IDX_MASK,
		  airoha_qdma_rr(qdma, EN751221_REG_RX_DMA_IDX) &
		  EN751221_RING_IDX_MASK);
}

static void en7528_qdma_recycle_rx_desc(struct airoha_qdma *qdma,
					struct airoha_queue *q, int index,
					uchar *rx_packet)
{
	uchar *spare = q->rx_spare;

	/*
	 * Match the vendor buffer-manager ownership model: the old NULL
	 * descriptor gets a different buffer from the one that has just been
	 * consumed.  Once the stack releases rx_packet, it becomes the spare
	 * for the next recycle.
	 */
	en7528_qdma_arm_rx_desc(q, q->tail, spare, index);
	q->rx_spare = rx_packet;
	q->tail = index;
	q->head = index;
	airoha_qdma_wr(qdma, EN751221_REG_RX_CPU_IDX, index);
}

static void en751221_qdma_init_rx_desc(struct airoha_qdma *qdma)
{
	struct airoha_queue *q = &qdma->q_rx[0];

	en751221_qdma_rearm_rx_desc(qdma);

	/* On EN751221, equal CPU/HW indices mean that the RX ring is full. */
	q->head = 0;
	airoha_qdma_wr(qdma, EN751221_REG_RX_CPU_IDX, 0);
	airoha_qdma_wr(qdma, EN751221_REG_RX_DMA_IDX, 1);
}

static int en751221_qdma_init_rx(struct airoha_qdma *qdma)
{
	struct airoha_queue *q = &qdma->q_rx[0];
	unsigned long dma_addr;

	if (qdma->eth->soc->version == 0x7528) {
		q->ndesc = EN7528_RX_DSCP_NUM;
		q->rx_buf_len = EN7528_RX_BUF_LEN;
		q->rx_pkt_offset = EN7528_RX_PKT_OFFSET;
	} else {
		q->ndesc = RX_DSCP_NUM;
		q->rx_buf_len = PKTSIZE_ALIGN;
		q->rx_pkt_offset = 0;
	}

	if (q->ndesc > PKTBUFSRX) {
		printf("QDMA: RX ring needs %d packet buffers, only %d available\n",
		       q->ndesc, PKTBUFSRX);
		return -EINVAL;
	}

	q->desc = en751221_dma_alloc_control(qdma,
					     q->ndesc * sizeof(*q->desc),
					     &dma_addr);
	if (!q->desc)
		return -ENOMEM;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));
	airoha_qdma_wr(qdma, EN751221_REG_RX_RING_BASE, dma_addr);
	/*
	 * RX_RING_CFG/THR hold ring0 in bits 11:0 and ring1 in bits 27:16.
	 * The vendor driver programs the low watermark as ndesc / 8 (clamped
	 * to at least one descriptor), not zero.
	 */
	airoha_qdma_wr(qdma, EN751221_REG_RX_RING_SIZE,
		       FIELD_PREP(EN751221_RING_SIZE_MASK, q->ndesc));
	if (qdma->eth->soc->version == 0x7528)
		airoha_qdma_wr(qdma, EN751221_REG_RX_RING_THR, 0);
	else
		airoha_qdma_wr(qdma, EN751221_REG_RX_RING_THR,
			       FIELD_PREP(EN751221_RING_THR_MASK,
					  max_t(u32, q->ndesc >> 3, 1)));

	eth_trace("rx ring: cpu=%08lx dma=%08lx ndesc=%d cfg=%08x thr=%08x\n",
		  (ulong)q->desc, dma_addr, q->ndesc,
		  airoha_qdma_rr(qdma, EN751221_REG_RX_RING_SIZE),
		  airoha_qdma_rr(qdma, EN751221_REG_RX_RING_THR));

	if (qdma->eth->soc->version == 0x7528)
		en7528_qdma_init_rx_desc(qdma);
	else
		en751221_qdma_init_rx_desc(qdma);
	en751221_desc_trace("rx desc0", 0, &q->desc[0]);
	return 0;
}

static void en751221_qdma_reset_tx_desc(struct airoha_queue *q, int index)
{
	struct airoha_qdma_desc *desc = &q->desc[index];

	WRITE_ONCE(desc->rsv, 0);
	WRITE_ONCE(desc->ctrl, QDMA_DESC_DONE_MASK);
	WRITE_ONCE(desc->addr, 0);
	WRITE_ONCE(desc->data, 0);
	WRITE_ONCE(desc->msg0, 0);
	WRITE_ONCE(desc->msg1, 0);
	WRITE_ONCE(desc->msg2, 0);
	WRITE_ONCE(desc->msg3, 0);
	dma_map_unaligned(desc, sizeof(*desc), DMA_TO_DEVICE);
}

static int en751221_qdma_init_tx(struct airoha_qdma *qdma)
{
	struct airoha_queue *q = &qdma->q_tx[0];
	unsigned long dma_addr;
	int i;

	q->ndesc = TX_DSCP_NUM;
	q->head = 0;
	q->desc = en751221_dma_alloc_control(qdma,
					     q->ndesc * sizeof(*q->desc),
					     &dma_addr);
	if (!q->desc)
		return -ENOMEM;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));
	for (i = 0; i < q->ndesc; i++)
		en751221_qdma_reset_tx_desc(q, i);

	airoha_qdma_wr(qdma, EN751221_REG_TX_RING_BASE, dma_addr);
	airoha_qdma_wr(qdma, EN751221_REG_TX_CPU_IDX, 0);
	airoha_qdma_wr(qdma, EN751221_REG_TX_DMA_IDX, 0);

	eth_trace("tx ring: cpu=%08lx dma=%08lx ndesc=%d\n",
		  (ulong)q->desc, dma_addr, q->ndesc);
	en751221_desc_trace("tx desc0", 0, &q->desc[0]);

	if (qdma->eth->soc->version == 0x7528) {
		qdma->tx_bounce_size = PKTSIZE_ALIGN;
		qdma->tx_bounce = airoha_dma_alloc_uncached(qdma->tx_bounce_size, &dma_addr);
		if (!qdma->tx_bounce)
			return -ENOMEM;
		qdma->tx_bounce_dma = dma_addr;

		memset(qdma->tx_bounce, 0, qdma->tx_bounce_size);
		eth_trace("tx bounce: cpu=%08lx dma=%08lx size=%zu\n",
			  (ulong)qdma->tx_bounce, qdma->tx_bounce_dma,
			  qdma->tx_bounce_size);
	}

	return 0;
}

static int en751221_qdma_init_tx_irq(struct airoha_qdma *qdma)
{
	struct airoha_tx_irq_queue *irq_q = &qdma->q_tx_irq[0];
	unsigned long dma_addr;
	size_t size;
	u32 cfg;

	irq_q->size = qdma->eth->soc->version == 0x7528 ?
		      EN7528_IRQ_QUEUE_LEN : EN751221_IRQ_QUEUE_LEN;
	size = irq_q->size * sizeof(u32);
	irq_q->q = en751221_dma_alloc_control(qdma, size, &dma_addr);
	if (!irq_q->q)
		return -ENOMEM;

	irq_q->qdma = qdma;
	memset(irq_q->q, 0xff, size);
	dma_map_unaligned(irq_q->q, size, DMA_TO_DEVICE);

	airoha_qdma_wr(qdma, EN751221_REG_IRQ_BASE, dma_addr);
	cfg = FIELD_PREP(EN751221_IRQ_THRESHOLD_MASK,
			 qdma->eth->soc->version == 0x7528 ?
			 EN7528_IRQ_THRESHOLD : EN751221_IRQ_THRESHOLD) |
	      FIELD_PREP(EN751221_IRQ_DEPTH_MASK, irq_q->size);
	airoha_qdma_wr(qdma, EN751221_REG_IRQ_CFG, cfg);

	eth_trace("tx irq q: cpu=%08lx dma=%08lx depth=%d cfg=%08x\n",
		  (ulong)irq_q->q, dma_addr, irq_q->size, cfg);

	return 0;
}

static void en751221_qdma_reset_tx(struct airoha_qdma *qdma)
{
	struct airoha_queue *q = &qdma->q_tx[0];
	int i;

	for (i = 0; i < q->ndesc; i++)
		en751221_qdma_reset_tx_desc(q, i);

	q->head = 0;
	airoha_qdma_wr(qdma, EN751221_REG_TX_CPU_IDX, 0);
	airoha_qdma_wr(qdma, EN751221_REG_TX_DMA_IDX, 0);
}

static int en751221_qdma_init_hfwd(struct airoha_qdma *qdma)
{
	unsigned long dma_addr;
	u32 status, val;
	int num_desc, low_thr, size;

	/*
	 * EN7528 TCBoot uses a deliberately small bootloader profile: 32 HWF
	 * descriptors, low watermark 1 and 2048-byte forwarding buffers.  Keep
	 * the larger Linux-derived profile for EN751221.
	 */
	if (qdma->eth->soc->version == 0x7528) {
		num_desc = EN7528_HW_DSCP_NUM;
		low_thr = EN7528_HWFWD_LOW_THRESHOLD;
	} else {
		num_desc = EN751221_HW_DSCP_NUM;
		low_thr = EN751221_HWFWD_LOW_THRESHOLD;
	}

	size = num_desc * sizeof(struct en751221_qdma_fwd_desc);
	qdma->hfwd.desc = en751221_dma_alloc_control(qdma, size, &dma_addr);
	if (!qdma->hfwd.desc)
		return -ENOMEM;
	memset(qdma->hfwd.desc, 0, size);
	dma_map_unaligned(qdma->hfwd.desc, size, DMA_TO_DEVICE);
	airoha_qdma_wr(qdma, EN751221_REG_FWD_DSCP_BASE, dma_addr);

	/*
	 * LMGR derives the address of buffer N as base + N * payload_size, and
	 * the vendor driver always hands it a page-aligned dma_alloc_coherent()
	 * region.  A plain ARCH_DMA_MINALIGN allocation lands at an arbitrary
	 * 32-byte boundary, so force the payload alignment explicitly.
	 */
	size = AIROHA_MAX_PACKET_SIZE * num_desc;
	qdma->hfwd.q = airoha_dma_alloc_aligned(size, AIROHA_MAX_PACKET_SIZE,
						&dma_addr);
	if (!qdma->hfwd.q)
		return -ENOMEM;
	memset(qdma->hfwd.q, 0, size);
	dma_map_single(qdma->hfwd.q, size, DMA_TO_DEVICE);
	airoha_qdma_wr(qdma, EN751221_REG_FWD_BUF_BASE, dma_addr);

	val = FIELD_PREP(HW_FWD_DSCP_PAYLOAD_SIZE_MASK, 0) |
	      FIELD_PREP(EN751221_HWFWD_LOW_THR_MASK, low_thr);
	airoha_qdma_wr(qdma, EN751221_REG_FWD_DSCP_CFG, val);

	/*
	 * Match en75_init_hw_fwd(): preserve OVERHEAD_EN, program the hardware
	 * forwarding descriptor count and set the accounting overhead to 0x14
	 * before kicking LMGR.
	 */
	airoha_qdma_rmw(qdma, EN751221_REG_LMGR_INIT_CFG,
			EN751221_HWFWD_DESC_NUM_MASK |
			EN751221_HWFWD_OVERHEAD_MASK,
			FIELD_PREP(EN751221_HWFWD_DESC_NUM_MASK, num_desc) |
			FIELD_PREP(EN751221_HWFWD_OVERHEAD_MASK,
				   qdma->eth->soc->version == 0x7528 ?
				   EN7528_HWFWD_OVERHEAD :
				   EN751221_HWFWD_OVERHEAD));
	airoha_qdma_set(qdma, EN751221_REG_LMGR_INIT_CFG, LMGR_INIT_START);

	if (read_poll_timeout(airoha_qdma_rr, status,
			      !(status & LMGR_INIT_START), USEC_PER_MSEC,
			      30 * USEC_PER_MSEC, qdma,
			      EN751221_REG_LMGR_INIT_CFG))
		return -ETIMEDOUT;

	/*
	 * EMPTY/LOW assert while LMGR is being seeded and are sticky in the
	 * generation-1 status register.  Clear them after initialization so a
	 * later TX timeout reports the current datapath state, not startup noise.
	 */
	airoha_qdma_wr(qdma, EN751221_REG_INT_STATUS, 0xffffffff);

	return 0;
}

static int en751221_qdma_init(struct airoha_qdma *qdma)
{
	u32 cfg;
	int ret, i;

	en751221_qdma_trace(qdma, "qdma: entry");

	/* Stop the engine before replacing all descriptor rings. */
	airoha_qdma_wr(qdma, REG_QDMA_GLOBAL_CFG, 0);
	airoha_qdma_wr(qdma, EN751221_REG_INT_ENABLE, 0);
	airoha_qdma_wr(qdma, EN751221_REG_INT_STATUS, 0xffffffff);

	ret = en751221_qdma_init_rx(qdma);
	if (ret)
		return ret;

	ret = en751221_qdma_init_tx(qdma);
	if (ret)
		return ret;

	ret = en751221_qdma_init_tx_irq(qdma);
	if (ret)
		return ret;

	en751221_qdma_trace(qdma, "qdma: rings ok");

	ret = en751221_qdma_init_hfwd(qdma);
	if (ret)
		return ret;

	en751221_qdma_trace(qdma, "qdma: lmgr ok");

	/*
	 * Match the bootloader profile actually left by TCBoot on EN7528.
	 * Unlike Linux, it does not request TX descriptor writeback; completion
	 * is observed through TX_DMA_IDX / the done queue.
	 */
	if (qdma->eth->soc->version == 0x7528) {
		cfg = GLOBAL_CFG_RX_2B_OFFSET_MASK |
		      EN751221_GLOBAL_CFG_MSG_WORD_SWAP |
		      GLOBAL_CFG_PAYLOAD_BYTE_SWAP_MASK |
		      EN7528_GLOBAL_CFG_TX_IMMEDIATE_DONE |
		      GLOBAL_CFG_IRQ0_EN_MASK |
		      FIELD_PREP(GLOBAL_CFG_MAX_ISSUE_NUM_MASK, 3);

		/* Vendor 7512_eth.c: native BE descriptors require bit 27. */
		if (qdma->eth->soc->dscp_byte_swap)
			cfg |= GLOBAL_CFG_DSCP_BYTE_SWAP_MASK;

		airoha_qdma_wr(qdma, EN751221_REG_RX_PROTECT_CFG,
			       EN7528_RX_PROTECT_CFG);
		for (i = 0; i < EN751221_NUM_TXQ_DIS_CFG; i++)
			airoha_qdma_wr(qdma, EN751221_REG_TXQ_DIS_CFG(i), 0);

		airoha_qdma_wr(qdma, EN751221_REG_TXQ_CNGST_CFG,
			       EN7528_TXQ_CNGST_CFG);
		airoha_qdma_wr(qdma, EN751221_REG_TXQ_DYN_TOTALTHR,
			       EN7528_TXQ_TOTAL_THR);
		airoha_qdma_wr(qdma, EN751221_REG_TXQ_DYN_CHNLTHR,
			       EN7528_TXQ_CHNL_THR);
		airoha_qdma_wr(qdma, EN751221_REG_TXQ_DYN_QUEUETHR,
			       EN7528_TXQ_QUEUE_THR);
		airoha_qdma_wr(qdma, EN751221_REG_INT_ENABLE,
			       EN7528_INT_ENABLE);
	} else {
		cfg = GLOBAL_CFG_TX_WB_DONE_MASK |
		      FIELD_PREP(GLOBAL_CFG_MAX_ISSUE_NUM_MASK, 3) |
		      GLOBAL_CFG_IRQ0_EN_MASK |
		      GLOBAL_CFG_CHECK_DONE_MASK |
		      EN751221_GLOBAL_CFG_MSG_WORD_SWAP |
		      GLOBAL_CFG_PAYLOAD_BYTE_SWAP_MASK;
		if (qdma->eth->soc->dscp_byte_swap)
			cfg |= GLOBAL_CFG_DSCP_BYTE_SWAP_MASK;

		airoha_qdma_set(qdma, EN751221_REG_RX_PROTECT_CFG,
				EN751221_RX_PROTECT_EN);
		for (i = 0; i < EN751221_NUM_TXQ_DIS_CFG; i++)
			airoha_qdma_wr(qdma, EN751221_REG_TXQ_DIS_CFG(i),
				       i ? 0xffffffff : ~BIT(0));

		airoha_qdma_wr(qdma, EN751221_REG_TXQ_DYN_TOTALTHR,
			       FIELD_PREP(EN751221_TXQ_THR_MAX_MASK,
					  EN751221_TXQ_TOTAL_MAX) |
			       FIELD_PREP(EN751221_TXQ_THR_MIN_MASK,
					  EN751221_TXQ_TOTAL_MIN));
		airoha_qdma_wr(qdma, EN751221_REG_TXQ_DYN_CHNLTHR,
			       FIELD_PREP(EN751221_TXQ_THR_MAX_MASK,
					  EN751221_TXQ_CHNL_MAX) |
			       FIELD_PREP(EN751221_TXQ_THR_MIN_MASK,
					  EN751221_TXQ_CHNL_MIN));
		airoha_qdma_wr(qdma, EN751221_REG_TXQ_DYN_QUEUETHR,
			       FIELD_PREP(EN751221_TXQ_THR_MAX_MASK,
					  EN751221_TXQ_QUEUE_MAX) |
			       FIELD_PREP(EN751221_TXQ_THR_MIN_MASK,
					  EN751221_TXQ_QUEUE_MIN));

		airoha_qdma_rmw(qdma, EN751221_REG_TXQ_CNGST_CFG,
				EN751221_TXQ_DROP_EN | EN751221_TXQ_DEI_DROP_EN |
				EN751221_TXQ_DYN_THR_EN |
				EN751221_TXQ_CNGST_RSV_MASK |
				EN751221_TXQ_DYN_ALPHA_MASK |
				EN751221_TXQ_CNGST_MODE_MASK |
				EN751221_TXQ_CNGST_EN_MASK |
				EN751221_TXQ_CNGST_THR_MASK,
				EN751221_TXQ_DROP_EN | EN751221_TXQ_DEI_DROP_EN |
				EN751221_TXQ_DYN_THR_EN |
				FIELD_PREP(EN751221_TXQ_CNGST_EN_MASK, 0x7) |
				FIELD_PREP(EN751221_TXQ_CNGST_THR_MASK,
					   EN751221_TXQ_CNGST_THR));
	}

	eth_trace("qdma: txq cngst=%08x total=%08x chnl=%08x queue=%08x rxprot=%08x\n",
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_CNGST_CFG),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DYN_TOTALTHR),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DYN_CHNLTHR),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DYN_QUEUETHR),
		  airoha_qdma_rr(qdma, EN751221_REG_RX_PROTECT_CFG));
	eth_trace("qdma: txqdis %08x %08x %08x %08x %08x %08x %08x %08x\n",
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(0)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(1)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(2)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(3)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(4)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(5)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(6)),
		  airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(7)));

	airoha_qdma_wr(qdma, REG_QDMA_GLOBAL_CFG, cfg);
	airoha_qdma_wr(qdma, EN751221_REG_RX_DELAY_INT, 0);

	en751221_qdma_trace(qdma, "qdma: ready");

	return 0;
}

static u32 en751221_qdma_lmgr_free(struct airoha_qdma *qdma)
{
	return airoha_qdma_rr(qdma, EN751221_REG_DBG_LMGR_STATUS) &
	       EN751221_LMGR_FREE_MASK;
}

static void airoha_qdma_reset_rx_desc(struct airoha_queue *q, int index)
{
	struct airoha_qdma_desc *desc;
	uchar *rx_packet;
	u32 val;

	desc = &q->desc[index];
	rx_packet = net_rx_packets[index];
	index = (index + 1) % q->ndesc;

	dma_map_single(rx_packet, PKTSIZE_ALIGN, DMA_TO_DEVICE);

	WRITE_ONCE(desc->msg0, cpu_to_le32(0));
	WRITE_ONCE(desc->msg1, cpu_to_le32(0));
	WRITE_ONCE(desc->msg2, cpu_to_le32(0));
	WRITE_ONCE(desc->msg3, cpu_to_le32(0));
	WRITE_ONCE(desc->addr, cpu_to_le32(virt_to_phys(rx_packet)));
	WRITE_ONCE(desc->data, cpu_to_le32(index));
	val = FIELD_PREP(QDMA_DESC_LEN_MASK, PKTSIZE_ALIGN);
	WRITE_ONCE(desc->ctrl, cpu_to_le32(val));

	dma_map_unaligned(desc, sizeof(*desc), DMA_TO_DEVICE);
}

static void airoha_qdma_init_rx_desc(struct airoha_queue *q)
{
	int i;

	for (i = 0; i < q->ndesc; i++)
		airoha_qdma_reset_rx_desc(q, i);
}

static int airoha_qdma_init_rx_queue(struct airoha_queue *q,
				     struct airoha_qdma *qdma, int ndesc)
{
	int qid = q - &qdma->q_rx[0];
	unsigned long dma_addr;

	q->ndesc = ndesc;
	q->head = 0;
	q->rx_pkt_offset = qdma->eth->soc->version == 0x7528 ? 2 : 0;

	q->desc = airoha_dma_alloc_coherent(q->ndesc * sizeof(*q->desc), &dma_addr);
	if (!q->desc)
		return -ENOMEM;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));
	dma_map_single(q->desc, q->ndesc * sizeof(*q->desc), DMA_TO_DEVICE);

	airoha_qdma_wr(qdma, REG_RX_RING_BASE(qid), dma_addr);
	airoha_qdma_rmw(qdma, REG_RX_RING_SIZE(qid),
			RX_RING_SIZE_MASK,
			FIELD_PREP(RX_RING_SIZE_MASK, ndesc));

	airoha_qdma_rmw(qdma, REG_RX_RING_SIZE(qid), RX_RING_THR_MASK,
			FIELD_PREP(RX_RING_THR_MASK, 0));
	airoha_qdma_rmw(qdma, REG_RX_CPU_IDX(qid), RX_RING_CPU_IDX_MASK,
			FIELD_PREP(RX_RING_CPU_IDX_MASK, q->ndesc - 1));
	airoha_qdma_rmw(qdma, REG_RX_DMA_IDX(qid), RX_RING_DMA_IDX_MASK,
			FIELD_PREP(RX_RING_DMA_IDX_MASK, q->head));

	return 0;
}

static int airoha_qdma_init_rx(struct airoha_qdma *qdma)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		int err;

		err = airoha_qdma_init_rx_queue(&qdma->q_rx[i], qdma,
						RX_DSCP_NUM);
		if (err)
			return err;
	}

	return 0;
}

static int airoha_qdma_init_tx_queue(struct airoha_queue *q,
				     struct airoha_qdma *qdma, int size)
{
	int qid = q - &qdma->q_tx[0];
	unsigned long dma_addr;

	q->ndesc = size;
	q->head = 0;

	q->desc = airoha_dma_alloc_coherent(q->ndesc * sizeof(*q->desc), &dma_addr);
	if (!q->desc)
		return -ENOMEM;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));
	dma_map_single(q->desc, q->ndesc * sizeof(*q->desc), DMA_TO_DEVICE);

	airoha_qdma_wr(qdma, REG_TX_RING_BASE(qid), dma_addr);
	airoha_qdma_rmw(qdma, REG_TX_CPU_IDX(qid), TX_RING_CPU_IDX_MASK,
			FIELD_PREP(TX_RING_CPU_IDX_MASK, q->head));
	airoha_qdma_rmw(qdma, REG_TX_DMA_IDX(qid), TX_RING_DMA_IDX_MASK,
			FIELD_PREP(TX_RING_DMA_IDX_MASK, q->head));

	return 0;
}

static int airoha_qdma_tx_irq_init(struct airoha_tx_irq_queue *irq_q,
				   struct airoha_qdma *qdma, int size)
{
	int id = irq_q - &qdma->q_tx_irq[0];
	unsigned long dma_addr;

	irq_q->q = airoha_dma_alloc_coherent(size * sizeof(u32), &dma_addr);
	if (!irq_q->q)
		return -ENOMEM;

	memset(irq_q->q, 0xffffffff, size * sizeof(u32));
	irq_q->size = size;
	irq_q->qdma = qdma;

	dma_map_single(irq_q->q, size * sizeof(u32), DMA_TO_DEVICE);

	airoha_qdma_wr(qdma, REG_TX_IRQ_BASE(id), dma_addr);
	airoha_qdma_rmw(qdma, REG_TX_IRQ_CFG(id), TX_IRQ_DEPTH_MASK,
			FIELD_PREP(TX_IRQ_DEPTH_MASK, size));

	return 0;
}

static int airoha_qdma_init_tx(struct airoha_qdma *qdma)
{
	int i, err;

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_irq); i++) {
		err = airoha_qdma_tx_irq_init(&qdma->q_tx_irq[i], qdma,
					      IRQ_QUEUE_LEN);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		err = airoha_qdma_init_tx_queue(&qdma->q_tx[i], qdma,
						TX_DSCP_NUM);
		if (err)
			return err;
	}

	return 0;
}

static int airoha_qdma_init_hfwd_queues(struct airoha_qdma *qdma)
{
	unsigned long dma_addr;
	u32 status;
	int size;

	size = HW_DSCP_NUM * sizeof(struct airoha_qdma_fwd_desc);
	qdma->hfwd.desc = airoha_dma_alloc_coherent(size, &dma_addr);
	if (!qdma->hfwd.desc)
		return -ENOMEM;

	memset(qdma->hfwd.desc, 0, size);
	dma_map_single(qdma->hfwd.desc, size, DMA_TO_DEVICE);

	airoha_qdma_wr(qdma, REG_FWD_DSCP_BASE, dma_addr);

	size = AIROHA_MAX_PACKET_SIZE * HW_DSCP_NUM;
	qdma->hfwd.q = airoha_dma_alloc_coherent(size, &dma_addr);
	if (!qdma->hfwd.q)
		return -ENOMEM;

	memset(qdma->hfwd.q, 0, size);
	dma_map_single(qdma->hfwd.q, size, DMA_TO_DEVICE);

	airoha_qdma_wr(qdma, REG_FWD_BUF_BASE, dma_addr);

	airoha_qdma_rmw(qdma, REG_HW_FWD_DSCP_CFG,
			HW_FWD_DSCP_PAYLOAD_SIZE_MASK |
			HW_FWD_DSCP_MIN_SCATTER_LEN_MASK,
			FIELD_PREP(HW_FWD_DSCP_PAYLOAD_SIZE_MASK, 0) |
			FIELD_PREP(HW_FWD_DSCP_MIN_SCATTER_LEN_MASK, 1));
	airoha_qdma_rmw(qdma, REG_LMGR_INIT_CFG,
			LMGR_INIT_START | LMGR_SRAM_MODE_MASK |
			HW_FWD_DESC_NUM_MASK,
			FIELD_PREP(HW_FWD_DESC_NUM_MASK, HW_DSCP_NUM) |
			LMGR_INIT_START);

	udelay(1000);
	return read_poll_timeout(airoha_qdma_rr, status,
				 !(status & LMGR_INIT_START), USEC_PER_MSEC,
				 30 * USEC_PER_MSEC, qdma,
				 REG_LMGR_INIT_CFG);
}

static int airoha_qdma_hw_init(struct airoha_qdma *qdma)
{
	u32 cfg;
	int i;

	/* clear pending irqs */
	for (i = 0; i < 2; i++)
		airoha_qdma_wr(qdma, REG_INT_STATUS(i), 0xffffffff);

	cfg = GLOBAL_CFG_CPU_TXR_RR_MASK |
	      GLOBAL_CFG_PAYLOAD_BYTE_SWAP_MASK |
	      GLOBAL_CFG_IRQ0_EN_MASK |
	      GLOBAL_CFG_TX_WB_DONE_MASK |
	      FIELD_PREP(GLOBAL_CFG_MAX_ISSUE_NUM_MASK, 3);

	/*
	 * EN7528 uses the same 0x100/0x200 ring layout as the newer QDMA,
	 * but its vendor bootloader also requests the two-byte RX alignment.
	 */
	if (qdma->eth->soc->version == 0x7528)
		cfg |= GLOBAL_CFG_RX_2B_OFFSET_MASK;

	airoha_qdma_wr(qdma, REG_QDMA_GLOBAL_CFG, cfg);

	/* disable qdma rx delay interrupt */
	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		if (!qdma->q_rx[i].ndesc)
			continue;

		airoha_qdma_clear(qdma, REG_RX_DELAY_INT_IDX(i),
				  RX_DELAY_INT_MASK);
	}

	return 0;
}

static int airoha_qdma_init(struct udevice *dev,
			    struct airoha_eth *eth,
			    struct airoha_qdma *qdma)
{
	int err;

	qdma->eth = eth;
	qdma->regs = dev_remap_addr_name(dev, "qdma0");
	if (IS_ERR(qdma->regs))
		return PTR_ERR(qdma->regs);
	if (airoha_uses_legacy_qdma(eth))
		return en751221_qdma_init(qdma);

	err = airoha_qdma_init_rx(qdma);
	if (err)
		return err;

	err = airoha_qdma_init_tx(qdma);
	if (err)
		return err;

	err = airoha_qdma_init_hfwd_queues(qdma);
	if (err)
		return err;

	err = airoha_qdma_hw_init(qdma);
	if (err)
		return err;

	en7528_qdma_trace(qdma, "qdma: en7528");
	return 0;
}

#if defined(CONFIG_PCS_AIROHA)
static int airoha_pcs_init(struct udevice *dev)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct udevice *pcs_dev;
	const char *managed;
	int ret;

	ret = uclass_get_device_by_phandle(UCLASS_MISC, dev, "pcs",
					   &pcs_dev);
	if (ret || !pcs_dev)
		return ret;

	port->pcs_dev = pcs_dev;
	port->mode = dev_read_phy_mode(dev);
	managed = dev_read_string(dev, "managed");
	port->neg_mode = !strncmp(managed, "in-band-status",
				  sizeof("in-band-status"));

	airoha_pcs_pre_config(pcs_dev, port->mode);

	ret = airoha_pcs_post_config(pcs_dev, port->mode);
	if (ret)
		return ret;

	return airoha_pcs_config(pcs_dev, port->neg_mode,
				 port->mode, NULL, true);
}
#endif

static int econet_hw_init(void)
{
	u32 reset_mask = EN751221_RST_QDMA0 | EN751221_RST_QDMA1 |
			 EN751221_RST_FE | EN751221_RST_GSW;
	u32 val;

	/* Expose the frame-engine register window instead of the FE SRAM. */
	__raw_writel(0, (void __iomem *)EN751221_FE_SRAM_SEL);

	/* EN751221 still uses the legacy direct reset path. */
	val = __raw_readl((void __iomem *)EN751221_RESET_CONTROL2);
	eth_trace("reset: rstctrl2=%08x mask=%08x\n", val, reset_mask);
	val |= reset_mask;
	__raw_writel(val, (void __iomem *)EN751221_RESET_CONTROL2);
	mdelay(20);
	val &= ~reset_mask;
	__raw_writel(val, (void __iomem *)EN751221_RESET_CONTROL2);
	mdelay(20);
	eth_trace("reset: released, rstctrl2=%08x\n",
		  __raw_readl((void __iomem *)EN751221_RESET_CONTROL2));

	return 0;
}

static int airoha_hw_init(struct udevice *dev,
			  struct airoha_eth *eth)
{
	int ret, i;

	if (eth->soc->direct_reset)
		return econet_hw_init();

	/* disable xsi */
	if (eth->xsi_rsts.count) {
		ret = reset_assert_bulk(&eth->xsi_rsts);
		if (ret)
			return ret;
	}

	ret = reset_assert_bulk(&eth->rsts);
	if (ret)
		return ret;

	mdelay(20);

	ret = reset_deassert_bulk(&eth->rsts);
	if (ret)
		return ret;

	if (eth->xsi_rsts.count) {
		ret = reset_deassert_bulk(&eth->xsi_rsts);
		if (ret)
			return ret;
	}

	mdelay(20);

	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++) {
		ret = airoha_qdma_init(dev, eth, &eth->qdma[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int econet_mdio_wait(struct airoha_eth *eth)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(airoha_switch_rr(eth, SWITCH_PHY_IAC) &
		      SWITCH_PHY_ACS_ST))
			return 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

static int econet_mdio_write(struct airoha_eth *eth, u8 phy, u8 reg, u16 data)
{
	u32 val;
	int ret;

	ret = econet_mdio_wait(eth);
	if (ret)
		return ret;

	val = SWITCH_PHY_ACS_ST | SWITCH_MDIO_ST_CL22 |
	      SWITCH_MDIO_CMD_WRITE |
	      FIELD_PREP(SWITCH_MDIO_PHY_ADDR, phy) |
	      FIELD_PREP(SWITCH_MDIO_REG_ADDR, reg) |
	      FIELD_PREP(SWITCH_MDIO_DATA, data);
	airoha_switch_wr(eth, SWITCH_PHY_IAC, val);

	return econet_mdio_wait(eth);
}

static int econet_mdio_read(struct airoha_eth *eth, u8 phy, u8 reg)
{
	u32 val;
	int ret;

	ret = econet_mdio_wait(eth);
	if (ret)
		return ret;

	val = SWITCH_PHY_ACS_ST | SWITCH_MDIO_ST_CL22 |
	      SWITCH_MDIO_CMD_READ |
	      FIELD_PREP(SWITCH_MDIO_PHY_ADDR, phy) |
	      FIELD_PREP(SWITCH_MDIO_REG_ADDR, reg);
	airoha_switch_wr(eth, SWITCH_PHY_IAC, val);

	ret = econet_mdio_wait(eth);
	if (ret)
		return ret;

	return FIELD_GET(SWITCH_MDIO_DATA,
			 airoha_switch_rr(eth, SWITCH_PHY_IAC));
}

static int econet_mdio_read_mmd(struct airoha_eth *eth, u8 phy, u8 devad,
				u16 regnum)
{
	int ret;

	ret = econet_mdio_write(eth, phy, MII_MMD_CTRL, devad);
	if (ret)
		return ret;

	ret = econet_mdio_write(eth, phy, MII_MMD_DATA, regnum);
	if (ret)
		return ret;

	ret = econet_mdio_write(eth, phy, MII_MMD_CTRL,
				devad | MII_MMD_CTRL_NOINCR);
	if (ret)
		return ret;

	return econet_mdio_read(eth, phy, MII_MMD_DATA);
}

static int econet_mdio_write_mmd(struct airoha_eth *eth, u8 phy, u8 devad,
				 u16 regnum, u16 val)
{
	int ret;

	ret = econet_mdio_write(eth, phy, MII_MMD_CTRL, devad);
	if (ret)
		return ret;

	ret = econet_mdio_write(eth, phy, MII_MMD_DATA, regnum);
	if (ret)
		return ret;

	ret = econet_mdio_write(eth, phy, MII_MMD_CTRL,
				devad | MII_MMD_CTRL_NOINCR);
	if (ret)
		return ret;

	return econet_mdio_write(eth, phy, MII_MMD_DATA, val);
}

static int mt7530_phy_get_eco_rev(struct airoha_eth *eth, u8 phy)
{
	int ret;

	ret = econet_mdio_read_mmd(eth, phy, MDIO_MMD_VEND2,
				   MT7530_PHY_ECO_REV_REG);
	if (ret < 0)
		return ret;

	return FIELD_GET(MT7530_PHY_ECO_REV_MASK, ret);
}

static int en751221_mt7530_pbus_read(struct airoha_eth *eth, u32 reg, u32 *val)
{
	int lo, hi, ret;

	ret = econet_mdio_write(eth, MT7530_PBUS_PHY, MT7530_PBUS_PAGE_REG,
				reg >> 6);
	if (ret)
		return ret;

	lo = econet_mdio_read(eth, MT7530_PBUS_PHY, (reg >> 2) & 0xf);
	if (lo < 0)
		return lo;

	hi = econet_mdio_read(eth, MT7530_PBUS_PHY, MT7530_PBUS_HIGH_REG);
	if (hi < 0)
		return hi;

	*val = (u32)lo | ((u32)hi << 16);
	return 0;
}

static int en751221_mt7530_pbus_write(struct airoha_eth *eth, u32 reg, u32 val)
{
	int ret;

	ret = econet_mdio_write(eth, MT7530_PBUS_PHY, MT7530_PBUS_PAGE_REG,
				reg >> 6);
	if (ret)
		return ret;

	ret = econet_mdio_write(eth, MT7530_PBUS_PHY, (reg >> 2) & 0xf,
				val & 0xffff);
	if (ret)
		return ret;

	return econet_mdio_write(eth, MT7530_PBUS_PHY, MT7530_PBUS_HIGH_REG,
				 val >> 16);
}

static int en751221_mt7530_detect(struct airoha_eth *eth, bool *found)
{
	u32 rev;
	int ret;

	ret = en751221_mt7530_pbus_read(eth, MT7530_CHIP_REV, &rev);
	if (ret)
		return ret;

	*found = (rev & MT7530_CHIP_ID_MASK) == MT7530_CHIP_ID;
	return 0;
}

static int en751221_mt7530_phy_init(struct airoha_eth *eth)
{
	int phy, id1, id2, eco_rev, tries, ret;

	for (phy = 0; phy < MT7530_EXT_PHY_NUM; phy++) {
		id1 = econet_mdio_read(eth, phy, MII_PHYSID1);
		if (id1 < 0)
			return id1;
		id2 = econet_mdio_read(eth, phy, MII_PHYSID2);
		if (id2 < 0)
			return id2;

		if (id1 != MT7530_EXT_PHY_ID1 || id2 != MT7530_EXT_PHY_ID2)
			continue;

		ret = econet_mdio_write(eth, phy, MII_BMCR, BMCR_RESET);
		if (ret)
			return ret;
		for (tries = 100; tries > 0; tries--) {
			int bmcr = econet_mdio_read(eth, phy, MII_BMCR);

			if (bmcr < 0)
				return bmcr;
			if (!(bmcr & BMCR_RESET))
				break;
			mdelay(1);
		}
		if (!tries)
			return -ETIMEDOUT;

		eco_rev = mt7530_phy_get_eco_rev(eth, phy);
		if (eco_rev < 0)
			return eco_rev;

		/* mt7530GePhyCfgLoad(E4.0) from the vendor bootloader. */
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0417, 0x7775);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1e, 0x00a6, 0x0350);
		if (ret)
			return ret;

		/* Same EN751221-MCM EEE workaround used by the Linux PHY driver. */
		ret = econet_mdio_write_mmd(eth, phy, MDIO_MMD_AN, 0x003c, 0);
		if (ret)
			return ret;

		ret = econet_mdio_write(eth, phy, MII_CTRL1000, 0x1e00);
		if (ret)
			return ret;
		ret = econet_mdio_write(eth, phy, MII_BMCR,
					BMCR_ANENABLE | BMCR_ANRESTART);
		if (ret)
			return ret;
	}

	return 0;
}

static int en751221_mt7530_init(struct airoha_eth *eth)
{
	u32 reg, pmcr_down, pmcr_up;
	bool found;
	int i, ret;

	ret = en751221_mt7530_detect(eth, &found);
	if (ret || !found)
		return ret;

	/* TCBoot detect_switch() resets the external MT7530 before TRGMII. */
	ret = en751221_mt7530_pbus_write(eth, MT7530_SYS_CTRL, BIT(1) | BIT(0));
	if (ret)
		return ret;
	udelay(100);

	ret = en751221_mt7530_pbus_read(eth, MT7530_FC_CTRL, &reg);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, MT7530_FC_CTRL, reg & ~BIT(31));
	if (ret)
		return ret;

	/* Enable TRGMII on both ends exactly as macCfgExtSwitch(). */
	ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_CLK_CG, 0);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, MT7530_MHWTRAP, 0x01017e8f);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_CLK_CG, 1);
	if (ret)
		return ret;
	airoha_switch_wr(eth, MT7530_TRGMII_CLK_CG, 0);
	airoha_switch_wr(eth, MT7530_MHWTRAP, 0x01017e8f);
	airoha_switch_wr(eth, MT7530_TRGMII_CLK_CG, 1);

	/* 200 MHz TRGMII clock programming used by the EN751221 SDK. */
	for (i = 0; i < 2; i++) {
		u8 phy = i ? 12 : 0;

		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0404, 0x1000);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0409, 0x0057);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x040a, 0x0057);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0403, 0x1800);
		if (ret)
			return ret;
		mdelay(5);
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0403, 0x1c00);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0401, 0xc020);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0406, 0xa030);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0406, 0xa038);
		if (ret)
			return ret;
		ret = econet_mdio_write_mmd(eth, phy, 0x1f, 0x0410, 0x0003);
		if (ret)
			return ret;
	}
	mdelay(50);

	pmcr_down = SWITCH_IPG_CFG_SHRINK | SWITCH_MAC_MODE |
		    SWITCH_FORCE_MODE | SWITCH_MAC_RX_EN | SWITCH_BKOFF_EN |
		    SWITCH_BKPR_EN | SWITCH_FORCE_SPD_1000 | SWITCH_FORCE_DPX;
	pmcr_up = pmcr_down | SWITCH_MAC_TX_EN | SWITCH_FORCE_LNK;

	airoha_switch_wr(eth, SWITCH_PMCR(5), pmcr_down);
	ret = en751221_mt7530_pbus_write(eth, SWITCH_PMCR(6), pmcr_down);
	if (ret)
		return ret;
	mdelay(5);

	ret = en751221_mt7530_pbus_read(eth, MT7530_TRGMII_RCK_CTRL, &reg);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_RCK_CTRL,
					 reg | BIT(28));
	if (ret)
		return ret;
	mdelay(5);
	ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_RCK_CTRL,
					 reg & ~BIT(28));
	if (ret)
		return ret;

	reg = airoha_switch_rr(eth, MT7530_TRGMII_RCK_CTRL);
	airoha_switch_wr(eth, MT7530_TRGMII_RCK_CTRL, reg | BIT(28));
	mdelay(5);
	airoha_switch_wr(eth, MT7530_TRGMII_RCK_CTRL, reg & ~BIT(28));

	ret = en751221_mt7530_pbus_read(eth, MT7530_TRGMII_CTRL, &reg);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_CTRL, reg | BIT(31));
	if (ret)
		return ret;
	reg = airoha_switch_rr(eth, MT7530_TRGMII_CTRL);
	airoha_switch_wr(eth, MT7530_TRGMII_CTRL, reg | BIT(31));

	for (i = 0; i < 6; i++) {
		u32 val = i == 5 ? 0x77 : 0xaa;

		ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_TD_ODT(i), val);
		if (ret)
			return ret;
		airoha_switch_wr(eth, MT7530_TRGMII_TD_ODT(i), val);
	}

	ret = en751221_mt7530_pbus_write(eth, MT7530_P6ECR, 1);
	if (ret)
		return ret;
	airoha_switch_wr(eth, MT7530_P6ECR, 1);
	mdelay(5);

	ret = en751221_mt7530_pbus_read(eth, MT7530_TRGMII_CTRL, &reg);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, MT7530_TRGMII_CTRL,
					 reg & ~BIT(31));
	if (ret)
		return ret;
	reg = airoha_switch_rr(eth, MT7530_TRGMII_CTRL);
	airoha_switch_wr(eth, MT7530_TRGMII_CTRL, reg & ~BIT(31));

	airoha_switch_wr(eth, SWITCH_PSC(5), 0x000fff10);
	airoha_switch_wr(eth, SWITCH_PSC(6), 0x000fff10);
	airoha_switch_wr(eth, SWITCH_PMCR(5), pmcr_up);
	ret = en751221_mt7530_pbus_write(eth, SWITCH_PMCR(6), pmcr_up);
	if (ret)
		return ret;

	/* External switch CPU/flood setup from macSetGSW(). */
	ret = en751221_mt7530_pbus_write(eth, SWITCH_MFC, EN751221_EXT_MFC);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, 0x00a4, 0x08160816);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, 0x001c, 0x68166816);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, 0x0020, 0x08160816);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, 0x0024, 0x08160816);
	if (ret)
		return ret;
	ret = en751221_mt7530_pbus_write(eth, 0x7810, 0x00010022);
	if (ret)
		return ret;

	ret = en751221_mt7530_phy_init(eth);
	if (ret)
		return ret;

	return 0;
}

static int en751221_phy_init(struct airoha_eth *eth)
{
	bool e3;
	u16 phy_reset = 0x2000;
	int phy, ret;

	/*
	 * PHYs 8..11 are the EN751221 integrated PHY block. The vendor E3
	 * workaround is keyed by the SoC family/revision, not by the ECO
	 * revision of the external MT7530 PHYs.
	 */
	e3 = get_pkg() == EN751221_PKG && get_pdidr() == 3;
	if (e3)
		phy_reset = 0x2020;

	eth_trace("EN751221 internal PHY reset=%04x e3=%d\n",
		  phy_reset, e3);

	for (phy = 8; phy <= 11; phy++) {
		ret = econet_mdio_write(eth, phy, 0x1f, 0x8000);
		if (ret)
			return ret;

		/* restante da sequência atual */
	}

	return 0;
}

static int en7528_phy_init(struct airoha_eth *eth)
{
	int phy, ret;

	/*
	 * EN7528 exposes its four integrated GEPHYs at MDIO addresses 9..12.
	 * The older shared macPhyReset() helper used by EN7512 hard-codes
	 * 8..11, but the EN7528-specific vendor PHY driver and bootrom both
	 * operate on 9..12.  TCBoot used to leave these PHYs initialized before
	 * chainloading U-Boot, so direct cold boot must reset the correct range.
	 */
	for (phy = 9; phy <= 12; phy++) {
		ret = econet_mdio_write(eth, phy, 0x1f, 0x8000);
		if (ret)
			return ret;

		ret = econet_mdio_write(eth, phy, 0x1e, 0x2000);
		if (ret)
			return ret;
	}

	/* bootram tc3162_eth_init() gives the analog PHYs one second here. */
	mdelay(1000);

	return 0;
}

static int airoha_switch_init(struct udevice *dev, struct airoha_eth *eth)
{
	struct airoha_eth_soc_data *data = (void *)dev_get_driver_data(dev);
	ofnode switch_node;
	fdt_addr_t addr;

	switch_node = ofnode_by_compatible(ofnode_null(),
					   data->switch_compatible);
	if (!ofnode_valid(switch_node))
		return -EINVAL;

	addr = ofnode_get_addr(switch_node);
	if (addr == FDT_ADDR_T_NONE)
		return -ENOMEM;

	/* Switch doesn't have a DEV, gets address and setup Flood and CPU port */
	eth->switch_regs = map_sysmem(addr, 0);

	if (data->version == 0x7512) {
		int ret;

		ret = en751221_mt7530_init(eth);
		if (ret) {
			printf("EN751221 external MT7530 init failed: %d\n", ret);
			return ret;
		}

		ret = en751221_phy_init(eth);
		if (ret) {
			printf("EN751221 internal PHY init failed: %d\n", ret);
			return ret;
		}
	}

	/*
	 * EN7528 uses the integrated EN751627-style switch.  Configure the
	 * GEPHY clock path, restart the PHY analog blocks exactly as the vendor
	 * bootloader does, then restore the known-good switch forwarding state.
	 */
	if (data->version == 0x7528) {
		int port, ret;

		airoha_switch_wr(eth, SWITCH_GEPHY_CONN_CFG,
				 EN7528_TCBOOT_GEPHY_CONN_CFG);

		ret = en7528_phy_init(eth);
		if (ret) {
			printf("EN7528 GEPHY init failed: %d\n", ret);
			return ret;
		}

		/*
		 * Do not depend on TCBoot's per-port VLAN/isolation state. The
		 * EN751627 plain-LAN configuration designates P6 as the CPU
		 * port while flooding unknown traffic to all ports: 0xffffffe0.
		 */
		airoha_switch_wr(eth, SWITCH_MFC,
				 SWITCH_BC_FFP | SWITCH_UNM_FFP | SWITCH_UNU_FFP |
				 SWITCH_CPU_EN | FIELD_PREP(SWITCH_CPU_PORT, 6));
		airoha_switch_wr(eth, SWITCH_PMCR(6), EN7528_TCBOOT_PMCR6);

		for (port = 1; port <= 4; port++) {
			airoha_switch_wr(eth, SWITCH_PCR(port), EN7528_TCBOOT_PCR);
			airoha_switch_wr(eth, SWITCH_PSC(port), EN7528_TCBOOT_PSC);
			airoha_switch_wr(eth, SWITCH_PVC(port), EN7528_TCBOOT_PVC);
			airoha_switch_wr(eth, SWITCH_PPBV1(port),
					 EN7528_TCBOOT_PPBV1);
			airoha_switch_wr(eth, SWITCH_PMCR(port),
					 EN7528_TCBOOT_PMCR_USER);
		}

		airoha_switch_wr(eth, SWITCH_PCR(6), EN7528_TCBOOT_PCR);
		airoha_switch_wr(eth, SWITCH_PSC(6), EN7528_TCBOOT_PSC);
		airoha_switch_wr(eth, SWITCH_PVC(6), EN7528_TCBOOT_PVC);
		airoha_switch_wr(eth, SWITCH_PPBV1(6), EN7528_TCBOOT_PPBV1);
		airoha_switch_wr(eth, SWITCH_APC, EN7528_VENDOR_APC);
		airoha_switch_wr(eth, SWITCH_PHY_POLL, 0x7f7f8c08);

		/* Enable per-port MIB counters used by the EN7528 TX-path trace. */
		airoha_switch_wr(eth, SWITCH_MIB_CCR, SWITCH_MIB_OCT_CNT_MASK);
		airoha_switch_wr(eth, SWITCH_MIB_CCR,
				 SWITCH_MIB_ENABLE | SWITCH_MIB_OCT_CNT_MASK);

		for (port = 1; port <= 4; port++)
			eth_trace("switch: p%d pcr=%08x psc=%08x pvc=%08x ppbv1=%08x pmcr=%08x\n",
				  port, airoha_switch_rr(eth, SWITCH_PCR(port)),
				  airoha_switch_rr(eth, SWITCH_PSC(port)),
				  airoha_switch_rr(eth, SWITCH_PVC(port)),
				  airoha_switch_rr(eth, SWITCH_PPBV1(port)),
				  airoha_switch_rr(eth, SWITCH_PMCR(port)));

		eth_trace("switch: mfc=%08x pmcr6=%08x pcr6=%08x pvc6=%08x apc=%08x sysctrl=%08x\n",
			  airoha_switch_rr(eth, SWITCH_MFC),
			  airoha_switch_rr(eth, SWITCH_PMCR(6)),
			  airoha_switch_rr(eth, SWITCH_PCR(6)),
			  airoha_switch_rr(eth, SWITCH_PVC(6)),
			  airoha_switch_rr(eth, SWITCH_APC),
			  airoha_switch_rr(eth, SWITCH_SYS_CTRL));
		return 0;
	}
	if (data->econet) {
		u32 pmcr;

		/* Integrated MT7530: flood unknown traffic to all ports and use P6. */
		airoha_switch_wr(eth, SWITCH_MFC,
				 SWITCH_BC_FFP | SWITCH_UNM_FFP | SWITCH_UNU_FFP |
				 SWITCH_CPU_EN | FIELD_PREP(SWITCH_CPU_PORT, 6));

		pmcr = SWITCH_MAC_MODE | SWITCH_FORCE_MODE | SWITCH_MAC_TX_EN |
		       SWITCH_MAC_RX_EN | SWITCH_BKOFF_EN | SWITCH_BKPR_EN |
		       SWITCH_FORCE_SPD_1000 | SWITCH_FORCE_DPX |
		       SWITCH_FORCE_LNK;

		if (eth->soc->version == 0x7528)
			return 0;

		/*
		 * P5 is the TRGMII cascade to the external MCM switch while P6
		 * is the CPU port.  The EN751221 OEM setup uses shrink IPG only
		 * on the cascade and short IPG on the CPU link.
		 */
		airoha_switch_wr(eth, SWITCH_PMCR(5),
				 pmcr | SWITCH_IPG_CFG_SHRINK);
		airoha_switch_wr(eth, SWITCH_PMCR(6),
				 pmcr | SWITCH_IPG_CFG_SHORT);
		airoha_switch_wr(eth, SWITCH_PHY_POLL, 0x7f7f8c08);

		return 0;
	}

	airoha_switch_wr(eth, SWITCH_MFC, SWITCH_BC_FFP |
			 SWITCH_UNM_FFP | SWITCH_UNU_FFP);
	airoha_switch_wr(eth, SWITCH_PMCR(6),
			 SWITCH_IPG_CFG_SHORT | SWITCH_MAC_MODE |
			 SWITCH_FORCE_MODE | SWITCH_MAC_TX_EN |
			 SWITCH_MAC_RX_EN | SWITCH_BKOFF_EN |
			 SWITCH_BKPR_EN | SWITCH_FORCE_SPD_1000 |
			 SWITCH_FORCE_DPX | SWITCH_FORCE_LNK);

	if (data->gen1)
		eth_trace("switch: mfc=%08x pmcr6=%08x pcr6=%08x apc=%08x sysctrl=%08x\n",
			  airoha_switch_rr(eth, SWITCH_MFC),
			  airoha_switch_rr(eth, SWITCH_PMCR(6)),
			  airoha_switch_rr(eth, SWITCH_PCR(6)),
			  airoha_switch_rr(eth, SWITCH_APC),
			  airoha_switch_rr(eth, SWITCH_SYS_CTRL));

	/* Sideband signal error for Port 3, which need the auto polling */
	airoha_switch_wr(eth, SWITCH_PHY_POLL,
			 FIELD_PREP(SWITCH_PHY_AP_EN, 0x7f) |
			 FIELD_PREP(SWITCH_EEE_POLL_EN, 0x7f) |
			 SWITCH_PHY_PRE_EN |
			 FIELD_PREP(SWITCH_PHY_END_ADDR, 0xc) |
			 FIELD_PREP(SWITCH_PHY_ST_ADDR, 0x8));

	/* AN7583 require tweak to GEPHY_CONN_CFG and clear PHY BMCR_PDOWN */
	if (!strcmp(data->switch_compatible, "airoha,an7583-switch")) {
		int i;

		airoha_switch_rmw(eth, SWITCH_GEPHY_CONN_CFG,
				  SWITCH_DPHY_CKIN_SEL |
				  SWITCH_PHY_CORE_REG_CLK_SEL |
				  SWITCH_ETHER_AFE_PWD,
				  SWITCH_DPHY_CKIN_SEL |
				  SWITCH_PHY_CORE_REG_CLK_SEL |
				  FIELD_PREP(SWITCH_ETHER_AFE_PWD, 0));

		/* Disable BMCR_PDOWN for every PHY */
		for (i = 0; i < AIROHA_MAX_NUM_SWITCH_PORT; i++) {
			int try;
			u32 val;

			airoha_switch_wr(eth, SWITCH_PBUS_PHY_IAC,
					 SWITCH_PBUS_PHY_START |
					 SWITCH_PBUS_PHY_CMD_READ |
					 FIELD_PREP(SWITCH_PBUS_PHY_PORTADDR, i) |
					 FIELD_PREP(SWITCH_PBUS_PHY_REGADDR,
						    AIROHA_PBUS_C22_MASK | MII_BMCR));

			for (try = 0; try < AIROHA_MAX_PBUS_TRY; try++) {
				val = airoha_switch_rr(eth, SWITCH_PBUS_PHY_IAC);
				if (!(val & SWITCH_PBUS_PHY_START))
					break;

				udelay(AIROHA_PBUS_SLEEP);
			}

			val = airoha_switch_rr(eth, SWITCH_PBUS_PHY_IARD);
			val &= ~BMCR_PDOWN;

			airoha_switch_wr(eth, SWITCH_PBUS_PHY_IAWD, val);
			airoha_switch_wr(eth, SWITCH_PBUS_PHY_IAC,
					 SWITCH_PBUS_PHY_START |
					 SWITCH_PBUS_PHY_CMD_WRITE |
					 FIELD_PREP(SWITCH_PBUS_PHY_PORTADDR, i) |
					 FIELD_PREP(SWITCH_PBUS_PHY_REGADDR,
						    AIROHA_PBUS_C22_MASK | MII_BMCR));

			for (try = 0; try < AIROHA_MAX_PBUS_TRY; try++) {
				val = airoha_switch_rr(eth, SWITCH_PBUS_PHY_IAC);
				if (!(val & SWITCH_PBUS_PHY_START))
					break;

				udelay(AIROHA_PBUS_SLEEP);
			}
		}
	}

	return 0;
}

static int airoha_alloc_gdm_port(struct udevice *dev, ofnode node)
{
	struct airoha_eth *eth = dev_get_priv(dev);
	struct udevice *gdm_dev;
	struct driver *gdm_drv;
	char *str;
	int ret;
	u32 id;

	gdm_drv = lists_driver_lookup_name("airoha-eth-port");
	if (!gdm_drv)
		return -ENOENT;

	ret = ofnode_read_u32(node, "reg", &id);
	if (ret)
		return ret;
	if (eth->soc->econet)
		id++;

	if (id > AIROHA_MAX_NUM_GDM_PORTS)
		return -EINVAL;

#if !defined(CONFIG_PCS_AIROHA)
	if (id != 1)
		return -ENOTSUPP;
#endif

	str = eth->gdm_port_str[id];
	snprintf(str, AIROHA_GDM_PORT_STRING_LEN,
		 "airoha-gdm%d", id);

	return device_bind_with_driver_data(dev, gdm_drv, str,
					    (ulong)eth, node, &gdm_dev);
}

static struct udevice *airoha_switch_mdio_init(struct udevice *dev)
{
	struct airoha_eth_soc_data *data = (void *)dev_get_driver_data(dev);
	ofnode switch_node, mdio_node;
	struct udevice *mdio_dev;
	int ret;

	if (!CONFIG_IS_ENABLED(MDIO_MT7531_MMIO))
		return NULL;

	switch_node = ofnode_by_compatible(ofnode_null(),
					   data->switch_compatible);
	if (!ofnode_valid(switch_node)) {
		debug("Warning: missing airoha switch node\n");
		return ERR_PTR(-EINVAL);
	}

	mdio_node = ofnode_find_subnode(switch_node, "mdio");
	if (!ofnode_valid(mdio_node)) {
		debug("Warning: missing airoha switch mdio subnode\n");
		return ERR_PTR(-EINVAL);
	}

	ret = device_bind_driver_to_node(dev, "mt7531-mdio-mmio", "mt7531-mdio",
					 mdio_node, &mdio_dev);
	if (ret) {
		debug("Warning: failed to bind airoha switch mdio\n");
		return ERR_PTR(ret);
	}

	return mdio_dev;
}

static int airoha_eth_probe(struct udevice *dev)
{
	struct airoha_eth_soc_data *data = (void *)dev_get_driver_data(dev);
	struct airoha_eth *eth = dev_get_priv(dev);
	struct udevice *mdio_dev;
	ofnode node;
	int i, ret;

	eth->soc = data;
	if (!data->direct_reset) {
#if IS_ENABLED(CONFIG_ARCH_AIROHA) || IS_ENABLED(CONFIG_ARCH_ECONET)
		struct regmap *scu_regmap;

		scu_regmap = airoha_get_scu_regmap();
		if (IS_ERR(scu_regmap))
			return PTR_ERR(scu_regmap);

		/* FEMEM_SEL=1 hides QDMA/FE behind the shared SRAM window. */
		regmap_write(scu_regmap, SCU_SHARE_FEMEM_SEL, 0x0);
#else
		return -EOPNOTSUPP;
#endif
	}

	eth->fe_regs = dev_remap_addr_name(dev, "fe");
	if (!eth->fe_regs)
		return -ENOMEM;

	if (!data->direct_reset) {
		eth->rsts.resets = devm_kcalloc(dev, AIROHA_MAX_NUM_RSTS,
						sizeof(struct reset_ctl), GFP_KERNEL);
		if (!eth->rsts.resets)
			return -ENOMEM;
		eth->rsts.count = AIROHA_MAX_NUM_RSTS;

		if (data->num_xsi_rsts) {
			eth->xsi_rsts.resets = devm_kcalloc(dev, data->num_xsi_rsts,
							    sizeof(struct reset_ctl), GFP_KERNEL);
			if (!eth->xsi_rsts.resets)
				return -ENOMEM;
		}
		eth->xsi_rsts.count = data->num_xsi_rsts;

		ret = reset_get_by_name(dev, "fe", &eth->rsts.resets[0]);
		if (ret)
			return ret;

		ret = reset_get_by_name(dev, "pdma", &eth->rsts.resets[1]);
		if (ret)
			return ret;

		ret = reset_get_by_name(dev, "qdma", &eth->rsts.resets[2]);
		if (ret)
			return ret;

		ret = reset_get_by_name(dev, "switch",
					&eth->rsts.resets[3]);
		if (ret)
			return ret;

		for (i = 0; i < data->num_xsi_rsts; i++) {
			ret = reset_get_by_name(dev, data->xsi_rsts_names[i],
						&eth->xsi_rsts.resets[i]);
			if (ret)
				return ret;
		}
	}

	ret = airoha_hw_init(dev, eth);
	if (ret)
		return ret;

	ret = airoha_switch_init(dev, eth);
	if (ret)
		return ret;

	/*
	 * The EN751221 vendor cold-boot sequence initializes the PHYs before
	 * QDMA.  Chainloaded U-Boot used to inherit both from TCBoot, so keep
	 * this ordering only for the legacy direct-reset path.
	 */
	if (data->direct_reset) {
		ret = airoha_qdma_init(dev, eth, &eth->qdma[0]);
		if (ret)
			return ret;
	}

	/* Switch MDIO PHYs may be used by several GDM devices. */
	if (data->switch_mdio) {
		mdio_dev = airoha_switch_mdio_init(dev);
		if (!IS_ERR_OR_NULL(mdio_dev))
			eth->switch_mdio_dev = mdio_dev;
	}

	ofnode_for_each_subnode(node, dev_ofnode(dev)) {
		if (!ofnode_device_is_compatible(node, "airoha,eth-mac") &&
		    !ofnode_device_is_compatible(node, "econet,eth-mac"))
			continue;

		if (!ofnode_is_enabled(node))
			continue;

		ret = airoha_alloc_gdm_port(dev, node);
		if (ret && ret != -ENOTSUPP)
			return ret;
	}

	return 0;
}

static int airoha_eth_port_of_to_plat(struct udevice *dev)
{
	struct airoha_eth *eth = (void *)dev_get_driver_data(dev);
	struct airoha_gdm_port *port = dev_get_priv(dev);
	u32 id;
	int ret;

	ret = dev_read_u32(dev, "reg", &id);
	if (ret)
		return ret;

	port->id = id + (eth->soc->econet ? 1 : 0);

	return 0;
}

static int airoha_eth_port_probe(struct udevice *dev)
{
	struct airoha_eth *eth = (void *)dev_get_driver_data(dev);
	struct airoha_gdm_port *port = dev_get_priv(dev);
	int ret;

	port->qdma = &eth->qdma[0];

	ret = airoha_fe_init(port);
	if (ret)
		return ret;

	if (port->id > 1) {
#if defined(CONFIG_PCS_AIROHA)
		ret = airoha_pcs_init(dev);
		if (ret)
			return ret;

		port->phydev = dm_eth_phy_connect(dev);
#else
		return -EINVAL;
#endif
	} else {
		/*
		 * GDM1 device connected to airoha switch. Probe airoha switch
		 * mdio to be able set/query states of corresponding LAN ports.
		 */
		if (!eth->switch_mdio_dev)
			return 0;

		ret = device_probe(eth->switch_mdio_dev);
		if (ret) {
			debug("Warning: failed to probe airoha switch mdio\n");
			eth->switch_mdio_dev = NULL;
		}
	}

	return 0;
}

static int airoha_eth_init(struct udevice *dev)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_queue *q;
	int qid;

	qid = 0;
	q = &qdma->q_rx[qid];

	if (airoha_uses_legacy_qdma(qdma->eth)) {
		if (qdma->eth->soc->version == 0x7528)
			en7528_qdma_init_rx_desc(qdma);
		else
			en751221_qdma_init_rx_desc(qdma);
		en751221_qdma_reset_tx(qdma);
	} else {
		airoha_qdma_init_rx_desc(q);
	}

	if (airoha_uses_legacy_qdma(qdma->eth)) {
		/*
		 * Enable the two engines one at a time and sample the LMGR
		 * counters in between, so a pool that drains on TX_DMA_EN can
		 * be told apart from one that drains on RX_DMA_EN.
		 */
		en751221_qdma_trace(qdma, "init: dma off");

		airoha_qdma_set(qdma, REG_QDMA_GLOBAL_CFG,
				GLOBAL_CFG_TX_DMA_EN_MASK);
		udelay(200);
		en751221_qdma_trace(qdma, "init: tx dma on");

		airoha_qdma_set(qdma, REG_QDMA_GLOBAL_CFG,
				GLOBAL_CFG_RX_DMA_EN_MASK);
		udelay(200);
		en751221_qdma_trace(qdma, "init: rx dma on");

		en751221_fe_trace(qdma->eth, port->id, "init: fe");
	} else {
		airoha_qdma_set(qdma, REG_QDMA_GLOBAL_CFG,
				GLOBAL_CFG_TX_DMA_EN_MASK |
				GLOBAL_CFG_RX_DMA_EN_MASK);
	}

#if defined(CONFIG_PCS_AIROHA)
	if (port->id > 1) {
		struct phy_device *phydev = port->phydev;
		int speed, duplex;
		int ret;

		if (phydev) {
			ret = phy_config(phydev);
			if (ret)
				return ret;

			ret = phy_startup(phydev);
			if (ret)
				return ret;

			speed = phydev->speed;
			duplex = phydev->duplex;
		} else {
			duplex = DUPLEX_FULL;

			/* Hardcode speed for linkup */
			switch (port->mode) {
			case PHY_INTERFACE_MODE_USXGMII:
			case PHY_INTERFACE_MODE_10GBASER:
				speed = SPEED_10000;
				break;
			case PHY_INTERFACE_MODE_2500BASEX:
				speed = SPEED_2500;
				break;
			case PHY_INTERFACE_MODE_SGMII:
			case PHY_INTERFACE_MODE_1000BASEX:
				speed = SPEED_1000;
				break;
			default:
				return -EINVAL;
			}
		}

		airoha_pcs_link_up(port->pcs_dev, port->neg_mode, port->mode,
				   speed, duplex);
	}
#endif

	return 0;
}

static void airoha_eth_stop(struct udevice *dev)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;

#if defined(CONFIG_PCS_AIROHA)
	if (port->id > 1) {
		if (port->phydev)
			phy_shutdown(port->phydev);

		airoha_pcs_link_down(port->pcs_dev);
	}
#endif

	airoha_qdma_clear(qdma, REG_QDMA_GLOBAL_CFG,
			  GLOBAL_CFG_TX_DMA_EN_MASK |
			  GLOBAL_CFG_RX_DMA_EN_MASK);

	if (airoha_uses_legacy_qdma(qdma->eth))
		en751221_qdma_trace(qdma, "stop");
}

static int en751221_eth_send(struct udevice *dev, void *packet, int length)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_queue *q = &qdma->q_tx[0];
	struct airoha_qdma_desc *desc;
	dma_addr_t dma_addr = 0;
	bool use_bounce = qdma->eth->soc->version == 0x7528;
	u32 index, next, ctrl = 0, hw = 0, irq_status = 0;
	u32 last_int, last_hw, last_free;
	int tx_len = use_bounce ? max(length, ETH_ZLEN) : length;
	int traced = 0;
	int i;

	if (use_bounce && tx_len > qdma->tx_bounce_size)
		return -EMSGSIZE;

	if (qdma->eth->soc->version == 0x7528)
		en7528_qdma_reap_tx_irq(qdma);

	index = q->head;
	next = (index + 1) % q->ndesc;
	desc = &q->desc[index];
	en751221_packet_trace("TX", packet, length);

	if (use_bounce) {
		memcpy(qdma->tx_bounce, packet, length);
		if (tx_len > length)
			memset((u8 *)qdma->tx_bounce + length, 0, tx_len - length);

		/* Complete uncached payload stores before QDMA reads it. */
		wmb();
		dma_addr = qdma->tx_bounce_dma;
	} else {
		dma_addr = dma_map_single(packet, length, DMA_TO_DEVICE);
	}

	/*
	 * Generation-1 QDMA uses TX_CPUI as a moving null-descriptor pointer,
	 * not as the producer index of a conventional ring.  The descriptor
	 * referenced by the newly published CPUI must remain DONE until it is
	 * turned into the next packet descriptor.
	 */
	en751221_qdma_reset_tx_desc(q, next);

	WRITE_ONCE(desc->rsv, 0);
	WRITE_ONCE(desc->ctrl, FIELD_PREP(QDMA_DESC_LEN_MASK, tx_len));
	WRITE_ONCE(desc->addr, use_bounce ? dma_addr : virt_to_phys(packet));
	WRITE_ONCE(desc->data, next);
	WRITE_ONCE(desc->msg0,
		   FIELD_PREP(EN751221_TXMSG_CHANNEL_MASK, 0) |
		   FIELD_PREP(EN751221_TXMSG_QUEUE_MASK, 0));
	/*
	 * Vendor BE/LE bitfield declarations put fport in bits [21:19] on
	 * both EN751627 and EN7528; VLAN/offload fields stay zero.
	 * GDM1 therefore produces word 1 = 0x00080000.
	 */
	WRITE_ONCE(desc->msg1,
		   FIELD_PREP(EN751221_TXMSG_FPORT_MASK,
			      EN751221_FPORT_GDM1));
	WRITE_ONCE(desc->msg2, 0);
	WRITE_ONCE(desc->msg3, 0);
	dma_map_unaligned(desc, sizeof(*desc), DMA_TO_DEVICE);

	/*
	 * EN75xx is non-coherent MIPS.  Match the Linux/vendor handoff and make
	 * the descriptor globally visible before publishing TX_CPUI to QDMA.
	 */
	eth_trace("tx: len=%d dma_len=%d head=%d next=%d pkt=%08lx dma=%08lx%s\n",
		  length, tx_len, index, next, (ulong)virt_to_phys(packet),
		  (ulong)(use_bounce ? dma_addr : virt_to_phys(packet)),
		  use_bounce ? " bounce" : "");
	en751221_desc_trace("tx: pkt desc", index, desc);
	en751221_desc_trace("tx: null desc", next, &q->desc[next]);
	if (qdma->eth->soc->version == 0x7528 && !(((u8 *)packet)[0] & 1))
		en7528_switch_trace_fdb(qdma->eth, packet);
	en7528_tx_path_trace(qdma->eth, "txpath: before");
	en751221_qdma_trace(qdma, "tx: before kick");

	wmb();
	airoha_qdma_wr(qdma, EN751221_REG_TX_CPU_IDX, next);

	last_int = ~0;
	last_hw = ~0;
	last_free = ~0;

	for (i = 0; i < 10000; i++) {
		dma_unmap_unaligned((dma_addr_t)desc, sizeof(*desc),
				    DMA_FROM_DEVICE);
		ctrl = READ_ONCE(desc->ctrl);
		hw = airoha_qdma_rr(qdma, EN751221_REG_TX_DMA_IDX) &
		     EN751221_RING_IDX_MASK;
		/*
		 * TX_DMA_IDX is the hardware ownership boundary and does not rely
		 * on a descriptor writeback reaching CPU-visible memory.  This is
		 * already required by EN7528, but it also matters on EN751221 when
		 * U-Boot is entered through the vendor recovery path: the QDMA can
		 * consume and transmit the descriptor while ctrl.DONE never becomes
		 * visible.  EN751221 still waits for and consumes the TX completion
		 * queue below before recycling the LMGR context.
		 */
		if (hw == next || (ctrl & QDMA_DESC_DONE_MASK))
			break;

		/*
		 * Sample densely at the start - where the engine either takes
		 * the descriptor or gives up - then thin out, and only emit a
		 * line when something actually moved.  Capped so a stuck
		 * engine cannot flood the console.
		 */
		if (eth_traced && traced < 24 && (i < 8 || !(i % 500))) {
			u32 cur_int, cur_hw, cur_free;

			cur_int = airoha_qdma_rr(qdma,
						 EN751221_REG_INT_STATUS);
			cur_hw = airoha_qdma_rr(qdma, EN751221_REG_TX_DMA_IDX) &
				 EN751221_RING_IDX_MASK;
			cur_free = en751221_qdma_lmgr_free(qdma);

			if (cur_int != last_int || cur_hw != last_hw ||
			    cur_free != last_free) {
				printf("[airoha] tx: wait i=%-5d ctrl=%08x hw=%03x int=%08x free=%04x used=%08x\n",
				       i, ctrl, cur_hw, cur_int, cur_free,
				       airoha_qdma_rr(qdma,
					       EN751221_REG_DBG_HWF_BUF_USAGE));
				last_int = cur_int;
				last_hw = cur_hw;
				last_free = cur_free;
				traced++;
			}
		}

		udelay(1);
	}

	eth_trace("tx: loop end i=%d ctrl=%08x done=%d hw=%03x\n", i, ctrl,
		  !!(ctrl & QDMA_DESC_DONE_MASK), hw);

	if (!use_bounce)
		dma_unmap_single(dma_addr, length, DMA_TO_DEVICE);
	if (hw != next && !(ctrl & QDMA_DESC_DONE_MASK)) {
		printf("QDMA TX timeout: cfg=%08x cpu=%08x hw=%08x int=%08x hwcfg=%08x lmgr=%08x free=%08x used=%08x\n",
		       airoha_qdma_rr(qdma, REG_QDMA_GLOBAL_CFG),
		       airoha_qdma_rr(qdma, EN751221_REG_TX_CPU_IDX),
		       airoha_qdma_rr(qdma, EN751221_REG_TX_DMA_IDX),
		       airoha_qdma_rr(qdma, EN751221_REG_INT_STATUS),
		       airoha_qdma_rr(qdma, EN751221_REG_FWD_DSCP_CFG),
		       airoha_qdma_rr(qdma, EN751221_REG_LMGR_INIT_CFG),
		       airoha_qdma_rr(qdma, EN751221_REG_DBG_LMGR_STATUS),
		       airoha_qdma_rr(qdma, EN751221_REG_DBG_HWF_BUF_USAGE));
		printf("  txbase=%08x desc=%08lx [%08x %08x %08x %08x %08x %08x %08x %08x]\n",
		       airoha_qdma_rr(qdma, EN751221_REG_TX_RING_BASE),
		       (ulong)virt_to_phys(desc),
		       READ_ONCE(desc->rsv), READ_ONCE(desc->ctrl),
		       READ_ONCE(desc->addr), READ_ONCE(desc->data),
		       READ_ONCE(desc->msg0), READ_ONCE(desc->msg1),
		       READ_ONCE(desc->msg2), READ_ONCE(desc->msg3));
		printf("  gdm=%08x cport=%08x mfc=%08x pmcr6=%08x pcr6=%08x\n",
		       airoha_fe_rr(qdma->eth, REG_GDM_FWD_CFG(port->id)),
		       airoha_fe_rr(qdma->eth, REG_FE_CPORT_CFG),
		       airoha_switch_rr(qdma->eth, SWITCH_MFC),
		       airoha_switch_rr(qdma->eth, SWITCH_PMCR(6)),
		       airoha_switch_rr(qdma->eth, SWITCH_PCR(6)));
		printf("  txqdis: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(0)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(1)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(2)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(3)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(4)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(5)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(6)),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DIS_CFG(7)));
		printf("  txq: cngst=%08x total=%08x chnl=%08x queue=%08x rxprot=%08x\n",
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_CNGST_CFG),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DYN_TOTALTHR),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DYN_CHNLTHR),
		       airoha_qdma_rr(qdma, EN751221_REG_TXQ_DYN_QUEUETHR),
		       airoha_qdma_rr(qdma, EN751221_REG_RX_PROTECT_CFG));
		printf("  chn: hwf=%08x txen=%08x rxen=%08x txvld=%08x rxvld=%08x\n",
		       airoha_fe_rr(qdma->eth, REG_CDM1_HWF_CHN_EN),
		       airoha_fe_rr(qdma->eth, REG_GDM_TXCHN_EN(port->id)),
		       airoha_fe_rr(qdma->eth, REG_GDM_RXCHN_EN(port->id)),
		       airoha_fe_rr(qdma->eth, REG_GDM_TX_CHN_VLD(port->id)),
		       airoha_fe_rr(qdma->eth, REG_GDM_RX_CHN_VLD(port->id)));
		return -ETIMEDOUT;
	}

	en7528_tx_path_trace(qdma->eth, "txpath: after");

	/*
	 * EN7528's vendor U-Boot returns as soon as TX is submitted and reaps
	 * the asynchronous completion queue on a later submission.  We already
	 * waited for TX_DMA_IDX above, so an IRQ entry is not a prerequisite for
	 * declaring this descriptor consumed.
	 */
	if (qdma->eth->soc->version == 0x7528) {
		en7528_qdma_reap_tx_irq(qdma);
		en751221_qdma_trace(qdma, "tx: done");
		q->head = next;
		return 0;
	}

	for (i = 0; i < 10000; i++) {
		irq_status = airoha_qdma_rr(qdma, EN751221_REG_IRQ_STATUS);
		if (FIELD_GET(EN751221_IRQ_ENTRY_LEN_MASK, irq_status))
			break;
		udelay(1);
	}
	if (!FIELD_GET(EN751221_IRQ_ENTRY_LEN_MASK, irq_status)) {
		printf("QDMA TX completion timeout: cpu=%08x hw=%08x int=%08x irq=%08x ctrl=%08x\n",
		       airoha_qdma_rr(qdma, EN751221_REG_TX_CPU_IDX),
		       airoha_qdma_rr(qdma, EN751221_REG_TX_DMA_IDX),
		       airoha_qdma_rr(qdma, EN751221_REG_INT_STATUS),
		       irq_status, ctrl);
		return -ETIMEDOUT;
	}

	/*
	 * QDMA consumes one hardware-forward descriptor for each CPU TX
	 * context.  Advancing the completion queue returns that descriptor
	 * to LMGR; without this, the four-entry first-light pool is exhausted
	 * after four packets even though TX writeback reports DONE.
	 */
	eth_trace("tx: irq_status=%08x entry_len=%d head_idx=%d\n",
		  irq_status,
		  (int)FIELD_GET(EN751221_IRQ_ENTRY_LEN_MASK, irq_status),
		  (int)FIELD_GET(EN751221_IRQ_HEAD_IDX_MASK, irq_status));

	airoha_qdma_rmw(qdma, EN751221_REG_IRQ_CLEAR_LEN,
			EN751221_IRQ_CLEAR_LEN_MASK, 1);

	en751221_qdma_trace(qdma, "tx: done");

	q->head = next;
	return 0;
}

static int en751221_eth_recv(struct udevice *dev, uchar **packetp)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_queue *q = &qdma->q_rx[0];
	struct airoha_qdma_desc *desc;
	uchar *rx_packet;
	u32 index, hw_index, ctrl, length, addr;

	index = (q->head + 1) % q->ndesc;
	hw_index = airoha_qdma_rr(qdma, EN751221_REG_RX_DMA_IDX) &
		   EN751221_RING_IDX_MASK;

	/*
	 * EN751627/EN7528 RX_DMA_IDX names the descriptor currently owned by
	 * DMA.  A completed descriptor may therefore still be equal to the
	 * hardware index until another packet advances the engine.  The vendor
	 * receive path tests the descriptor DONE bit directly and does not use
	 * RX_DMA_IDX as an empty-ring test.
	 */
	if (qdma->eth->soc->version != 0x7528 && index == hw_index)
		return -EAGAIN;

	desc = &q->desc[index];
	dma_unmap_unaligned((dma_addr_t)desc, sizeof(*desc),
			    DMA_FROM_DEVICE);
	ctrl = READ_ONCE(desc->ctrl);
	if (!(ctrl & QDMA_DESC_DONE_MASK))
		return -EAGAIN;

	length = FIELD_GET(QDMA_DESC_LEN_MASK, ctrl);
	addr = READ_ONCE(desc->addr);

	en751221_desc_trace("rx: desc", index, desc);
	eth_trace("rx: head=%d index=%d hw=%d ctrl=%08x len=%d\n",
		  q->head, index, hw_index, ctrl, length);

	if (!length || length > q->rx_buf_len) {
		eth_trace("rx: bad length %d, recycling\n", length);
		if (qdma->eth->soc->version == 0x7528 && addr) {
			uchar *bad_packet = phys_to_virt(addr);

			en7528_qdma_recycle_rx_desc(qdma, q, index, bad_packet);
		} else {
			en751221_qdma_reset_rx_desc(q, index);
			airoha_qdma_wr(qdma, EN751221_REG_RX_CPU_IDX, index);
			q->head = index;
		}
		return -EIO;
	}

	rx_packet = phys_to_virt(addr);
	if (qdma->eth->soc->version == 0x7528) {
		rx_packet = EN7528_RX_UNCACHED(rx_packet);
		/* Order descriptor DONE before consuming uncached payload data. */
		rmb();
	} else {
		dma_unmap_single((dma_addr_t)rx_packet,
				 q->rx_buf_len + q->rx_pkt_offset,
				 DMA_FROM_DEVICE);
	}
	rx_packet += q->rx_pkt_offset;
	en751221_packet_trace("RX", rx_packet, length);
	*packetp = rx_packet;

	return length;
}

static int en751221_eth_free_pkt(struct udevice *dev, uchar *packet)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_queue *q = &qdma->q_rx[0];
	u32 index;

	if (!packet)
		return 0;

	index = (q->head + 1) % q->ndesc;
	if (qdma->eth->soc->version == 0x7528) {
		uchar *rx_packet = packet - q->rx_pkt_offset;

		en7528_qdma_recycle_rx_desc(qdma, q, index, rx_packet);
	} else {
		en751221_qdma_reset_rx_desc(q, index);
		airoha_qdma_wr(qdma, EN751221_REG_RX_CPU_IDX, index);
		q->head = index;
	}

	eth_trace("rx: recycled index=%d tail=%d free=%04x\n", index,
		  q->tail, en751221_qdma_lmgr_free(qdma));

	return 0;
}

static int airoha_eth_send(struct udevice *dev, void *packet, int length)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_qdma_desc *desc;
	struct airoha_queue *q;
	dma_addr_t dma_addr;
	u32 msg0, msg1;
	int qid, index;
	u8 fport;
	u32 val;
	int i;

	if (airoha_uses_legacy_qdma(qdma->eth))
		return en751221_eth_send(dev, packet, length);

	/*
	 * Newer GDMs pad short frames in hardware. EN7528 keeps the older
	 * GDM1 forwarding register layout used by TCBoot, where bit 28 is
	 * part of the jumbo-length field rather than GDM_PAD_EN. Pad short
	 * frames in software before handing them to QDMA.
	 */
	if (qdma->eth->soc->version == 0x7528 && length < ETH_ZLEN) {
		memset((u8 *)packet + length, 0, ETH_ZLEN - length);
		length = ETH_ZLEN;
	}

	dma_addr = dma_map_single(packet, length, DMA_TO_DEVICE);

	qid = 0;
	q = &qdma->q_tx[qid];
	desc = &q->desc[q->head];
	index = (q->head + 1) % q->ndesc;

	fport = airoha_get_fe_port(port);

	msg0 = 0;
	msg1 = FIELD_PREP(QDMA_ETH_TXMSG_FPORT_MASK, fport) |
	       FIELD_PREP(QDMA_ETH_TXMSG_METER_MASK, 0x7f);

	val = FIELD_PREP(QDMA_DESC_LEN_MASK, length);
	WRITE_ONCE(desc->ctrl, cpu_to_le32(val));
	WRITE_ONCE(desc->addr, cpu_to_le32(dma_addr));
	val = FIELD_PREP(QDMA_DESC_NEXT_ID_MASK, index);
	WRITE_ONCE(desc->data, cpu_to_le32(val));
	WRITE_ONCE(desc->msg0, cpu_to_le32(msg0));
	WRITE_ONCE(desc->msg1, cpu_to_le32(msg1));
	WRITE_ONCE(desc->msg2, cpu_to_le32(qdma->eth->soc->version == 0x7528 ?
					      0 : 0xffff));
	WRITE_ONCE(desc->msg3, cpu_to_le32(0));

	dma_map_unaligned(desc, sizeof(*desc), DMA_TO_DEVICE);

	en7528_qdma_trace(qdma, "tx: before");
	airoha_qdma_rmw(qdma, REG_TX_CPU_IDX(qid), TX_RING_CPU_IDX_MASK,
			FIELD_PREP(TX_RING_CPU_IDX_MASK, index));

	for (i = 0; i < 100; i++) {
		dma_unmap_unaligned(virt_to_phys(desc), sizeof(*desc),
				    DMA_FROM_DEVICE);
		if (desc->ctrl & QDMA_DESC_DONE_MASK)
			break;

		udelay(1);
	}

	/* Return error if for some reason the descriptor never ACK */
	if (!(desc->ctrl & QDMA_DESC_DONE_MASK)) {
		en7528_qdma_trace(qdma, "tx: timeout");
		return -EAGAIN;
	}

	q->head = index;
	en7528_qdma_trace(qdma, "tx: done");
	airoha_qdma_rmw(qdma, REG_IRQ_CLEAR_LEN(0),
			IRQ_CLEAR_LEN_MASK, 1);

	return 0;
}

static int airoha_eth_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_qdma_desc *desc;
	struct airoha_queue *q;
	u16 length;
	int qid;

	if (airoha_uses_legacy_qdma(qdma->eth))
		return en751221_eth_recv(dev, packetp);

	qid = 0;
	q = &qdma->q_rx[qid];
	desc = &q->desc[q->head];

	dma_unmap_unaligned(virt_to_phys(desc), sizeof(*desc),
			    DMA_FROM_DEVICE);

	if (!(desc->ctrl & QDMA_DESC_DONE_MASK))
		return -EAGAIN;

	length = FIELD_GET(QDMA_DESC_LEN_MASK, desc->ctrl);
	dma_unmap_single(desc->addr, length + q->rx_pkt_offset,
			 DMA_FROM_DEVICE);

	*packetp = (uchar *)phys_to_virt(desc->addr) + q->rx_pkt_offset;

	return length;
}

static int arht_eth_free_pkt(struct udevice *dev, uchar *packet, int length)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct airoha_qdma *qdma = port->qdma;
	struct airoha_queue *q;
	int qid;

	if (airoha_uses_legacy_qdma(qdma->eth))
		return en751221_eth_free_pkt(dev, packet);

	if (!packet)
		return 0;

	qid = 0;
	q = &qdma->q_rx[qid];

	/*
	 * Due to cpu cache issue the airoha_qdma_reset_rx_desc() function
	 * will always touch 2 descriptors placed on the same cacheline:
	 *   - if current descriptor is even, then current and next
	 *     descriptors will be touched
	 *   - if current descriptor is odd, then current and previous
	 *     descriptors will be touched
	 *
	 * Thus, to prevent possible destroying of rx queue, we should:
	 *   - do nothing in the even descriptor case,
	 *   - utilize 2 descriptors (current and previous one) in the
	 *     odd descriptor case.
	 *
	 * WARNING: Observations shows that PKTBUFSRX must be even and
	 *          larger than 7 for reliable driver operations.
	 */
	if (q->head & 0x01) {
		airoha_qdma_reset_rx_desc(q, q->head - 1);
		airoha_qdma_reset_rx_desc(q, q->head);

		airoha_qdma_rmw(qdma, REG_RX_CPU_IDX(qid), RX_RING_CPU_IDX_MASK,
				FIELD_PREP(RX_RING_CPU_IDX_MASK, q->head));
	}

	q->head = (q->head + 1) % q->ndesc;

	return 0;
}

static int arht_eth_write_hwaddr(struct udevice *dev)
{
	struct airoha_gdm_port *port = dev_get_priv(dev);
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct airoha_qdma *qdma = port->qdma;
	unsigned char *mac = pdata->enetaddr;
	u32 macaddr_lsb, macaddr_msb;

	macaddr_lsb = FIELD_PREP(SMACCR0_MAC2, mac[2]) |
		      FIELD_PREP(SMACCR0_MAC3, mac[3]) |
		      FIELD_PREP(SMACCR0_MAC4, mac[4]) |
		      FIELD_PREP(SMACCR0_MAC5, mac[5]);
	macaddr_msb = FIELD_PREP(SMACCR1_MAC1, mac[1]) |
		      FIELD_PREP(SMACCR1_MAC0, mac[0]);

	if (airoha_is_gen1(qdma->eth)) {
		airoha_fe_wr(qdma->eth, REG_GDM_MAC_LSB(port->id), macaddr_lsb);
		airoha_fe_wr(qdma->eth, REG_GDM_MAC_MSB(port->id), macaddr_msb);
	}

	/* Set MAC for Switch */
	airoha_switch_wr(qdma->eth, SWITCH_SMACCR0, macaddr_lsb);
	airoha_switch_wr(qdma->eth, SWITCH_SMACCR1, macaddr_msb);

	return 0;
}

static int airoha_eth_bind(struct udevice *dev)
{
	/*
	 * Force Probe as we set the Main ETH driver as misc
	 * to register multiple eth port for each GDM
	 */
	dev_or_flags(dev, DM_FLAG_PROBE_AFTER_BIND);

	return 0;
}

static const struct airoha_eth_soc_data en7523_data = {
	.version = 0x7523,
	.xsi_rsts_names = en7523_xsi_rsts_names,
	.num_xsi_rsts = ARRAY_SIZE(en7523_xsi_rsts_names),
	.switch_mdio = true,
	.switch_compatible = "airoha,en7523-switch",
};

static const struct airoha_eth_soc_data en751221_data = {
	.version = 0x7512,
	.econet = true,
	.gen1 = true,
	.legacy_qdma = true,
	.direct_reset = true,
	.dscp_byte_swap = true,
	.switch_mdio = true,
	.switch_compatible = "econet,en751221-switch",
};

/* Same QDMA/ring/switch profile, with native big-endian descriptors. */
static const struct airoha_eth_soc_data en751627_data = {
	.version = 0x7528,
	.gen1 = true,
	.legacy_qdma = true,
	.dscp_byte_swap = true,
	.switch_compatible = "airoha,en751627-switch",
};

static const struct airoha_eth_soc_data en7528_data = {
	.version = 0x7528,
	.gen1 = true,
	.legacy_qdma = true,
	.switch_mdio = true,
	.switch_compatible = "airoha,en7528-switch",
};

static const struct airoha_eth_soc_data en7581_data = {
	.version = 0x7581,
	.xsi_rsts_names = en7581_xsi_rsts_names,
	.num_xsi_rsts = ARRAY_SIZE(en7581_xsi_rsts_names),
	.switch_mdio = true,
	.switch_compatible = "airoha,en7581-switch",
};

static const struct airoha_eth_soc_data an7583_data = {
	.xsi_rsts_names = an7583_xsi_rsts_names,
	.num_xsi_rsts = ARRAY_SIZE(an7583_xsi_rsts_names),
	.switch_mdio = true,
	.switch_compatible = "airoha,an7583-switch",
};

static const struct udevice_id airoha_eth_ids[] = {
	{ .compatible = "econet,en751221-eth",
	  .data = (ulong)&en751221_data,
	},
	{ .compatible = "econet,en751627-eth",
	  .data = (ulong)&en751627_data,
	},
	{ .compatible = "econet,en7528-eth",
	  .data = (ulong)&en7528_data,
	},
	{ .compatible = "airoha,en7523-eth",
	  .data = (ulong)&en7523_data,
	},
	{ .compatible = "airoha,en7581-eth",
	  .data = (ulong)&en7581_data,
	},
	{ .compatible = "airoha,an7583-eth",
	  .data = (ulong)&an7583_data,
	},
	{ }
};

static const struct eth_ops airoha_eth_ops = {
	.start = airoha_eth_init,
	.stop = airoha_eth_stop,
	.send = airoha_eth_send,
	.recv = airoha_eth_recv,
	.free_pkt = arht_eth_free_pkt,
	.write_hwaddr = arht_eth_write_hwaddr,
};

U_BOOT_DRIVER(airoha_eth_port) = {
	.name = "airoha-eth-port",
	.id = UCLASS_ETH,
	.of_to_plat = airoha_eth_port_of_to_plat,
	.probe = airoha_eth_port_probe,
	.ops = &airoha_eth_ops,
	.priv_auto = sizeof(struct airoha_gdm_port),
	.plat_auto = sizeof(struct eth_pdata),
};

U_BOOT_DRIVER(airoha_eth) = {
	.name = "airoha-eth",
	.id = UCLASS_MISC,
	.of_match = airoha_eth_ids,
	.probe = airoha_eth_probe,
	.bind = airoha_eth_bind,
	.priv_auto = sizeof(struct airoha_eth),
};
