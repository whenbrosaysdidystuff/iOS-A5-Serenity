/*
 * v24: the speaker amp is a FourSemi FS1588 on I2C6 (0x20220000) addr 0x34 (dts fs15xx@34, Xiaomi
 * snd-soc-fs15xx.ko), GPIO 58 = its MOD pin. It needs its register table from odm/firmware/frsm-spk1.bin
 * (embedded at build time from the private folder, NOT in git), sequence = FourSemi frsm_i2ca v5.1.2.1
 * (GPL, OPPO ums9230 tree): i2c reset (10h=3, wait 0Eh&3==3), init table (scene 0), mute (11h=0),
 * power down (10h=1), 1Fh=5A5A; scene MUSIC (table of scene 1); start: 10h=0; unmute: 11h=00C0.
 * Stock route (odm/etc/audio_route.xml "speaker"): SPKL Mixer DACLSPKL + Speaker Function = SC2730
 * speaker PA (needs CLK_PA_32K ANA_CLK0 b2, DIG_CLK_PA ANA_DCL1 b0, DIG_CLK_FGU b2), class AB, PA_G 6.
 * (older) v23: V22 silent in all 4 stages (SC2730 SPK out class AB/D: STS11 PA_AB_DPOP_DVLD never set).
 * V23: (a) checks whether GPIO 58 actually reaches its pad (input enabled, drive 0/1, read the level),
 * reads the amp ID pin GPIO 55; (b) tone stages: E1 earpiece output (ANA_CDC9 bit0, ANA_CDC10 bit0)
 * + amp, E2 FS15xx-style one-wire start (300 us high, 20 us pulses), E3 2 us pulses (14c10 hook).
 * (older) v22: V21 with headphones plugged in: the DSP tone WAS heard in the headphones, the speaker stayed
 * silent (also during both buzz stages). So DSP -> VBC -> IIS0 -> digital codec -> SC2730 DAC -> HP works.
 * V22 keeps the tone running for 4 stages and changes only the speaker side:
 *   S1 amp GPIO 58 pulses (mode 4) + HP path (= V21)
 *   S2 + SC2730 speaker path, internal PA class AB (DACS switch, SDAPA, DAS DC offset, PA_EN)
 *   S3   same, class D (PA_D_EN)
 *   S4   amp GPIO 58 held high without pulses
 * Also fixes the HP volume register (ANA_CDC7 = 0x1090; V20/V21 wrote 0x1094 = CDC8, the SPK gain).
 * (older) v21: V20 run 3 got every DSP command accepted (STARTUP/HW_PARAMS/SRC/IIS = 0), the DSP turned on
 * MCDT_EB + VBC_EB itself (EB 0x00C95000) and drained the prefill, but the PIO feed ran ~1000x too
 * fast (avail math wrong) and nothing was heard. V21 splits the chain into audible stages:
 *   BUZZ 1: amp on, toggle the SC2730 HPL/HPR enables at 200 Hz (tests codec HP out -> amp -> speaker)
 *   BUZZ 2: toggle the amp enable GPIO 58 itself at 200 Hz
 *   TONE  : DSP path, fed by time (48 frames per ms), raw MCDT pointers shown.
 * (older) v20 BEEP (run 3: built with -mstrict-align; run 2 hit an alignment fault in sp32 with the MMU off): plays a 1 kHz tone on the speaker. Path (after Otto Pflueger's mainline UMS9230 drivers:
 * sprd-agdsp.c, vbc-v4-dsp.c, sprd-mcdt.c, ums9230-digital.c, codecs/sc2730.c, reeder-s19mps.dts):
 *   AP writes S16 stereo frames (one 32-bit word each) into MCDT DAC channel 4 (0x56490000+0x10)
 *   -> DSP FE "FAST_PLAYBACK" (id 3) -> VBC TX0 -> IIS0 (24 bit, frame inverted) -> digital codec
 *   0x56750000 -> SC2730 analog codec DAC -> HP buffer/HPL -> external amp (GPIO 58, Xiaomi dtbo
 *   sprd,spk-ext-pa-info mode 4 = 3 pulses then high) -> speaker.
 * DSP commands (ring at 0x94380400): STARTUP(4), HW_PARAMS(6), KCTL_SET(9) SRC / IIS TX width+lrmod,
 * HW_TRIGGER(7). If the DSP does not drain MCDT (it "swallows one scene start per boot" on ums512
 * per the Armbian RG Rotate notes) the scene is shut down and started again once.
 *
 * (older) v19: V18 loaded the firmware at the wrong place. U-Boot's own audcp boot (uboot_a.bin code at +0x2298,
 * called from +0x2450 with w0 = 0x89000000) checks "SharkL5_AUDCP" at 0x89000000, clears IRAM
 * 0x65004400/4c00/7400/8400/9400 + 0x94380000 (0xe10), then: b98|=1<<22, b88|=1 (core reset is BIT 0),
 * ba8&=~(1<<22), 3d0&=~0x3000000, 818&=~8, 3d4&=~0x3000000, AON78 = 0x9620, AON140 = (0x89000000+0x80)>>1,
 * AON144|=1, release b98 bit22 and b88 bit0, ba8&=~(1<<22). V19:
 *   A. state + header at 0x89000000;  B. mailbox clock, wake (msg 100), and if the DSP comes up, one
 *   register read;  C. otherwise boot it exactly like U-Boot (firmware copied to 0x89000000 only if the
 *   header is missing), wait for AGDSP_BOOT_OK (0xbeee) in the rx ring, then the register read.
 *
 * (older) v18 = v17 + boot the audio CP ourselves. V17 showed AUDCP powered off (PMU 0x544 sys/core = 7),
 * DDR 0x88f00000 empty and the mailbox gated (abort at 0x641c0000). V18:
 *   a. enable the mailbox clock (DT sprd,mailbox_clk = AON 0x04 bit 2);
 *   b. copy l_agdsp (embedded in this image, 0x4d6480 bytes, NOT in git) to 0x88f00000 and zero the
 *      smsg ring + command area (sprd_audcp_memset_communication_area);
 *   c. start_store() of sprd_audcp_boot.c with dsp-reboot-mode 1 (boot vector left as U-Boot set it);
 *   d. wait for power-up and messages, then the V17 register read over the ring.
 * Original v17 header:
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

static void upd(uintptr_t a, uint32_t mask, uint32_t val) { wr32(a, (rd32(a) & ~mask) | (val & mask)); }

static void dump32(const char *n, uintptr_t a, int words)
{
	label(n);
	for (int i = 0; i < words; i++) { puthex(rd32(a + 4 * i), 8); putc_(' ', WHITE); if (i % 4 == 3 && i + 1 < words) { nl(); puts_("    ", WHITE); } }
	nl();
}

static void state(const char *tag)
{
	uint32_t st = rd32(PMU + 0x544);
	label(tag); kv(" SYS", (st >> 8) & 0x1f, 2); kv("CORE", (st >> 16) & 0x1f, 2); kv("SLP", rd32(PMU + 0xd4) & 0xffff, 4);
	kv("RXWR", rd32(PTRS + 12), 4); kv("TXRD", rd32(PTRS), 4); nl();
}


/* ------------------------------------------------------------------ v20 */
static int up(void) { uint32_t st = rd32(PMU + 0x544); return ((st >> 8) & 0x1f) == 0 && ((st >> 16) & 0x1f) == 0; }
#define AGCP_AHB	0x56000000UL
#define AGCP_SET	0x56001000UL
#define MCDT		0x56490000UL
#define DIGC		0x56750000UL
#define MCH		4		/* MCDT DAC channel for FAST playback */
#define FE_FAST		3
#define PA_GPIO		58

