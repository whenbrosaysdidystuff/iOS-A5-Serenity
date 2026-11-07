/*
 * Redmi A5 (UMS9230) hardware probe, v17: audio DSP (AUDCP / AGDSP) - is it alive, does it answer?
 * U-Boot loads l_agdsp and boots the audio CP before Linux ("audio cp boot ok!"), so the DSP should
 * already run here. This probe:
 *   1. reads AUDCP power/boot state (PMU 0x64020000, AON 0x64000000), the image in DDR (0x88f00000),
 *      the AP<->DSP smsg ring (0x94380400, Unisoc audio-sipc.c) and both mailbox blocks;
 *   2. wakes the DSP (AON 0x14c bit 5 + mailbox msg 100, as agdsp_access_enable() does);
 *   3. sends ONE register read over the ring: AMSG_CH_VBC_CTL, SND_VBC_DSP_IO_KCTL_GET,
 *      SND_KCTL_TYPE_REG, reg = REG_VBC_VERSION (DSP view 0x01700F40), and shows the reply;
 *   4. last (may fault): AP-side VBC registers at 0x56480000.
 * Nothing is written to storage, the PMIC codec or the speaker amplifier.
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

#define ADI_BASE	0x64200000UL
#define ADI_SLAVE	(ADI_BASE + 0x20000UL)
#define ADI_RD_CMD	0x28
#define ADI_RD_DATA	0x2c

static int adi_bad;
static int adi_read(uint32_t reg, uint32_t *val)
{
	uint32_t pa = (uint32_t)(ADI_SLAVE + reg);
	wr32(ADI_BASE + ADI_RD_CMD, pa);
	for (int i = 0; i < 100000; i++) {
		uint32_t d = rd32(ADI_BASE + ADI_RD_DATA);
		if (!(d & 0x80000000u)) {
			*val = d & 0xffff;
			if (((d >> 16) & 0x7fff) != ((pa & 0x1fffc) >> 2)) { adi_bad++; return 2; }
			return 0;
		}
	}
	return 1;
}

static uint32_t rd(uint32_t reg) { uint32_t v = 0; adi_read(reg, &v); return v; }

static void putdec(int32_t v)
{
	char b[12]; int n = 0;
	if (v < 0) { putc_('-', WHITE); v = -v; }
	do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
	while (n) putc_(b[--n], WHITE);
}

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



#define PMU		0x64020000UL
#define AON		0x64000000UL
#define DSPBIN		0x88f00000UL
#define CMDPARA		0x94380000UL
#define SMSG		0x94380400UL
#define SMSG_SIZE	0xa10
#define RING_BYTES	((SMSG_SIZE - 16) / 2)		/* 0x500 = 64 msgs of 20 bytes */
#define RING_N		(RING_BYTES / 20)
#define TXBUF		SMSG
#define RXBUF		(SMSG + RING_BYTES)
#define PTRS		(SMSG + 2 * RING_BYTES)		/* txrd, txwr, rxrd, rxwr */
#define MBOX_IN		0x641c0000UL
#define MBOX_OUT	0x641d0000UL
#define VBC_AP		0x56480000UL
#define AP_GPIO		0x641b0000UL
#define AUDCP_CORE	5

static void kv(const char *k, uint64_t v, int d) { label(k); puthex(v, d); putc_(' ', WHITE); }

/* aligned 32-bit access only (MMU off): a 20-byte smsg = 5 words at a 4-byte aligned address */
static void msg_show(uintptr_t a)
{
	uint32_t w0 = rd32(a);
	label("C"); puthex(w0 & 0xffff, 4); label(" H"); puthex(w0 >> 16, 2); putc_(' ', WHITE);
	puthex(rd32(a + 4), 8); putc_(' ', WHITE); puthex(rd32(a + 8), 8); putc_(' ', WHITE);
	puthex(rd32(a + 12), 8); putc_(' ', WHITE); puthex(rd32(a + 16), 4); nl();
}

static void mbox_send(uint32_t lo, uint32_t hi)
{
	wr32(MBOX_IN + 0x4, lo);
	wr32(MBOX_IN + 0x8, hi);
	wr32(MBOX_IN + 0x0, AUDCP_CORE);
	wr32(MBOX_IN + 0xc, 1);
}

static void mbox_regs(const char *n, uintptr_t b)
{
	label(n);
	for (int o = 0; o <= 0x24; o += 4) { puthex(rd32(b + o), 8); putc_(' ', WHITE); if (o == 0xc || o == 0x1c) { nl(); puts_("    ", WHITE); } }
	nl();
}

