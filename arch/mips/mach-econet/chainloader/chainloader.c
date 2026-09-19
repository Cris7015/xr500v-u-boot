/* SPDX-License-Identifier: GPL-2.0+ */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

enum {
	SOH = 0x01,
	STX = 0x02,
	EOT = 0x04,
	ACK = 0x06,
	NAK = 0x15,
	CAN = 0x18,
	CRC_REQ = 'C',
};

#define UART_BASE                0xbfbf0000u
#define UART_RBR                 0x00u
#define UART_THR                 0x00u
#define UART_IER                 0x04u
#define UART_LSR                 0x14u
#define UART_LSR_DR              0x01u
#define UART_LSR_THRE            0x20u
#define UART_LSR_TEMT            0x40u
#define UART_LSR_ERR             0x1eu	/* OE | PE | FE | BI */

#ifndef UBOOT_LOAD_ADDR
#define UBOOT_LOAD_ADDR          0x81000000u
#endif

/* Cached KSEG0: uncached 8-bit stores corrupt the word on this chip (see README). */
#define UBOOT_LOAD_CACHED        (UBOOT_LOAD_ADDR)
#define UBOOT_ENTRY_CACHED       (UBOOT_LOAD_ADDR)
#define UBOOT_MAX_SIZE           0x00400000u

#define CR_TIMER_CTL             0xbfbf0100u

/* Assumed console baud rate for calibrating the timebase. */
#define CONSOLE_CPS              11520u	/* 115200 8N1 = 11520 chars/s */
#define DEFAULT_TICKS_PER_MS     250000u	/* fallback: CP0 Count at 250 MHz */

/* Protocol tuning. */
#define HANDSHAKE_TRIES          200
#define HANDSHAKE_MS             3000
#define BYTE_MS                  1000
#define NEXT_HDR_MS              5000
#define CSUM_PROBE_MS            300
#define PURGE_MS                 300
#define MAX_ERRORS               16

extern u32 __image_start;
extern u32 __chk_start;

#define CHK_CHUNK                128u

static inline u32 mmio_read32(u32 addr)
{
	return *(volatile u32 *)addr;
}

static inline void mmio_write32(u32 addr, u32 val)
{
	*(volatile u32 *)addr = val;
}

/* CP0 Count: fixed-rate timebase, independent of loop speed. */
static inline u32 cp0_count(void)
{
	u32 v;

	__asm__ volatile("mfc0 %0, $9" : "=r"(v));
	return v;
}

static void watchdog_kick(void)
{
	u32 val = mmio_read32(CR_TIMER_CTL);

	val &= 0xffc0ffffu;
	val |= 0x00200000u;
	mmio_write32(CR_TIMER_CTL, val);
	__asm__ volatile("sync" ::: "memory");
}

/* Mutable state lives in .bss: .data is covered by the image self-check. */
/*
 * Delay between seeing DR and reading the RBR. Replicates exactly the cost
 * of watchdog_kick() (two MMIO transactions plus sync, ~5 us), which is
 * what the v10 probe had between the two reads.
 */
static void uart_rx_settle(void)
{
	u32 val = mmio_read32(CR_TIMER_CTL);

	val &= 0xffc0ffffu;
	val |= 0x00200000u;
	mmio_write32(CR_TIMER_CTL, val);
	__asm__ volatile("sync" ::: "memory");
}

static u32 ticks_per_ms;
static u32 tx_chars;

static void uart_putc(u8 c)
{
	while (!(mmio_read32(UART_BASE + UART_LSR) & UART_LSR_THRE))
		;
	mmio_write32(UART_BASE + UART_THR, c);
	tx_chars++;
}

static void uart_puts(const char *s)
{
	while (*s) {
		// if (*s == '\n')
		// 	uart_putc('\r');
		uart_putc((u8)*s++);
	}
}

/* Only shifts and a table: no division in the diagnostic path. */
static void put_hex32(u32 v)
{
	static const char hex[] = "0123456789abcdef";
	int shift;

	for (shift = 28; shift >= 0; shift -= 4)
		uart_putc(hex[(v >> shift) & 0xf]);
}

/*
 * Calibrate the CP0 Count against the UART shift rate: the banner has
 * already been transmitted, so the elapsed time divided by the number of
 * characters gives the period of one character, which at 115200 8N1 is
 * 1/11520 s.
 */
static void calibrate(u32 t0, u32 chars)
{
	u32 dt, per_char, per_sec;

	dt = cp0_count() - t0;

	if (!chars)
		return;
	per_char = dt / chars;
	if (per_char < 8u || per_char > 400000u)
		return;			/* implausible: stay on fallback */

	per_sec = per_char * CONSOLE_CPS;
	if (per_sec < 1000000u || per_sec > 2000000000u)
		return;

	ticks_per_ms = per_sec / 1000u;
}

