/*
 * Redmi A5 (UMS9230) hardware probe, v15: FocalTech FT8057M touch over SPI3 (firmware to touch-IC RAM only).
 * v5 showed: CMD13 fine (card in transfer state) but every single-block read ends with a
 * data CRC error at every POSRD delay. v6 answers: does the data land in RAM anyway, and does
 * the NEGRD delay (bits 31:24 of DLL_DLY) and/or the DLL compensation (DLL_CFG bits 27:24) matter?
 * Read-only toward storage: only CMD13 and CMD17 are ever issued. MMU off: aligned accesses only.
 * Build with -DHOST_TEST to run against a software model.
 */
#include <stdint.h>

#ifdef HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#define rsr(r) host_rsr(#r)
static uint64_t host_rsr(const char *r);
static uint32_t rd32(uintptr_t a);
static uint16_t rd16(uintptr_t a);
static void wr32(uintptr_t a, uint32_t v);
static void wr16(uintptr_t a, uint16_t v);
static void wr8(uintptr_t a, uint8_t v);
#else
#include "font.h"
#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint16_t rd16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
#endif
static inline uint64_t rd64(uintptr_t a) { return (uint64_t)rd32(a) | (uint64_t)rd32(a + 4) << 32; }

#ifndef FB_DEFAULT
#define FB_DEFAULT	0x9CF2A000UL
#endif
#define FB_W		720
#define FB_H		1640
#define FRAME_BYTES	(FB_W * FB_H * 4UL)
#define SCALE		2
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		8
#define COLS		((FB_W - 2 * MARGIN) / CH_W)
#define ROWS		((FB_H - 2 * MARGIN) / CH_H)
#define BLACK		0xff000000U
#define WHITE		0xffffffffU
#define YELLOW		0xffffff00U
#define RED		0xffff0000U
#define GREEN		0xff00ff00U

#ifndef SDH
#define SDH		0x201d0000UL	/* sdio@201d0000 = sdio_emmc */
#endif

static int row, col;

/* ---- text console ---- */
#ifdef HOST_TEST
static char screen[64][64];
static void nl(void) { col = 0; row++; }
static void putc_(char ch, uint32_t fg)
{
	(void)fg;
	if (col >= COLS) nl();
	if (row < 64) screen[row][col] = ch;
	col++;
}
static void clear_all(void) { row = col = 0; for (int r = 0; r < 64; r++) for (int c = 0; c < 64; c++) screen[r][c] = ' '; }
static void dump_screen(void)
{
	for (int r = 0; r < 64 && r <= row; r++) {
		int e = 63; while (e >= 0 && screen[r][e] == ' ') e--;
		printf("%.*s\n", e + 1, screen[r]);
	}
}
#else
static volatile uint32_t *fb;
static void glyph(int c, int r, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + c * CH_W, y0 = MARGIN + r * CH_H;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t px = (g[gy] & (0x80 >> gx)) ? fg : BLACK;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = fb + (y0 + gy * SCALE + sy) * FB_W + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = px;
			}
		}
}
static void nl(void) { col = 0; row++; }
static void putc_(char ch, uint32_t fg)
{
	if (col >= COLS) nl();
	if (row >= ROWS) return;
	glyph(col, row, ch, fg);
	col++;
}
static void clear_all(void)
{
	for (unsigned long i = 0; i < FRAME_BYTES / 4; i++)
		fb[i] = BLACK;
	row = col = 0;
}
#endif

static void puts_(const char *s, uint32_t fg) { while (*s) putc_(*s++, fg); }
static void puthex(uint64_t v, int digits)
{
	static const char hx[] = "0123456789ABCDEF";
	for (int i = digits - 1; i >= 0; i--)
		putc_(hx[(v >> (4 * i)) & 0xf], WHITE);
}
static void label(const char *s) { puts_(s, YELLOW); }
static void line(const char *s, uint32_t fg) { puts_(s, fg); nl(); }
static void pad_to(int c) { while (col < c) putc_(' ', WHITE); }

/* ---- timing ---- */
#ifdef HOST_TEST
static uint64_t host_ticks;
static uint64_t now(void) { return host_ticks += 1000; }
#else
static uint64_t now(void) { __asm__ volatile("isb"); return rsr("cntpct_el0"); }
#endif
static uint64_t cntfrq;

