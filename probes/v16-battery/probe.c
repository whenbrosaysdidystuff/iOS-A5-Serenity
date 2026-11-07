/*
 * Redmi A5 (UMS9230) hardware probe, v16: battery (SC2730 fuel gauge), read-only.
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

/*
 * V16, READ ONLY: SC2730 fuel gauge (FGU, PMIC 0xc00; Linux sc27xx_fuel_gauge.c, sc2730_info):
 * module enable 0x1808 bit 7 (FGU_EN), clock enable 0x1810 bit 6 (FGU_RTC_EN); VOLTAGE 0xc20,
 * OCV 0xc24, CURRENT 0xc2c (0x2000 = 0 mA). Without the eFuse calibration the scale is nominal:
 * ~683 ADC per volt (calibration mid value), current ADC per amp = 4 x that (equal resistors).
 * Every second: voltage and current - plug and unplug the charger to see the sign change.
 */
#define ADI_BASE	0x64200000UL
#define ADI_SLAVE	(ADI_BASE + 0x20000UL)
#define ADI_RD_CMD	0x28
#define ADI_RD_DATA	0x2c
#define MODULE_EN0	0x1808
#define CLK_EN0		0x1810
#define FGU		0xc00
#define VOL_ADC_1V	683

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

void probe_main(uint64_t dtb, uint64_t load, uint64_t el)
{
	(void)load; (void)dtb;
	cntfrq = rsr("cntfrq_el0");
	fb = (volatile uint32_t *)FB_DEFAULT;
	clear_all();
	line("A5 PROBE V16 - BATTERY (READ ONLY)", YELLOW);
	uint32_t men = rd(MODULE_EN0), cen = rd(CLK_EN0);
	label("EL "); puthex(el, 1); label(" MODEN "); puthex(men, 4); label(" CLKEN "); puthex(cen, 4);
	label(" ECHO BAD "); puthex(adi_bad, 2); nl();
	label("FGU "); puts_((men >> 7) & 1 ? "ON" : "OFF", (men >> 7) & 1 ? GREEN : RED);
	label(" RTC CLK "); puts_((cen >> 6) & 1 ? "ON" : "OFF", (cen >> 6) & 1 ? GREEN : RED); nl();
	for (uint32_t o = 0; o < 0x30; o += 0x10) {
		puthex(FGU + o, 3); putc_(' ', WHITE);
		for (uint32_t k = 0; k < 0x10; k += 4) { puthex(rd(FGU + o + k), 4); putc_(' ', WHITE); }
		nl();
	}
	label("OCV ADC "); puthex(rd(FGU + 0x24), 4); label(" ~MV "); putdec((int32_t)(rd(FGU + 0x24) * 1000 / VOL_ADC_1V)); nl();
	line("EVERY SECOND (+MA = CHARGING):", YELLOW);
	line("PLUG / UNPLUG THE CHARGER", YELLOW);
	int r0 = row;
	uint64_t t0 = now();
	for (int i = 0;; i++) {
		uint32_t v = rd(FGU + 0x20), c = rd(FGU + 0x2c);
		int32_t mv = (int32_t)(v * 1000 / VOL_ADC_1V);
		int32_t ma = ((int32_t)c - 0x2000) * 1000 / (4 * VOL_ADC_1V);
		row = r0 + (i % 20); col = 0;
		puthex((uint32_t)((now() - t0) / cntfrq), 4); label(" S  V "); puthex(v, 4); label(" ~"); putdec(mv);
		label(" MV  I "); puthex(c, 4); label(" ~"); putdec(ma); label(" MA     ");
		nl(); col = 0; puts_("                                          ", WHITE);
		delay_ms(1000);
	}
}