#define ADI_FIFO_STS	0x30
static int adi_write(uint32_t reg, uint32_t v)
{
	int i;
	for (i = 0; i < 100000 && (rd32(ADI_BASE + ADI_FIFO_STS) & (1u << 11)); i++) ;
	wr32(ADI_SLAVE + reg, v & 0xffff);
	for (i = 0; i < 100000 && !(rd32(ADI_BASE + ADI_FIFO_STS) & (1u << 10)); i++) ;
	return i == 100000;
}
static void pset(uint32_t reg, uint32_t bits) { adi_write(reg, rd(reg) | bits); }
static void pclr(uint32_t reg, uint32_t bits) { adi_write(reg, rd(reg) & ~bits); }
static void delay_us(unsigned us) { uint64_t end = now() + cntfrq / 1000000 * us + 1; while (now() < end) ; }

/* ---- IPC (layout = Otto's struct agdsp_smem: 1 KiB params, 64 tx, 64 rx, 4 ptrs) */
static uint32_t rx_rd;
static void ipc_send(uint16_t ch, uint16_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3)
{
	uint32_t wr = rd32(PTRS + 4);
	uintptr_t m = TXBUF + 20 * (wr % RING_N);
	wr32(m + 0, cmd | (uint32_t)ch << 16);
	wr32(m + 4, p0); wr32(m + 8, p1); wr32(m + 12, p2); wr32(m + 16, p3);
	__asm__ volatile("dsb sy");
	wr32(PTRS + 4, wr + 1);
	__asm__ volatile("dsb sy");
	mbox_send(1, 0);
}
/* wait for a reply with this cmd/channel; returns its param3 or -1000 on timeout */
static int ipc_wait(uint16_t ch, uint16_t cmd, int ms)
{
	uint64_t t = now();
	while (now() - t < (uint64_t)ms * (cntfrq / 1000)) {
		while (rx_rd != rd32(PTRS + 12)) {
			uintptr_t m = RXBUF + 20 * (rx_rd % RING_N);
			uint32_t w0 = rd32(m), p3 = rd32(m + 16);
			rx_rd++;
			wr32(PTRS + 8, rx_rd);
			if ((w0 & 0xffff) == cmd && (w0 >> 16) == ch) return (int)p3;
		}
	}
	return -1000;
}
static int ipc_cmd(uint16_t cmd, uint32_t id, uint32_t stream, const void *par, uint32_t n)
{
	const uint8_t *p = par;
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4 && i + b < n; b++) w |= (uint32_t)p[i + b] << (8 * b);
		wr32(CMDPARA + i, w);
	}
	ipc_send(0, cmd, id, stream, (uint32_t)CMDPARA, 0);
	return ipc_wait(0, cmd, 2000);
}
static int kctl(uint32_t ctl, uint32_t id, uint32_t val)
{
	uint32_t p[2] = { id, val };
	return ipc_cmd(9, ctl, 0xffffffff, p, 8);
}