/* ---- helpers ---- */
static int sstarts(const char *s, const char *p) { while (*p && *s == *p) { s++; p++; } return !*p; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const volatile uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const volatile uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void delay_ms(unsigned ms) { uint64_t end = now() + cntfrq / 1000 * ms; while (now() < end) ; }
static void delay_us(unsigned us) { uint64_t end = now() + cntfrq / 1000000 * us + 1; while (now() < end) ; }

/*
 * V15 (from V14 on the phone: panel Panel_C3Z_35_03_0c_vid = ft8057_boe, SPI3 gated, reset GPIO 145
 * output high, IRQ GPIO 144 input):
 *  1. SPI3 on: apapb gate set register 0x20001000 bit 8 (spi3-eb), clock 0x20010058 = 26 MHz / 1,
 *     controller set up as Linux spi-sprd.c (mode 0, 8-bit, CLKD 1 -> 6.5 MHz, PIO, 32-byte FIFO)
 *  2. touch reset (GPIO 145 low 1 ms -> high), 0x55 = enter ROM boot, 0x90 = read boot ID
 *  3. firmware (focaltech_ft8057_spi_ts.ko boe_fw) -> PRAM (0xAD addr, 0xAE data), ROM ECC check
 *     (0xCC/0xCE/0xCD), DRAM (0xD00000) likewise, 0x08 = start. Touch IC RAM only: no flash commands.
 *  4. chip ID 0xA3/0x9F, FW version 0xA6, then touch points from 0x01 shown and drawn.
 * Packets as focaltech_spi_v2.c: write [cmd,0x00,lenH,lenL, 3 dummy, data]; read
 * [cmd,0xA0,lenH,lenL, 3 dummy] + data + CRC16 (poly 0x8408), status byte rx[3] & 0xA0 == 0.
 */
#define APB_EB		0x20000000UL
#define APB_EB_SET	0x20001000UL
#define AP_CLK_SPI3	0x20010058UL
#define SPI		0x20150000UL
#define GPIO_BANK9	(0x641b0000UL + 9 * 0x80)
#define G_DATA		0x00
#define TP_IRQ_BIT	1u		/* GPIO 144 */
#define TP_RST_BIT	2u		/* GPIO 145 */

#define SPI_TXD		0x00
#define SPI_CLKD	0x04
#define SPI_CTL0	0x08
#define SPI_CTL1	0x0c
#define SPI_CTL3	0x14
#define SPI_CTL5	0x1c
#define SPI_INT_CLR	0x24
#define SPI_INT_RAW	0x28
#define SPI_STS2	0x34
#define SPI_CTL6	0x40
#define SPI_FIFO_RST	0x48
#define SPI_CTL7	0x4c
#define SPI_CTL8	0x54
#define SPI_CTL9	0x58
#define SPI_CTL10	0x5c
#define SPI_CTL11	0x60
#define RX_END		(1u << 9)
#define TX_END		(1u << 8)

extern const uint8_t fw_data[], fw_end[];

static inline void wr8b(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint8_t rd8b(uintptr_t a) { return *(volatile uint8_t *)a; }

static uint32_t spi_err;

static void spi_init(void)
{
	wr32(APB_EB_SET, 1u << 8);			/* spi3-eb */
	wr32(AP_CLK_SPI3, 0);				/* 26 MHz, divider 1 */
	delay_us(10);
	uint32_t v = rd32(SPI + SPI_CTL0);
	v &= ~((1u << 13) | 3u);			/* mode 0: no SCK_REV, ... */
	v |= 1u << 1;					/* NG_TX (CPHA 0) */
	v &= ~(0x1fu << 2);
	v |= 8u << 2;					/* 8-bit words */
	v |= 0xfu << 8;					/* all chip selects released */
	wr32(SPI + SPI_CTL0, v);
	wr32(SPI + SPI_CTL5, 9);			/* frame interval (ITVL_NUM) */
	wr32(SPI + SPI_FIFO_RST, 1);
	delay_us(1);
	wr32(SPI + SPI_FIFO_RST, 0);
	v = rd32(SPI + SPI_CTL7);
	v &= ~((7u << 3) | (1u << 15));			/* 4-wire, single data line */
	wr32(SPI + SPI_CTL7, v);
	wr32(SPI + SPI_INT_CLR, 0x33f);
	wr32(SPI + SPI_CTL6, (2u << 8) | 8);
	wr32(SPI + SPI_CTL3, (2u << 8) | 8);
	wr32(SPI + SPI_CLKD, 1);			/* 26 MHz / (2 * 2) = 6.5 MHz */
	v = rd32(SPI + SPI_CTL1);
	v &= ~((3u << 12) | (3u << 14));
	v |= (3u << 12) | (3u << 14);			/* RX + TX, data line keeps last bit */
	wr32(SPI + SPI_CTL1, v);
}

static void cs(int assert)
{
	uint32_t v = rd32(SPI + SPI_CTL0);
	if (assert) v &= ~(1u << 8); else v |= 0xfu << 8;
	wr32(SPI + SPI_CTL0, v);
}

/* full duplex, CS held for the whole buffer */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	cs(1);
	delay_us(1);
	for (uint32_t done = 0; done < len;) {
		uint32_t n = len - done > 32 ? 32 : len - done;
		wr32(SPI + SPI_CTL8, 0);
		wr32(SPI + SPI_CTL9, n);
		wr32(SPI + SPI_CTL10, 0);
		wr32(SPI + SPI_CTL11, n);
		for (uint32_t i = 0; i < n; i++)
			wr8b(SPI + SPI_TXD, tx[done + i]);
		uint64_t end = now() + cntfrq / 100;		/* 10 ms */
		while (!(rd32(SPI + SPI_INT_RAW) & RX_END))
			if (now() > end) { cs(0); spi_err++; return -1; }
		wr32(SPI + SPI_INT_CLR, TX_END | RX_END);
		for (uint32_t i = 0; i < n; i++)
			rx[done + i] = rd8b(SPI + SPI_TXD);
		done += n;
	}
	delay_us(1);
	cs(0);
	delay_us(150);					/* CS_HIGH_DELAY */
	return 0;
}

static uint8_t txb[33 * 1024], rxb[33 * 1024];
static uint8_t last_status;

static uint16_t crc8408(const uint8_t *d, uint32_t n)
{
	uint16_t c = 0xffff;
	for (uint32_t i = 0; i < n; i++) {
		c ^= d[i];
		for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
	}
	return c;
}

/* fts_write: buf[0] = command, rest = data */
static int fts_write(const uint8_t *buf, uint32_t len)
{
	uint32_t dl = len - 1, t = 0;
	txb[t++] = buf[0]; txb[t++] = 0x00; txb[t++] = dl >> 8; txb[t++] = dl;
	if (dl) { txb[t++] = 0; txb[t++] = 0; txb[t++] = 0; for (uint32_t i = 0; i < dl; i++) txb[t++] = buf[1 + i]; }
	for (int r = 0; r < 3; r++) {
		if (spi_xfer(txb, rxb, t) == 0 && !(rxb[3] & 0xa0)) return 0;
		last_status = rxb[3];
		delay_us(150);
	}
	return -1;
}

static int fts_read(uint8_t cmd, uint8_t *out, uint32_t n)
{
	uint32_t t = 0;
	txb[t++] = cmd; txb[t++] = 0xa0; txb[t++] = n >> 8; txb[t++] = n;
	txb[t++] = 0; txb[t++] = 0; txb[t++] = 0;
	uint32_t dp = t;
	t += n + 2;
	for (uint32_t i = dp; i < t; i++) txb[i] = 0;
	for (int r = 0; r < 3; r++) {
		if (spi_xfer(txb, rxb, t) == 0 && !(rxb[3] & 0xa0)) {
			uint16_t c = crc8408(rxb + dp, n), got = rxb[dp + n] | rxb[dp + n + 1] << 8;
			if (c == got) { for (uint32_t i = 0; i < n; i++) out[i] = rxb[dp + i]; return 0; }
			last_status = 0xcc;
		} else last_status = rxb[3];
		delay_us(150);
	}
	return -1;
}

static void tp_reset(void)
{
	uint32_t d = rd32(GPIO_BANK9 + G_DATA);
	wr32(GPIO_BANK9 + G_DATA, d & ~TP_RST_BIT);
	delay_ms(1);
	wr32(GPIO_BANK9 + G_DATA, d | TP_RST_BIT);
}

static uint8_t boot_id[2];

static int enter_boot(void)
{
	for (int i = 0; i < 30; i++) {
		tp_reset();
		delay_ms(8 + i);
		for (int j = 0; j < 3; j++) {
			uint8_t c = 0x55;
			if (fts_write(&c, 1) == 0) {
				delay_ms(8);
				if (fts_read(0x90, boot_id, 2) == 0 && boot_id[0] == 0x80 &&
				    (boot_id[1] == 0x57 || boot_id[1] == 0xc7))
					return i;
			}
		}
	}
	return -1;
}

static uint16_t ecc_host(const uint8_t *d, uint32_t n)
{
	uint16_t e = 0;
	for (uint32_t i = 0; i < n; i += 2) {
		e ^= (uint16_t)(d[i] << 8 | d[i + 1]);
		for (int j = 0; j < 16; j++) e = (e & 1) ? (e >> 1) ^ 0x8408 : e >> 1;
	}
	return e;
}

static int ecc_tp(uint32_t addr, uint32_t len, uint16_t *out)
{
	uint8_t c[7] = { 0xcc, addr >> 16, addr >> 8, addr, len >> 16, len >> 8, len };
	if (fts_write(c, 7)) return -1;
	delay_ms(2);
	uint8_t v[2] = { 0, 0 };
	int i;
	for (i = 0; i < 100; i++) {
		if (fts_read(0xce, v, 1)) return -2;
		if (v[0] == 0xa5) break;
		delay_ms(1);
	}
	if (i == 100) return -3;
	if (fts_read(0xcd, v, 2)) return -4;
	*out = (uint16_t)(v[0] << 8 | v[1]);
	return 0;
}

/* PRAM (base 0) or DRAM (base 0xD00000) write in 32752-byte packets, then ECC compare */
static int ram_write(uint32_t base, const uint8_t *d, uint32_t len, uint16_t *eh, uint16_t *et)
{
	static uint8_t pkt[1 + 32752];
	for (uint32_t off = 0; off < len; off += 32752) {
		uint32_t n = len - off > 32752 ? 32752 : len - off, a = base + off;
		uint8_t sa[4] = { 0xad, a >> 16, a >> 8, a };
		if (fts_write(sa, 4)) return -1;
		pkt[0] = 0xae;
		for (uint32_t i = 0; i < n; i++) pkt[1 + i] = d[off + i];
		if (fts_write(pkt, 1 + n)) return -2;
	}
	*eh = ecc_host(d, len);
	int r = ecc_tp(base, len, et);
	if (r) return -10 + r;
	return *eh == *et ? 0 : -20;
}

/* stubs for the shared start.S */
#ifndef HOST_TEST
void irq_handler(void) { }
void secondary_main(uint64_t a, uint64_t b) { (void)a; (void)b; for (;;) __asm__ volatile("wfe"); }
void exc_report(uint64_t slot)
{
	uint64_t el = rsr("CurrentEL") >> 2;
	nl(); label("EXCEPTION SLOT "); puthex(slot, 2); nl();
	label("ESR "); puthex(el == 2 ? rsr("esr_el2") : rsr("esr_el1"), 16); nl();
	label("ELR "); puthex(el == 2 ? rsr("elr_el2") : rsr("elr_el1"), 16); nl();
	label("FAR "); puthex(el == 2 ? rsr("far_el2") : rsr("far_el1"), 16); nl();
}
#endif

static void dot(int x, int y, uint32_t c)
{
	for (int dy = -6; dy <= 6; dy++)
		for (int dx = -6; dx <= 6; dx++) {
			int px = x + dx, py = y + dy;
			if (px >= 0 && px < FB_W && py >= 0 && py < FB_H && dx * dx + dy * dy <= 36)
				fb[py * FB_W + px] = c;
		}
}

void probe_main(uint64_t dtb, uint64_t load, uint64_t el)
{
	(void)load; (void)dtb;
	cntfrq = rsr("cntfrq_el0");
	fb = (volatile uint32_t *)FB_DEFAULT;
	clear_all();
	line("A5 PROBE V15 - FT8057M TOUCH", YELLOW);
	uint32_t fwlen = (uint32_t)(fw_end - fw_data);
	uint32_t pram = (uint32_t)(fw_data[0x100] << 8 | fw_data[0x101]) * 2;
	uint32_t dram = (uint32_t)(fw_data[0x108] << 8 | fw_data[0x109]) * 2;
	int hdr_ok = ((fw_data[0x100] << 8 | fw_data[0x101]) + (fw_data[0x102] << 8 | fw_data[0x103])) == 0xffff &&
		     ((fw_data[0x108] << 8 | fw_data[0x109]) + (fw_data[0x10a] << 8 | fw_data[0x10b])) == 0xffff &&
		     pram + dram <= fwlen;
	label("EL "); puthex(el, 1); label(" FW "); puthex(fwlen, 5); label(" PRAM "); puthex(pram, 5);
	label(" DRAM "); puthex(dram, 4); puts_(hdr_ok ? " OK" : " BAD", hdr_ok ? GREEN : RED); nl();
	if (!hdr_ok) goto stop;

	spi_init();
	label("SPI3 EB "); puthex((rd32(APB_EB) >> 8) & 1, 1); label(" CTL0 "); puthex(rd32(SPI + SPI_CTL0), 8);
	label(" CTL1 "); puthex(rd32(SPI + SPI_CTL1), 8); nl();
	label("GPIO DATA "); puthex(rd32(GPIO_BANK9 + G_DATA) & 3, 1); nl();

	int tries = enter_boot();
	label("BOOT ID "); puthex(boot_id[0], 2); putc_(' ', WHITE); puthex(boot_id[1], 2);
	label(" TRY "); puthex((uint32_t)tries, 2); label(" ST "); puthex(last_status, 2);
	label(" SPIERR "); puthex(spi_err, 2); nl();
	if (tries < 0) { line("NO ROM BOOT ANSWER - STOPPED", RED); goto stop; }
	line("ROM BOOT OK", GREEN);

	uint16_t eh = 0, et = 0;
	uint64_t t0 = now();
	int r = ram_write(0, fw_data, pram, &eh, &et);
	label("PRAM RC "); puthex((uint32_t)r, 8); label(" ECC "); puthex(eh, 4); putc_('/', WHITE); puthex(et, 4);
	label(" MS "); puthex((uint32_t)((now() - t0) * 1000 / cntfrq), 4); nl();
	if (r) { line("PRAM FAILED - STOPPED", RED); goto stop; }
	r = ram_write(0xd00000, fw_data + pram, dram, &eh, &et);
	label("DRAM RC "); puthex((uint32_t)r, 8); label(" ECC "); puthex(eh, 4); putc_('/', WHITE); puthex(et, 4); nl();
	if (r) { line("DRAM FAILED - STOPPED", RED); goto stop; }
	uint8_t st = 0x08;
	r = fts_write(&st, 1);
	delay_ms(200);
	uint8_t id = 0, id2 = 0, ver = 0;
	int a = fts_read(0xa3, &id, 1), b = fts_read(0x9f, &id2, 1), c = fts_read(0xa6, &ver, 1);
	label("START "); puthex((uint32_t)r, 2); label(" CHIP "); puthex(id, 2); puthex(id2, 2);
	label(" FW "); puthex(ver, 2); label(" RC "); puthex((uint32_t)(a | b | c) & 0xff, 2); nl();
	line("TOUCH THE SCREEN:", GREEN);

	int r0 = row;
	uint32_t reads = 0, errs = 0;
	for (;;) {
		uint8_t buf[84];
		int irq_low = !(rd32(GPIO_BANK9 + G_DATA) & TP_IRQ_BIT);
		if (fts_read(0x01, buf, sizeof(buf)) == 0) {
			reads++;
			int n = buf[1] & 0x0f;
			row = r0; col = 0;
			label("N "); puthex(n, 1); label(" IRQ "); puthex(irq_low, 1); label(" EV "); puthex(buf[1] >> 4, 1);
			label(" RD "); puthex(reads, 6); label(" ER "); puthex(errs, 4); puts_("   ", WHITE); nl();
			for (int i = 0; i < 2; i++) {
				const uint8_t *p = buf + 2 + 6 * i;
				int pid = p[2] >> 4, flag = p[0] >> 6;
				int x = ((p[0] & 0x0f) << 8) | p[1], y = ((p[2] & 0x0f) << 8) | p[3];
				col = 0;
				label("P"); puthex(i, 1); label(" ID "); puthex(pid, 1); label(" F "); puthex(flag, 1);
				label(" X "); puthex(x, 3); label(" Y "); puthex(y, 3); label(" Z "); puthex(p[4], 2);
				puts_("   ", WHITE); nl();
				if (pid < 10 && flag != 1 && n) dot(x, y, i ? 0xff00ffffU : 0xffff8000U);
			}
		} else {
			errs++;
		}
		delay_ms(10);
	}
stop:
	for (;;)
		delay_ms(1000);
}
