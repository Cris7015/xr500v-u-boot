/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef ECONET_FLASH_LOADER_H
#define ECONET_FLASH_LOADER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define __iomem
#define BIT(n)				(1U << (n))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define ETIMEDOUT			110
#define EIO					5
#define EINVAL				22
#define ECONET_SFC_BASE		0xbfa10000U
#define __raw_readl(p)		(*(volatile u32 *)(p))
#define __raw_writel(v, p)	(*(volatile u32 *)(p) = (v))

int econet_sfc_init(void);
int econet_sfc_read(u32 offset, void *dst, size_t len);

#endif
