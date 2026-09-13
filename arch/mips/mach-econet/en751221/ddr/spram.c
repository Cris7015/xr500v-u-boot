/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "en751221_ddr.h"

#define ILOAD 4
#define DLOAD 5
#define ISTORE 8
#define DSTORE 9
#define IDATA 12
#define K0 0x80000000U
#define EN 0x80U
#define PAM 0xfffff000U
#define SZM 0xfffff000U
#define ERR BIT(28)

#define cop(op, a)                                                                           \
	__asm__ __volatile__(                                                                \
		".set push\n\t.set noreorder\n\t.set mips3\n\tcache %0,0(%1)\n\t.set pop" :: \
			"i"(op),                                                             \
		"r"((unsigned long)(a))                                                      \
		: "memory")

static inline void h(void)
{
	__asm__ __volatile__("ehb" ::: "memory");
}

static inline unsigned e(void)
{
	unsigned v;
	__asm__("mfc0 %0,$26" : "=r"(v));
	return v;
}

static inline void E(unsigned v)
{
	__asm__("mtc0 %0,$26" ::"r"(v));
}

static inline unsigned it(void)
{
	unsigned v;
	__asm__("mfc0 %0,$28" : "=r"(v));
	return v;
}

static inline void I(unsigned v)
{
	__asm__("mtc0 %0,$28" ::"r"(v));
}

static inline unsigned dt(void)
{
	unsigned v;
	__asm__("mfc0 %0,$28,2" : "=r"(v));
	return v;
}

static inline void D(unsigned v)
{
	__asm__("mtc0 %0,$28,2" ::"r"(v));
}

static unsigned load(unsigned o, int i)
{
	unsigned x = e(), v;
	E(x | ERR);
	h();
	if (i)
		cop(ILOAD, K0 | o);
	else
		cop(DLOAD, K0 | o);
	h();
	v = i ? it() : dt();
	h();
	E(x);
	h();
	return v;
}

static void store(unsigned o, unsigned v, int i)
{
	unsigned x = e();
	E(x | ERR);
	h();
	if (i) {
		I(v);
		cop(ISTORE, K0 | o);
	} else {
		D(v);
		cop(DSTORE, K0 | o);
	}
	h();
	E(x);
	h();
}

void probe_spram(unsigned base, unsigned enable)
{
	int i = (enable & 2) >> 1;
	unsigned t = load(8, i), z = t & SZM;
	if (!z)
		return;
	base = (base + z - 1) & ~(z - 1);
	t = base & PAM;
	if (enable & 1)
		t |= EN;
	store(0, t, i);
}

void ispram_fill(int size, unsigned from, unsigned to)
{
	unsigned p = load(0, 1) & PAM, z = size ? size : (load(8, 1) & SZM), o;
	if (from)
		p = from;
	for (o = 0; o < z; o += 8) {
		unsigned x = e(),
				 a = *(unsigned *)(unsigned long)(K0 | p + o),
				 b = *(unsigned *)(unsigned long)(K0 | p + o + 4);

		E(x | ERR);
		h();
		__asm__("mtc0 %0,$28,1\n\tmtc0 %1,$29,1" ::"r"(a), "r"(b));
		h();
		cop(IDATA, K0 | to + o);
		h();
		E(x);
	}
}