static u32 rx_lsr_err;
static u32 rx_wait_n;
static int have_pushback;
static u8 pushback_byte;

static int uart_getc_to(u8 *out, u32 ms)
{
	u32 t0 = cp0_count();
	u32 limit = ms * ticks_per_ms;
	u32 last = t0;
	int waited = 0;

	if (have_pushback) {
		have_pushback = 0;
		*out = pushback_byte;
		return 1;
	}

	for (;;) {
		u32 lsr = mmio_read32(UART_BASE + UART_LSR);
		u32 now;

		rx_lsr_err |= lsr & UART_LSR_ERR;
		if (lsr & UART_LSR_DR) {
			u32 raw;

			if (waited)
				rx_wait_n++;
			/*
			 * DR going high doesn't mean the RBR is already valid on
			 * this chip. The v10 probe read correctly because it had
			 * a watchdog_kick() between the LSR and RBR reads; without
			 * it, an immediate read returns bus garbage. Same sequence
			 * here.
			 */
			uart_rx_settle();
			raw = mmio_read32(UART_BASE + UART_RBR);
			*out = (u8)raw;
			return 1;
		}

		now = cp0_count();
		if ((now - t0) >= limit) {
			watchdog_kick();
			return 0;
		}
		waited = 1;
		/* Don't kick the watchdog every loop: this is the RX loop. */
		if ((now - last) >= ticks_per_ms) {
			watchdog_kick();
			last = now;
		}
	}
}

static void uart_pushback(u8 c)
{
	pushback_byte = c;
	have_pushback = 1;
}

/* Discard everything until the line is idle for the given time. */
static void uart_purge(u32 ms)
{
	u8 c;

	have_pushback = 0;
	while (uart_getc_to(&c, ms))
		;
}

static u16 crc16_xmodem(const u8 *buf, u32 len)
{
	u16 crc = 0;
	u32 i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= (u16)buf[i] << 8;
		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x8000)
				crc = (u16)((crc << 1) ^ 0x1021);
			else
				crc = (u16)(crc << 1);
		}
		if ((i & 0x7fu) == 0)
			watchdog_kick();
	}

	return crc;
}

static u8 csum8(const u8 *buf, u32 len)
{
	u8 sum = 0;
	u32 i;

	for (i = 0; i < len; i++)
		sum = (u8)(sum + buf[i]);

	return sum;
}

static u32 crc32_ieee(const volatile u8 *buf, u32 len)
{
	u32 crc = 0xffffffffu;
	u32 i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
		if ((i & 0x3fffu) == 0)
			watchdog_kick();
	}

	return ~crc;
}

/* Deferred diagnostics: nothing can be printed while the link is live. */
static u32 err_count;
static u32 err_first_kind;	/* 1 hdr, 2 timeout, 3 trailer, 4 sequence */
static u32 err_first_blk;
static u32 err_first_exp;
static u32 err_first_len;
static u32 err_first_got;
static u32 err_first_want;
static u8 err_first_data[16];
static u32 stat_blocks;
static u32 stat_dups;
static u32 stat_tries;
static int stat_1k;
static int stat_csum;

static void note_error(u32 kind, u32 blk, u32 exp, u32 len, u32 got, u32 want)
{
	if (!err_count) {
		err_first_kind = kind;
		err_first_blk = blk;
		err_first_exp = exp;
		err_first_len = len;
		err_first_got = got;
		err_first_want = want;
	}
	err_count++;
}

/*
 * XMODEM receiver: 128-byte (SOH) and 1K (STX) blocks, CRC16/XMODEM with
 * fallback to 8-bit checksum, per-byte timeout, and purge-based resync.
 */
