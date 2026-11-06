/*
 * Redmi A5 (UMS9230) hardware probe, v14: touch identification (read-only).
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

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
static void delay_ms(unsigned ms) { uint64_t end = now() + cntfrq / 1000 * ms; while (now() < end) ; }

/*
 * V14: which touch controller is on this A5, and is SPI3 alive? READ ONLY.
 *  - bootargs tokens that name the panel / touch IC (lcd_name=..., etc.): the A5's dtbo lists
 *    three touch drivers (Novatek NVT-ts-spi, OmniVision tcm-spi, FocalTech fts) for the
 *    different TDDI panels Xiaomi ships; the panel name decides which one is fitted
 *  - SPI3 (touch bus, 0x20150000): bus clock gate apapb 0x20000000 bit 8 (spi3-eb), bit 21
 *    (spi3-lfin-eb); its clock 0x20010058 (mux bits 1:0: 26M/128M/153.6M/192M, div bits 10:8);
 *    controller registers only if its gate is open (a gated block can hang the bus)
 *  - touch IRQ GPIO 144 / reset GPIO 145 (ap_gpio bank 9 bits 0/1)
 */
#define APB_EB		0x20000000UL
#define AP_CLK_SPI3	0x20010058UL
#define SPI3		0x20150000UL
#define GPIO_BANK9	(0x641b0000UL + 9 * 0x80)

static int interesting(const char *t)
{
	static const char keys[9][8] = { "lcd", "tp", "touch", "panel", "hw", "board", "project", "prj", "sku" };	/* no pointers: position independent */
	for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
		for (const char *s = t; *s && *s != ' ' && *s != '\n'; s++)
			if (sstarts(s, keys[k])) return 1;
	return 0;
}

/* print every bootargs token that mentions the panel / touch / hardware */
static int show_bootargs(uint64_t dtb)
{
	const volatile uint8_t *h = (const volatile uint8_t *)(uintptr_t)dtb;
	if (!dtb || be32(h) != 0xd00dfeed) return 0;
	const volatile uint8_t *p = h + be32(h + 8);
	const char *strs = (const char *)(h + be32(h + 12));
	const char *names[12];
	int depth = 0, shown = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) { const char *n = (const char *)p; names[depth < 12 ? depth : 11] = n; depth++; p += (slen(n) + 1 + 3) & ~3; }
		else if (tok == 2) depth--;
		else if (tok == 3) {
			uint32_t l = be32(p), off = be32(p + 4);
			const char *b = (const char *)(p + 8);
			p += 8 + ((l + 3) & ~3u);
			if (depth == 2 && seq(names[1], "chosen") && seq(strs + off, "bootargs")) {
				label("BOOTARGS LEN "); puthex(l, 4); nl();
				for (uint32_t i = 0; i < l && b[i]; i++) {
					if ((i == 0 || b[i - 1] == ' ' || b[i - 1] == '\n') && b[i] != ' ' && interesting(b + i)) {
						for (uint32_t j = i; j < l && b[j] && b[j] != ' ' && b[j] != '\n'; j++)
							putc_(b[j], GREEN);
						nl();
						shown++;
					}
				}
				return shown;
			}
		} else if (tok == 4) continue;
		else return -1;
	}
}

static uint64_t lcd_base(uint64_t dtb)
{
	const volatile uint8_t *h = (const volatile uint8_t *)(uintptr_t)dtb;
	if (!dtb || be32(h) != 0xd00dfeed) return 0;
	const volatile uint8_t *p = h + be32(h + 8);
	const char *strs = (const char *)(h + be32(h + 12));
	const char *names[12];
	int depth = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) { const char *n = (const char *)p; names[depth < 12 ? depth : 11] = n; depth++; p += (slen(n) + 1 + 3) & ~3; }
		else if (tok == 2) depth--;
		else if (tok == 3) {
			uint32_t l = be32(p), off = be32(p + 4);
			const char *b = (const char *)(p + 8);
			p += 8 + ((l + 3) & ~3u);
			if (depth == 2 && seq(names[1], "chosen") && seq(strs + off, "bootargs")) {
				for (uint32_t i = 0; i + 9 < l && b[i]; i++)
					if ((i == 0 || b[i - 1] == ' ' || b[i - 1] == '\n') && sstarts(b + i, "lcd_base=")) {
						uint64_t v = 0;
						for (const char *s = b + i + 9; ; s++) {
							int d;
							if (*s >= '0' && *s <= '9') d = *s - '0';
							else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
							else break;
							v = v << 4 | d;
						}
						return v;
					}
				return 0;
			}
		} else if (tok == 4) continue;
		else return 0;
	}
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

void probe_main(uint64_t dtb, uint64_t load, uint64_t el)
{
	(void)load;
	cntfrq = rsr("cntfrq_el0");
	uint64_t lcd = lcd_base(dtb);
#ifndef HOST_TEST
	fb = (volatile uint32_t *)(uintptr_t)((lcd >= 0x80000000UL && lcd < 0x140000000UL) ? lcd : FB_DEFAULT);
#else
	(void)lcd;
#endif
	clear_all();
	line("A5 PROBE V14 - TOUCH ID (READ ONLY)", YELLOW);
	label("EL "); puthex(el, 1); label(" DTB "); puthex(dtb, 10); nl();
	int n = show_bootargs(dtb);
	if (n <= 0) { label("NO PANEL TOKENS / DTB "); puthex((uint32_t)n, 8); nl(); }

	uint32_t eb = rd32(APB_EB);
	uint32_t ck = rd32(AP_CLK_SPI3);
	label("APB_EB "); puthex(eb, 8); label(" SPI3 "); puts_((eb >> 8) & 1 ? "ON" : "OFF", (eb >> 8) & 1 ? GREEN : RED);
	label(" LFIN "); puthex((eb >> 21) & 1, 1); nl();
	label("SPI3 CLK "); puthex(ck, 8); label(" SRC "); puthex(ck & 3, 1); label(" DIV "); puthex((ck >> 8) & 7, 1); nl();
	if ((eb >> 8) & 1) {
		for (uint32_t o = 0; o <= 0x74; o += 0x10) {
			puthex(o, 2); putc_(' ', WHITE);
			for (uint32_t k = 0; k < 0x10 && o + k <= 0x74; k += 4) { puthex(rd32(SPI3 + o + k), 8); putc_(' ', WHITE); }
			nl();
		}
	} else {
		line("SPI3 GATED - REGISTERS NOT READ", YELLOW);
	}
	uint32_t g[6] = { rd32(GPIO_BANK9 + 0x0), rd32(GPIO_BANK9 + 0x4), rd32(GPIO_BANK9 + 0x8),
			  rd32(GPIO_BANK9 + 0x28), rd32(GPIO_BANK9 + 0x14), rd32(GPIO_BANK9 + 0x18) };
	label("GPIO144/145 DATA "); puthex(g[0] & 3, 1); label(" DMSK "); puthex(g[1] & 3, 1);
	label(" DIR "); puthex(g[2] & 3, 1); label(" INEN "); puthex(g[3] & 3, 1); nl();
	label("IEV "); puthex(g[4] & 3, 1); label(" IE "); puthex(g[5] & 3, 1); nl();
	line("DONE - NOTHING WRITTEN", GREEN);
	for (;;)
		delay_ms(1000);
}