/* ---- DSP startup parameters (packed, 618 bytes; field order of vbc-v4-dsp.h) */
static uint8_t sp[640] __attribute__((aligned(16)));
static uint32_t spo;
static void sp32(uint32_t v) { for (int b = 0; b < 4; b++) sp[spo++] = (uint8_t)(v >> (8 * b)); }
static void sppair(int n, const uint32_t *vals) { for (int i = 0; i < n; i++) { sp32(i); sp32(vals ? vals[i] : 0); } }
static uint32_t build_startup(void)
{
	static const uint32_t one3[3] = { 1, 1, 1 };
	spo = 0;
	for (int i = 0; i < 32; i++) sp[spo++] = 0;		/* stream_name */
	sp32(FE_FAST); sp32(0); sp32(0); sp32(0); sp32(0);	/* fe_id stream tx_id rx_id ref_rx_id */
	sppair(4, 0);		/* rx_source */
	sppair(2, 0);		/* tx_out */
	sppair(3, 0);		/* mux_tx: TX0..2 -> IIS0 */
	sppair(4, 0);		/* mux_rx */
	sppair(5, 0);		/* iis_do */
	sppair(3, one3);	/* tx_wd: 24 bit */
	sppair(3, one3);	/* tx_lr_mod: right high (frame inversion) */
	sppair(4, 0);		/* rx_wd */
	sppair(4, 0);		/* rx_lr_mod */
	for (int i = 0; i < 4; i++) sp32(0);			/* loopback */
	sp32(0); sp32(0);					/* iis_master */
	for (int i = 0; i < 4; i++) { sp32(0); sp32(i); sp32(0); sp32(0); }	/* mdg */
	sp32(0); sp32(0); sp32(0); sp32(0); sp32(0); sp32(0);	/* smthdg[1] */
	for (int i = 0; i < 2; i++) { sp32(0); sp32(i); sp32(i); sp32(0); sp32(0); sp32(i); sp32(0); sp32(0); }	/* mixerdg */
	sp32(0);						/* mixerdg_step */
	for (int i = 0; i < 5; i++) { sp32(0); sp32(i); sp32(0); }	/* mixer: NOT_MIX */
	for (int i = 0; i < 7; i++) sp32(0);			/* sbcenc */
	sp32(0); sp32(0);					/* ivsense */
	sppair(4, 0);		/* mst_sel */
	sp[spo++] = 0; sp[spo++] = 0;				/* voice_record_type */
	return spo;
}