void probe_main(uint64_t dtb, uint64_t load, uint64_t el)
{
	(void)load; (void)dtb;
	cntfrq = rsr("cntfrq_el0");
	fb = (volatile uint32_t *)FB_DEFAULT;
	clear_all();
	line("A5 PROBE V17 - AUDIO DSP", YELLOW);
	kv("EL", el, 1); nl();

	/* 1. state */
	uint32_t st = rd32(PMU + 0x544);
	kv("PMU544", st, 8); kv("SYS", (st >> 8) & 0x1f, 2); kv("CORE", (st >> 16) & 0x1f, 2); nl();
	kv("SLEEP D4", rd32(PMU + 0xd4), 8); kv("850", rd32(PMU + 0x850), 8); nl();
	kv("860", rd32(PMU + 0x860), 8); kv("3D0", rd32(PMU + 0x3d0), 8); kv("3D4", rd32(PMU + 0x3d4), 8); nl();
	kv("818", rd32(PMU + 0x818), 8); kv("B88", rd32(PMU + 0xb88), 8); kv("B98", rd32(PMU + 0xb98), 8); nl();
	kv("BA8", rd32(PMU + 0xba8), 8); kv("AON78", rd32(AON + 0x78), 8); kv("140", rd32(AON + 0x140), 8); nl();
	kv("AON144", rd32(AON + 0x144), 8); kv("14C", rd32(AON + 0x14c), 8); nl();
	label("DDR 88F00000: ");
	for (int i = 0; i < 32; i += 4) { uint32_t w = rd32(DSPBIN + i); for (int b = 0; b < 4; b++) { char c = (char)(w >> (8 * b)); putc_(c >= 0x20 && c < 0x7f ? c : '.', GREEN); } }
	nl();
	label("PARA "); for (int i = 0; i < 16; i += 4) { puthex(rd32(CMDPARA + i), 8); putc_(' ', WHITE); } nl();
	uint32_t txrd = rd32(PTRS), txwr = rd32(PTRS + 4), rxrd = rd32(PTRS + 8), rxwr = rd32(PTRS + 12);
	kv("TXRD", txrd, 8); kv("TXWR", txwr, 8); nl();
	kv("RXRD", rxrd, 8); kv("RXWR", rxwr, 8); nl();
	line("RX RING (FIRST 4):", YELLOW);
	for (int i = 0; i < 4; i++) msg_show(RXBUF + 20 * i);
	mbox_regs("IN  ", MBOX_IN);
	mbox_regs("OUT ", MBOX_OUT);
	uint32_t g3 = rd32(AP_GPIO + 3 * 0x80), d3 = rd32(AP_GPIO + 3 * 0x80 + 8);
	kv("GPIO55 ID", (g3 >> 7) & 1, 1); kv("GPIO58 PA", (g3 >> 10) & 1, 1); kv("DIR", d3, 4); nl();
	label("PMIC 1000: ");
	for (uint32_t o = 0; o < 0x20; o += 4) { puthex(rd(0x1000 + o), 4); putc_(' ', WHITE); } nl();

	/* 2. wake (agdsp_access_enable) */
	int bad = (txwr - txrd) > RING_N || (rxwr - rxrd) > RING_N || txwr > 0x100000 || rxwr > 0x100000;
	if (bad) { line("RING PTRS INSANE -> ZERO THEM (LINUX DOES TOO)", RED); for (int i = 0; i < 16; i += 4) wr32(PTRS + i, 0); txrd = txwr = 0; }
	wr32(AON + 0x14c, rd32(AON + 0x14c) | 0x20);
	mbox_send(100, 0);
	uint64_t t = now(); int ok = 0;
	while (now() - t < cntfrq / 10) { if (((rd32(PMU + 0x544) >> 8) & 0x1f) == 0) { ok = 1; break; } }
	kv("WAKE", ok, 1); kv("14C", rd32(AON + 0x14c), 8); kv("PMU544", rd32(PMU + 0x544), 8); nl();

	/* 3. one KCTL_GET (REG_VBC_VERSION) */
	wr32(CMDPARA + 0, 0x01700F40);	/* reg */
	wr32(CMDPARA + 4, 0xffffffff);	/* mask */
	wr32(CMDPARA + 8, 0x5a5a5a5a);	/* value (DSP should overwrite) */
	uint32_t wr = rd32(PTRS + 4), rx0 = rd32(PTRS + 12);
	uintptr_t m = TXBUF + 20 * (wr & (RING_N - 1));
	wr32(m + 0, 8u | 0u << 16);	/* command SND_VBC_DSP_IO_KCTL_GET, channel AMSG_CH_VBC_CTL */
	wr32(m + 4, 0);			/* id = SND_KCTL_TYPE_REG */
	wr32(m + 8, 0xffffffff);	/* stream = -1 */
	wr32(m + 12, (uint32_t)CMDPARA);
	wr32(m + 16, 0);
	__asm__ volatile("dsb sy");
	wr32(PTRS + 4, wr + 1);
	__asm__ volatile("dsb sy");
	mbox_send(0, 8u << 16 | 0u);
	line("SENT KCTL_GET VBC VERSION, WAITING 2 S", YELLOW);
	t = now(); int got = 0;
	while (now() - t < 2 * cntfrq) { if (rd32(PTRS + 12) != rx0) { got = 1; break; } }
	delay_ms(50);
	kv("REPLY", got, 1); kv("MS", (now() - t) * 1000 / cntfrq, 4); nl();
	kv("TXRD", rd32(PTRS), 8); kv("TXWR", rd32(PTRS + 4), 8); kv("RXWR", rd32(PTRS + 12), 8); nl();
	for (uint32_t i = rx0; i != rd32(PTRS + 12) && i - rx0 < 6; i++) msg_show(RXBUF + 20 * (i & (RING_N - 1)));
	label("PARA "); for (int i = 0; i < 12; i += 4) { puthex(rd32(CMDPARA + i), 8); putc_(' ', WHITE); } nl();
	kv("VBC VERSION", rd32(CMDPARA + 8), 8); nl();
	mbox_regs("OUT ", MBOX_OUT);

	/* 4. AP side VBC (may fault if the AGCP domain is off) */
	line("VBC AP 56480000 (MAY FAULT):", YELLOW);
	for (uint32_t o = 0x20; o < 0x70; o += 0x10) {
		puthex(o, 2); putc_(' ', WHITE);
		for (uint32_t k = 0; k < 0x10; k += 4) { puthex(rd32(VBC_AP + o + k), 8); putc_(' ', WHITE); }
		nl();
	}
	line("DONE", GREEN);
	for (;;) __asm__ volatile("wfe");
}