static u32 xmodem_receive(void)
{
	volatile u8 *dst = (volatile u8 *)UBOOT_LOAD_CACHED;
	static u8 packet[1024];
	u8 expected = 1;
	u32 total = 0;
	u8 ch;
	int cancels = 0;
	int tries;

	uart_purge(100);

	for (tries = 0;; tries++) {
		stat_tries = (u32)tries;
		if (tries >= HANDSHAKE_TRIES)
			return 0;
		/* Ask for CRC first; later also offer checksum mode. */
		uart_putc(tries < 40 ? CRC_REQ : NAK);
		if (!uart_getc_to(&ch, HANDSHAKE_MS))
			continue;
		if (ch == SOH || ch == STX || ch == EOT || ch == CAN)
			break;
	}

	for (;;) {
		u32 plen, i;
		u8 blk, blki, t1, t2;
		u16 got_crc, want_crc;
		int have_t2, ok;

		if (ch == EOT) {
			uart_putc(ACK);
			return total;
		}

		if (ch == CAN) {
			if (++cancels >= 2)
				return 0;
			if (!uart_getc_to(&ch, BYTE_MS))
				goto resync;
			continue;
		}
		cancels = 0;

		if (ch != SOH && ch != STX) {
			note_error(1, ch, expected, 0, 0, 0);
			goto resync;
		}

		plen = (ch == STX) ? 1024u : 128u;
		if (plen == 1024u)
			stat_1k = 1;

		if (!uart_getc_to(&blk, BYTE_MS) ||
		    !uart_getc_to(&blki, BYTE_MS)) {
			note_error(2, 0, expected, plen, 0, 0);
			goto resync;
		}

		for (i = 0; i < plen; i++) {
			if (!uart_getc_to(&packet[i], BYTE_MS)) {
				note_error(2, blk, expected, i, 0, 0);
				goto resync;
			}
			if ((i & 0x1fu) == 0)
				watchdog_kick();
		}

		if (!uart_getc_to(&t1, BYTE_MS)) {
			note_error(2, blk, expected, plen, 0, 0);
			goto resync;
		}

		have_t2 = uart_getc_to(&t2, CSUM_PROBE_MS);
		want_crc = crc16_xmodem(packet, plen);
		got_crc = have_t2 ? (u16)(((u16)t1 << 8) | t2) : 0;
		ok = 0;

		if (have_t2 && got_crc == want_crc) {
			ok = 1;
		} else if (t1 == csum8(packet, plen)) {
			/* Checksum mode: t2 is already the start of the next packet. */
			stat_csum = 1;
			ok = 1;
			if (have_t2)
				uart_pushback(t2);
		}

		if (!ok) {
			if (!err_count)
				for (i = 0; i < sizeof(err_first_data) && i < plen; i++)
					err_first_data[i] = packet[i];
			note_error(3, blk, expected, plen, got_crc, want_crc);
			goto resync;
		}

		if ((u8)(blk + blki) != 0xff) {
			note_error(1, blk, blki, plen, 0, 0);
			goto resync;
		}

		/* Retransmission of a block already received. */
		if (blk == (u8)(expected - 1)) {
			stat_dups++;
			uart_putc(ACK);
			if (!uart_getc_to(&ch, NEXT_HDR_MS))
				goto resync;
			continue;
		}

		if (blk != expected) {
			note_error(4, blk, expected, plen, 0, 0);
			goto resync;
		}

		if (total + plen > UBOOT_MAX_SIZE) {
			uart_putc(CAN);
			uart_putc(CAN);
			return 0;
		}

		for (i = 0; i < plen; i++)
			dst[total + i] = packet[i];
		total += plen;
		expected++;
		stat_blocks++;
		watchdog_kick();

		uart_putc(ACK);
		if (!uart_getc_to(&ch, NEXT_HDR_MS)) {
			note_error(2, 0, expected, 0, 0, 0);
			goto resync;
		}
		continue;

resync:
		if (err_count >= MAX_ERRORS) {
			uart_putc(CAN);
			uart_putc(CAN);
			uart_putc(CAN);
			uart_purge(PURGE_MS);
			return 0;
		}
		uart_purge(PURGE_MS);
		uart_putc(NAK);
		if (!uart_getc_to(&ch, NEXT_HDR_MS)) {
			note_error(2, 0, expected, 0, 0, 0);
			ch = 0;
		}
	}
}

static void put_hex8(u8 v)
{
	static const char hex8[] = "0123456789abcdef";

	uart_putc(hex8[(v >> 4) & 0xf]);
	uart_putc(hex8[v & 0xf]);
}

static u32 chunk_crc(const volatile u8 *img, u32 len, u32 i)
{
	u32 off = i * CHK_CHUNK;
	u32 n = len - off;

	if (n > CHK_CHUNK)
		n = CHK_CHUNK;
	return crc32_ieee(img + off, n);
}

/*
 * Checks the loaded image against the per-128-byte-block CRC32 table that
 * tools/econet_chainloader_image.py wrote at its end: locates the block,
 * not just flags failure.
 */