static int scene_start(void)
{
	uint32_t n = build_startup();
	int r1 = ipc_cmd(4, FE_FAST, 0, sp, n);
	uint32_t hw[13] = { 0 };
	hw[8] = FE_FAST; hw[9] = 0; hw[10] = 2; hw[11] = 0 /* 48k */; hw[12] = 3 /* VBC_DAT_L16 */;
	int r2 = ipc_cmd(6, FE_FAST, 0, hw, sizeof hw);
	int r3 = kctl(2 /* SRC */, 0, 48000);
	int r4 = 0;
	for (int i = 0; i < 3; i++) { r4 |= kctl(29, i, 1); r4 |= kctl(30, i, 1); }
	kv("STARTUP", (uint32_t)r1, 8); kv("HWP", (uint32_t)r2, 8); nl();
	kv("SRC", (uint32_t)r3, 8); kv("IIS", (uint32_t)r4, 8); kv("LEN", n, 3); nl();
	return r1;
}

static uint32_t mcdt_avail(void)
{
	uint32_t v = rd32(MCDT + 0xe4 + 4 * MCH);
	uint32_t r = (v >> 16) & 0x3ff, w = v & 0x3ff;
	return w >= r ? 512 - w + r : r - w;
}

/* 1 kHz at 48 kHz: 48-sample period, amplitude 6000 */
static const int16_t sine48[48] = {
	0, 783, 1553, 2296, 3000, 3654, 4243, 4755, 5196, 5543, 5796, 5950,
	6000, 5950, 5796, 5543, 5196, 4755, 4243, 3654, 3000, 2296, 1553, 783,
	0, -783, -1553, -2296, -3000, -3654, -4243, -4755, -5196, -5543, -5796, -5950,
	-6000, -5950, -5796, -5543, -5196, -4755, -4243, -3654, -3000, -2296, -1553, -783 };
static uint32_t phase;
static uint32_t tone_word(int on)
{
	uint16_t s = on ? (uint16_t)sine48[phase % 48] : 0;
	phase++;
	return (uint32_t)s | (uint32_t)s << 16;
}

static void gpio_out(unsigned g, int v)
{
	uintptr_t b = AP_GPIO + (g / 16) * 0x80; uint32_t bit = 1u << (g % 16);
	wr32(b + 0x4, rd32(b + 0x4) | bit);	/* DMSK */
	wr32(b + 0x8, rd32(b + 0x8) | bit);	/* DIR = out */
	wr32(b + 0x0, v ? (rd32(b) | bit) : (rd32(b) & ~bit));
}
static void pa_on(void)
{
	gpio_out(PA_GPIO, 0);
	delay_ms(2);
	for (int i = 1; i < 4; i++) { gpio_out(PA_GPIO, 1); delay_us(10); gpio_out(PA_GPIO, 0); delay_us(10); }
	gpio_out(PA_GPIO, 1);
	delay_ms(22);
}

