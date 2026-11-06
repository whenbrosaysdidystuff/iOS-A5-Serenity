/*
 * Stop the SC2730 PMIC watchdog that the A5 bootloader leaves armed (300 s, then the
 * PMIC resets the phone). Sequence and checks are exactly those of probe V13, which
 * stopped it on the phone (CTRL 0x000a -> 0x0000, counter frozen, still running at 226 s).
 *
 * ADI controller (sprd,ums9230-adi) at 0x64200000, PMIC slave window at +0x20000.
 * Reads: write the slave register's physical address to RD_CMD, poll RD_DATA (bit 31
 * busy, bits 30:16 echo address bits 16:2, bits 15:0 value). Writes go straight to the
 * slave window while the write FIFO is not full. Watchdog at PMIC 0x40, 32768 Hz.
 *
 * Nothing is written unless every read echoed the right address, CTRL says RUN, and the
 * counter really moves - so on other hardware (or QEMU) this does nothing.
 */
#include <stdint.h>
#include "pmic_wdt.h"

#define ADI_BASE	0x64200000UL
#define ADI_SLAVE	(ADI_BASE + 0x20000UL)
#define ADI_RD_CMD	0x28
#define ADI_RD_DATA	0x2c
#define ADI_FIFO_STS	0x30
#define FIFO_FULL	(1u << 11)
#define FIFO_EMPTY	(1u << 10)
#define WDG		0x40
#define W_CTRL		(WDG + 0x08)
#define W_CNT_LO	(WDG + 0x18)
#define W_CNT_HI	(WDG + 0x1c)
#define W_LOCK		(WDG + 0x20)
#define UNLOCK_KEY	0xe551
#define C_INT		1u
#define C_RUN		2u
#define C_RST		8u

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint64_t now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline uint64_t freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

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

static int adi_write(uint32_t reg, uint32_t v)
{
	int i;
	for (i = 0; i < 100000 && (rd32(ADI_BASE + ADI_FIFO_STS) & FIFO_FULL); i++)
		;
	if (i == 100000) return 1;
	wr32(ADI_SLAVE + reg, v & 0xffff);
	for (i = 0; i < 100000 && !(rd32(ADI_BASE + ADI_FIFO_STS) & FIFO_EMPTY); i++)
		;
	return i == 100000 ? 2 : 0;
}

static uint32_t cnt(int *rc)
{
	uint32_t lo = 0, hi = 0;
	*rc |= adi_read(W_CNT_HI, &hi) | adi_read(W_CNT_LO, &lo);
	return hi << 16 | lo;
}

static void wait_ms(unsigned ms)
{
	uint64_t end = now() + freq() / 1000 * ms;
	while (now() < end)
		;
}

/* returns PMIC_WDT_STOPPED, _NOT_RUNNING, _NOT_VERIFIED or _STILL_RUNNING; *ctrl = CTRL before */
int pmic_wdt_stop(uint32_t *ctrl)
{
	uint32_t c0 = 0, c1 = 0;
	int rc;

	adi_bad = 0;
	rc = adi_read(W_CTRL, &c0);
	uint32_t n1 = cnt(&rc);
	wait_ms(100);		/* 100 ms = ~3277 ticks */
	uint32_t n2 = cnt(&rc);
	*ctrl = c0;
	if (rc || adi_bad)
		return PMIC_WDT_NOT_VERIFIED;
	if (!(c0 & C_RUN) || n1 == n2)
		return PMIC_WDT_NOT_RUNNING;

	int w = adi_write(W_LOCK, UNLOCK_KEY);
	w |= adi_write(W_CTRL, c0 & ~(C_INT | C_RUN | C_RST));
	w |= adi_write(W_LOCK, 0);

	rc = adi_read(W_CTRL, &c1);
	n1 = cnt(&rc);
	wait_ms(100);
	n2 = cnt(&rc);
	if (w || rc || adi_bad || (c1 & C_RUN) || n1 != n2)
		return PMIC_WDT_STILL_RUNNING;
	return PMIC_WDT_STOPPED;
}