static void self_check(void)
{
	const volatile u8 *img = (const volatile u8 *)&__image_start;
	const volatile u32 *tab = (const volatile u32 *)&__chk_start;
	u32 len = (u32)&__chk_start - (u32)&__image_start;
	u32 nchunks = (len + CHK_CHUNK - 1u) / CHK_CHUNK;
	u32 i, n, off, bad = 0, first_bad = 0;

	uart_puts(" self len=0x");
	put_hex32(len);
	uart_puts(" blocks=0x");
	put_hex32(nchunks);

	for (i = 0; i < nchunks; i++) {
		if (chunk_crc(img, len, i) != tab[i]) {
			if (!bad)
				first_bad = i;
			bad++;
		}
	}

	uart_puts(" bad=0x");
	put_hex32(bad);
	if (!bad) {
		uart_puts(" OK\n");
		return;
	}

	uart_puts("\nbad idx:");
	for (i = 0; i < nchunks; i++)
		if (chunk_crc(img, len, i) != tab[i]) {
			uart_puts(" 0x");
			put_hex32(i);
		}

	off = first_bad * CHK_CHUNK;
	n = len - off;
	if (n > CHK_CHUNK)
		n = CHK_CHUNK;

	uart_puts("\ngot=0x");
	put_hex32(chunk_crc(img, len, first_bad));
	uart_puts(" want=0x");
	put_hex32(tab[first_bad]);
	uart_puts("\ndump @0x");
	put_hex32((u32)&__image_start + off);
	uart_putc('\n');
	for (i = 0; i < n; i++) {
		put_hex8(img[off + i]);
		if ((i & 0x1fu) == 0x1fu)
			uart_putc('\n');
	}
	uart_puts("corrupted image; continuing anyway\n");
}

/*
 * Writes 16 bytes with 8-bit stores and reads them back as words, once
 * through KSEG0 (cached) and once through KSEG1 (uncached), at the same
 * physical address. Expected in both: 10111213 14151617 18191a1b 1c1d1e1f.
 */
extern void chainload_jump(u32 entry) __attribute__((noreturn));

static void report(void)
{
	uart_puts("\nblocks=0x");
	put_hex32(stat_blocks);
	uart_puts(" dup=0x");
	put_hex32(stat_dups);
	uart_puts(" tries=0x");
	put_hex32(stat_tries);
	uart_puts(" mode=");
	uart_puts(stat_1k ? "1k/" : "128/");
	uart_puts(stat_csum ? "csum" : "crc16");
	uart_puts(" err=0x");
	put_hex32(err_count);
	uart_puts(" lsrerr=0x");
	put_hex32(rx_lsr_err);
	if (err_count) {
		uart_puts("\nfirst: kind=0x");
		put_hex32(err_first_kind);
		uart_puts(" blk=0x");
		put_hex32(err_first_blk);
		uart_puts(" exp=0x");
		put_hex32(err_first_exp);
		uart_puts(" at=0x");
		put_hex32(err_first_len);
		uart_puts(" got=0x");
		put_hex32(err_first_got);
		uart_puts(" want=0x");
		put_hex32(err_first_want);
		uart_puts("\ndata:");
		{
			u32 i;

			for (i = 0; i < sizeof(err_first_data); i++) {
				uart_putc(' ');
				put_hex8(err_first_data[i]);
			}
		}
		uart_puts(" waits=0x");
		put_hex32(rx_wait_n);
	}
	uart_putc('\n');
}

static void halt(void)
{
	for (;;)
		watchdog_kick();
}

void chainloader_main(void)
{
	u32 len, image_crc, t0;
	volatile u32 *w = (volatile u32 *)UBOOT_LOAD_CACHED;
	u32 i;

	watchdog_kick();

	/* The BootROM may leave the UART IRQ enabled; this runs in polling mode. */
	mmio_write32(UART_BASE + UART_IER, 0);

	ticks_per_ms = DEFAULT_TICKS_PER_MS;
	tx_chars = 0;
	t0 = cp0_count();
	uart_puts("\nEN751221 BootROM chainloader\n");
	uart_puts("XMODEM 128/1k, CRC16 ou checksum -> 0x81000000\n");
	calibrate(t0, tx_chars);

	uart_puts("ticks/ms=0x");
	put_hex32(ticks_per_ms);

	self_check();

	uart_puts("waiting\n");

	len = xmodem_receive();
	if (!len) {
		report();
		uart_puts("XMODEM failed/cancelled\n");
		halt();
	}

	__asm__ volatile("sync" ::: "memory");

	report();
	uart_puts("received 0x");
	put_hex32(len);
	uart_puts(" bytes\nfirst words: ");
	for (i = 0; i < 4; i++) {
		put_hex32(w[i]);
		if (i != 3)
			uart_putc(' ');
	}
	uart_putc('\n');

	image_crc = crc32_ieee((volatile u8 *)UBOOT_LOAD_CACHED, len);
	uart_puts("crc32: ");
	put_hex32(image_crc);
	uart_putc('\n');

	if (w[0] == 0x00000000u || w[0] == 0xffffffffu) {
		uart_puts("invalid first word; refusing jump\n");
		halt();
	}

	watchdog_kick();
	uart_puts("jump 0x81000000\n");
	__asm__ volatile("sync" ::: "memory");
	chainload_jump(UBOOT_ENTRY_CACHED);
}