static void codec_on(void)
{
	/* SC2730 analog (mainline sc2730.c probe + DAPM order for DAC -> HP) */
	pset(0x1808, 1u << 4);			/* MODULE_EN: ANA_AUD_EN */
	adi_write(0x0140, 0);			/* AUD_AUDIF_CTL0 */
	pset(0x180c, (1u << 7) | (1u << 1) | 1u);	/* CLK_EN: AUD_SCLK, AUDIF_6M5, AUDIF */
	pset(0x0100, 1u << 1);			/* AUD_TOPA_CLK_EN: TOPA_6M5 */
	pset(0x1068, (1u << 15) | (1u << 13) | (1u << 12) | (1u << 11));	/* ANA_CLK0 */
	pset(0x1000, (1u << 13) | (1u << 9) | (1u << 4));	/* ANA_PMU0: VB BG BIAS */
	pset(0x1100, (1u << 9) | (1u << 1));	/* ANA_DCL1: DIG_CLK, DRV_SOFT */
	delay_ms(5);
	pset(0x1068, (1u << 8) | (1u << 7));	/* CLK_DAC, CLK_CP */
	pset(0x1100, 1u << 4);			/* DIG_CLK_HPDPOP */
	pset(0x1000, 1u << 1);			/* CP_LDO */
	pset(0x1124, (1u << 11) | (1u << 5));	/* DCL10: CP soft en */
	delay_us(110);
	pclr(0x101c, (1u << 10) | (1u << 9) | (1u << 8));	/* PMU7: CP neg pd off */
	pset(0x1000, 1u << 2);			/* CP_AD_EN */
	delay_us(80);
	pset(0x1000, 1u << 3);			/* CP_EN */
	pset(0x0108, (1u << 2) | (1u << 4));	/* AUD_LP_MODULE_CTRL: DAC_EN_L/R */
	pset(0x1088, (1u << 4) | (1u << 3));	/* ANA_CDC5: DACL DACR */
	pset(0x109c, 1u << 1);			/* ANA_CDC10: HP BUF */
	pset(0x1098, (1u << 3) | (1u << 2));	/* ANA_CDC9: HP switches */
	adi_write(0x1090, (rd(0x1090) & ~0xffu) | 0x77);	/* ANA_CDC7: HP volume L/R 0 dB */
	pset(0x109c, (1u << 3) | (1u << 2));	/* ANA_CDC10: HPL HPR */
	delay_ms(60);
	/* digital codec (ums9230-digital.c): DACL/DACR on, 48 kHz */
	wr32(DIGC + 0x0, rd32(DIGC + 0x0) | 5);
	wr32(DIGC + 0xc, (rd32(DIGC + 0xc) & ~0xfu) | 1);
}

/* keep the MCDT fed at 48 frames/ms (time paced, fill capped) for ms milliseconds */
static uint64_t stage_t0;
static uint32_t fw, fc, f_rprev;
static int f_init;
static void feed_ms(uint32_t ms)
{
	uint64_t end = now() + (uint64_t)ms * (cntfrq / 1000);
	if (!f_init) { f_rprev = (rd32(MCDT + 0xe4 + 4 * MCH) >> 16) & 0x3ff; f_init = 1; }
	while (now() < end) {
		uint64_t us = (now() - stage_t0) * 1000000 / cntfrq;
		uint32_t target = 256 + (uint32_t)(us * 48 / 1000);
		uint32_t v = rd32(MCDT + 0xe4 + 4 * MCH);
		uint32_t r = (v >> 16) & 0x3ff, w = v & 0x3ff;
		fc += (r - f_rprev) & 0x3ff; f_rprev = r;
		uint32_t fill = (w - r) & 0x3ff;
		while (fw < target && fill < 440) { wr32(MCDT + 4 * MCH, tone_word(1)); fw++; fill++; }
	}
}

/* ---- I2C6 (sprd i2c, mainline i2c-sprd.c register layout), polled */
#define I2C6		0x20220000UL
static void i2c_init(void)
{
	wr32(0x20001000, 1u << 27);			/* AP APB EB set: I2C6_EB */
	wr32(0x20010048, 0);				/* ap-i2c6-clk: 26 MHz, div 1 */
	delay_us(10);
	wr32(I2C6 + 0x00, 1u << 8);			/* DVD_OPT */
	wr32(I2C6 + 0x00, (rd32(I2C6) & ~(0xfu << 16)) | (15u << 16));
	wr32(I2C6 + 0x00, (rd32(I2C6) & ~(0xfu << 12)) | (4u << 12));
	wr32(I2C6 + 0x20, (51u << 16) | 76u);		/* 100 kHz from 26 MHz */
	wr32(I2C6 + 0x24, 0);
	wr32(I2C6 + 0x28, 104);
	wr32(I2C6 + 0x2c, 1);
	wr32(I2C6 + 0x14, rd32(I2C6 + 0x14) & ~4u);
	wr32(I2C6 + 0x00, rd32(I2C6) | (1u << 2) | (1u << 1) | (1u << 21) | (1u << 22));
}
static int i2c_msg(uint8_t addr, uint8_t *buf, uint32_t len, int rd, int last)
{
	wr32(I2C6 + 0x2c, 1);				/* reset fifo */
	wr32(I2C6 + 0x04, (uint32_t)addr << 1);
	wr32(I2C6 + 0x08, len);
	uint32_t c = rd32(I2C6) & ~(1u << 3);
	c |= (uint32_t)rd << 3;
	if (rd || last) c &= ~(1u << 20); else c |= 1u << 20;
	wr32(I2C6, c);
	if (!rd) for (uint32_t k = 0; k < len; k++) wr8(I2C6 + 0x10, buf[k]);
	wr32(I2C6, rd32(I2C6) | 1u);			/* start */
	uint64_t t = now(); int done = 0;
	while (now() - t < cntfrq / 50) { if (rd32(I2C6 + 0x14) & 4u) { done = 1; break; } }
	int ack = !(rd32(I2C6 + 0x14) & 2u);
	if (done && ack && rd) for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)rd32(I2C6 + 0x0c);
	wr32(I2C6 + 0x14, rd32(I2C6 + 0x14) & ~2u);
	wr32(I2C6, rd32(I2C6) & ~1u);
	wr32(I2C6 + 0x14, rd32(I2C6 + 0x14) & ~4u);
	return !done ? -2 : (!ack ? -1 : 0);
}
#define FS_ADDR 0x34
static uint32_t fs_writes, fs_fails;
static int fs_wr(uint8_t reg, uint16_t val)
{
	uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	int e = i2c_msg(FS_ADDR, b, 3, 0, 1);
	fs_writes++; if (e) fs_fails++;
	return e;
}
static int fs_rd(uint8_t reg, uint16_t *val)
{
	uint8_t b[2] = { reg, 0 };
	int e = i2c_msg(FS_ADDR, b, 1, 0, 0);
	if (e) return e;
	e = i2c_msg(FS_ADDR, b, 2, 1, 1);
	if (!e) *val = (uint16_t)(b[0] << 8 | b[1]);
	return e;
}
static int fs_upd(uint8_t reg, uint16_t mask, uint16_t val)
{
	uint16_t o = 0;
	int e = fs_rd(reg, &o);
	if (e) { fs_fails++; return e; }
	return fs_wr(reg, (uint16_t)((o & ~mask) | (val & mask)));
}

/* ---- frsm-spk1.bin (FourSemi fwm: 32-byte header, INDX table of {type, offset}, tables "NAME"+u16 size) */
extern const uint8_t frsm_bin[], frsm_bin_end[];
__asm__(".section .rodata.frsm, \"a\"\n.balign 16\n.globl frsm_bin\nfrsm_bin:\n.incbin \"" FRSM_PATH "\"\n.globl frsm_bin_end\nfrsm_bin_end:\n.balign 16\n.text\n");
static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static const uint8_t *fw_table(int type)
{
	const uint8_t *idx = frsm_bin + 32;		/* "INDX" */
	uint16_t n = u16le(idx + 4) / 4;
	for (int i = 0; i < n; i++)
		if (u16le(idx + 6 + 4 * i) == type) return idx + u16le(idx + 6 + 4 * i + 2);
	return 0;
}
static uint16_t fw_chip(void) { return u16le(frsm_bin + 14); }
static int fw_nscenes(void) { const uint8_t *t = fw_table(2); return t ? u16le(t + 4) / 8 : -1; }
static int fw_write_reg_table(uint16_t off)
{
	const uint8_t *rt = fw_table(4);
	if (!rt || off == 0xffff) return -10;
	const uint8_t *t = rt + off;
	uint16_t size = u16le(t);
	const uint8_t *p = t + 2;
	uint32_t k = 0;
	while (k < size) {
		uint8_t c = p[k];
		if (c <= 0xf8) { fs_wr(c, u16le(p + k + 1)); k += 3; }
		else if (c == 0xfd) { fs_upd(p[k + 1], u16le(p + k + 4), u16le(p + k + 2)); k += 6; }
		else if (c == 0xff) { delay_ms(u16le(p + k + 1)); k += 3; }
		else return -11;		/* burst (0xfe) not used by this table */
	}
	return 0;
}
static int fs_init_and_start(void)
{
	const uint8_t *sc = fw_table(2);
	if (!sc) return -20;
	const uint8_t *s0 = sc + 6, *s1 = sc + 6 + 8;
	/* dev_init: i2c reset, wait CHIPINI */
	fs_wr(0x10, 0x0003);
	delay_ms(5);
	uint16_t v = 0; int i;
	for (i = 0; i < 20; i++) { if (!fs_rd(0x0e, &v) && (v & 3) == 3) break; delay_ms(2); }
	if (i == 20) return -21;
	int r = fw_write_reg_table(u16le(s0 + 2));
	if (r) return r;
	fs_rd(0x3f, &v);
	if (v & 0x8000) { fs_wr(0x3f, v & 0x7fff); delay_ms(8); }
	fs_wr(0x11, 0x0000);
	fs_wr(0x10, 0x0001);
	fs_wr(0x1f, 0x5a5a);
	/* set scene 1 (MUSIC) */
	uint16_t key = 0; fs_rd(0x1f, &key);
	r = fw_write_reg_table(u16le(s1 + 2));
	if (r) return r - 100;
	fs_wr(0x1f, key);
	fs_rd(0x3f, &v);
	int lnm = !!(v & 0x8000);
	fs_upd(0x3f, 0x8000, (uint16_t)(lnm << 15));	/* hw_params */
	fs_wr(0x10, 0x0000);				/* start up */
	delay_ms(5);
	fs_wr(0x11, 0x00c0);				/* unmute */
	return 0;
}

void probe_main(uint64_t dtb, uint64_t load, uint64_t el)
{
	(void)load; (void)dtb; (void)el;
	cntfrq = rsr("cntfrq_el0");
	fb = (volatile uint32_t *)FB_DEFAULT;
	clear_all();
	line("A5 PROBE V24 - FS1588 SPEAKER", YELLOW);

	/* wake the DSP (V19) */
	wr32(AON + 0x04, rd32(AON + 0x04) | 4);
	delay_ms(1);
	wr32(AON + 0x14c, rd32(AON + 0x14c) | 0x20);
	mbox_send(100, 0);
	uint64_t t = now();
	while (now() - t < cntfrq / 5 && !up()) ;
	state("WAKE");
	if (!up()) { line("DSP DID NOT WAKE - STOP", RED); for (;;) __asm__ volatile("wfe"); }
	rx_rd = rd32(PTRS + 8);
	ipc_wait(0xffff, 0xffff, 50);		/* drain BOOT_OK etc. */

	/* AGCP clocks: AUD_EB (19), MCDT_EB (12). V20 run 1: the set register (+0x1000) left bit 12 clear
	 * (EB = 0x00C90000) and MCDT reads faulted. Try a plain read-modify-write too. */
	kv("AGCP EB", rd32(AGCP_AHB), 8); kv("AGSEL", rd32(AGCP_AHB + 0x3c), 8); nl();
	wr32(AGCP_SET, (1u << 19) | (1u << 12));
	wr32(AGCP_AHB, rd32(AGCP_AHB) | (1u << 19) | (1u << 12));
	delay_ms(1);
	kv("EB NOW", rd32(AGCP_AHB), 8); nl();

	codec_on();
	kv("PMU0", rd(0x1000), 4); kv("CLK0", rd(0x1068), 4); kv("CDC10", rd(0x109c), 4); kv("CDC9", rd(0x1098), 4); nl();
	kv("DIG TOP", rd32(DIGC), 8); kv("DAC", rd32(DIGC + 0xc), 8); nl();

	line("SCENE START (BEFORE MCDT)", YELLOW);
	scene_start();
	kv("EB AFTER", rd32(AGCP_AHB), 8); nl();
	if (!(rd32(AGCP_AHB) & (1u << 12))) {
		line("MCDT CLOCK STILL OFF - STOP", RED);
		ipc_cmd(5, FE_FAST, 0, sp, spo);
		for (;;) __asm__ volatile("wfe");
	}
	kv("MCDT AV", mcdt_avail(), 3); kv("ST", rd32(MCDT + 0x134), 8); nl();

	for (int attempt = 0; attempt < 2; attempt++) {
		if (attempt) { line("SCENE RESTART", YELLOW); scene_start(); }
		/* MCDT ch4: clear, watermarks like Linux (full 511, empty 320), no DMA: PIO */
		wr32(MCDT + 0x158, rd32(MCDT + 0x158) | (1u << MCH));
		wr32(MCDT + 0x60 + 4 * MCH, (320u << 16) | 511u);
		kv("AV0", mcdt_avail(), 3);
		for (int i = 0; i < 480; i++) wr32(MCDT + 4 * MCH, tone_word(0));
		kv("AV1", mcdt_avail(), 3); nl();
		ipc_send(0, 7, FE_FAST, 0, 1, 0);	/* HW_TRIGGER start */
		delay_ms(200);
		uint32_t av = mcdt_avail();
		kv("AV 200MS", av, 3); kv("ST", rd32(MCDT + 0x134), 8); nl();
		if (av > 100) break;			/* it drains */
		line("NOT DRAINING -> SHUTDOWN + RESTART", RED);
		ipc_cmd(5, FE_FAST, 0, sp, spo);
	}

	/* ---- SC2730 speaker PA, stock route */
	pset(0x1068, 1u << 2);					/* ANA_CLK0 CLK_PA_32K */
	pset(0x1100, (1u << 0) | (1u << 2));			/* ANA_DCL1 DIG_CLK_PA, DIG_CLK_FGU */
	adi_write(0x1094, (rd(0x1094) & ~0xfu) | 0x6);		/* CDC8 PA_G = 6 */
	adi_write(0x10b8, rd(0x10b8) & ~(0x1fu << 11));		/* CDC17 PA_RSV = 0 */
	pset(0x108c, 1u << 3);					/* CDC6 DAS_EN (DACS Switch) */
	delay_us(110);
	adi_write(0x10c0, (rd(0x10c0) & ~(3u << 6)) | (3u << 6));
	delay_us(15);
	adi_write(0x10b8, (rd(0x10b8) & ~(0x1fu << 11)) | (8u << 11));
	delay_ms(20);
	adi_write(0x10c0, rd(0x10c0) & ~(3u << 6));
	delay_us(110);
	pset(0x1098, 1u << 4);					/* CDC9 SDAPA */
	adi_write(0x108c, (rd(0x108c) & ~3u) | 1u | (1u << 2));	/* DAS DC offset */
	adi_write(0x10c0, rd(0x10c0) & ~(3u << 6));		/* SPK PA event */
	pclr(0x10ac, (1u << 5) | (1u << 0));			/* class AB, depop on */
	delay_ms(1);
	pset(0x10ac, 1u << 6);					/* PA_EN */
	delay_ms(60);
	int i;
	for (i = 0; i < 135 && !(rd(0x11b0) & (1u << 8)); i++) delay_ms(1);
	kv("SPK PA READY", (rd(0x11b0) >> 8) & 1, 1); kv("MS", 60 + i, 3); kv("CDC14", rd(0x10ac), 4); nl();

	/* ---- FS1588 */
	gpio_out(PA_GPIO, 0);
	i2c_init();
	uint16_t v = 0;
	int e = fs_rd(0x01, &v);
	kv("FS DEVID", v, 4); kv("ERR", (uint32_t)e, 2);
	if (e) {
		gpio_out(PA_GPIO, 1); delay_us(1300);
		e = fs_rd(0x01, &v);
		kv(" MOD1", v, 4); kv("ERR", (uint32_t)e, 2);
	}
	nl();
	uint16_t a0 = 0; fs_rd(0x0f, &a0);
	kv("0F ADDR", a0, 4); kv("FW CHIP", fw_chip(), 4); kv("SCENES", fw_nscenes(), 2); nl();
	int r = fs_init_and_start();
	uint16_t s0 = 0, s3 = 0, s10 = 0, s11 = 0, s1f = 0;
	fs_rd(0x00, &s0); fs_rd(0x03, &s3); fs_rd(0x10, &s10); fs_rd(0x11, &s11); fs_rd(0x1f, &s1f);
	kv("FS INIT", (uint32_t)r, 8); kv("WRITES", fs_writes, 3); kv("FAILS", fs_fails, 3); nl();
	kv("00", s0, 4); kv("03", s3, 4); kv("10", s10, 4); kv("11", s11, 4); kv("1F", s1f, 4); nl();

	/* ---- tone */
	stage_t0 = now();
	line("LISTEN: TONE ON SPEAKER (4 S)", GREEN);
	feed_ms(4000);
	kv("WRITTEN", fw, 8); kv("CONSUMED", fc, 8); nl();
	fs_rd(0x00, &s0); fs_rd(0x03, &s3);
	kv("AFTER 00", s0, 4); kv("03", s3, 4); nl();
	fs_wr(0x11, 0x0000);				/* mute */
	fs_wr(0x10, 0x0001);				/* power down */
	pclr(0x10ac, 1u << 6);
	gpio_out(PA_GPIO, 0);
	ipc_cmd(5, FE_FAST, 0, sp, spo);
	line("DONE", GREEN);
	for (;;) __asm__ volatile("wfe");
}
