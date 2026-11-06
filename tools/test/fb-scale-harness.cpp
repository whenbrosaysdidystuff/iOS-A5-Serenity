#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <vector>
struct Mem { uint64_t len; uint64_t getLength() const { return len; } };
struct SweetSurface { uint32_t width, height, bytesPerRow, offset, elementWidth = 1, elementHeight = 1; Mem *memory; };
struct AppleCLCD {
	uint32_t fWidth, fHeight, fRowBytes; uintptr_t fPixels;
	uint32_t fScaleMode, fScaleNum, fScaleDen, fScaledW, fScaledH, fPadX, fPadY; uint32_t *fColX;
	void presentScaled(SweetSurface *s, uintptr_t src, int32_t sx, int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h);
	void clearBars(void);
};
static AppleCLCD *sweet_fb_main;
#include "funcs.inc"
#include "p2c.inc"
int main(int argc, char **argv) {
	int mode = atoi(argv[1]);
	const uint32_t LW = 1125, LH = 2436, PW = 720, PH = 1640, stride = 2880;
	std::vector<uint32_t> canvas(LW * LH), panel(stride / 4 * PH, 0xffff00ff);
	for (uint32_t y = 0; y < LH; y++) for (uint32_t x = 0; x < LW; x++) {
		uint32_t c = 0xff000040 + ((x * 200 / LW) << 16) + ((y * 200 / LH) << 8);
		if (x % 75 == 0 || y % 75 == 0) c = 0xffffffff;          /* grid */
		if (x < 3 || y < 3 || x >= LW - 3 || y >= LH - 3) c = 0xffff0000; /* red border */
		if ((x / 6 + y / 6) % 2 && y > 1100 && y < 1300 && x > 300 && x < 800) c = 0xff000000; /* fine checker */
		canvas[y * LW + x] = c;
	}
	AppleCLCD fb{}; fb.fWidth = PW; fb.fHeight = PH; fb.fRowBytes = stride; fb.fPixels = (uintptr_t)panel.data();
	/* same setup as AppleCLCD::start */
	if ((uint64_t)PW * LH <= (uint64_t)PH * LW) { fb.fScaleNum = PW; fb.fScaleDen = LW; } else { fb.fScaleNum = PH; fb.fScaleDen = LH; }
	fb.fScaledW = (uint32_t)((uint64_t)LW * fb.fScaleNum / fb.fScaleDen); fb.fScaledH = (uint32_t)((uint64_t)LH * fb.fScaleNum / fb.fScaleDen);
	fb.fPadX = (PW - fb.fScaledW) / 2; fb.fPadY = (PH - fb.fScaledH) / 2; fb.fColX = new uint32_t[PW]; fb.fScaleMode = mode;
	sweet_fb_main = &fb;
	Mem m{(uint64_t)LW * LH * 4}; SweetSurface s{LW, LH, LW * 4, 0, 1, 1, &m};
	fb.clearBars();
	fb.presentScaled(&s, (uintptr_t)canvas.data(), 0, 0, 0, 0, LW, LH);
	/* checks: no magenta left (everything written), border red at the canvas edges */
	uint32_t magenta = 0; for (uint32_t y = 0; y < PH; y++) for (uint32_t x = 0; x < PW; x++) magenta += panel[y * 720 + x] == 0xffff00ff;
	printf("mode %d scaled %ux%u pad %u,%u unwritten %u\n", mode, fb.fScaledW, fb.fScaledH, fb.fPadX, fb.fPadY, magenta);
	printf("top-left canvas px %08x, bottom-right %08x, bar %08x\n", panel[fb.fPadY * 720 + 0], panel[(fb.fPadY + fb.fScaledH - 1) * 720 + 719], panel[5 * 720 + 5]);
	uint32_t cx, cy; bool ok = sweet_fb_panel_to_canvas(360, 820, &cx, &cy); printf("panel 360,820 -> canvas %u,%u (%d)\n", cx, cy, ok);
	ok = sweet_fb_panel_to_canvas(0, 10, &cx, &cy); printf("panel 0,10 -> %d (bar)\n", ok);
	ok = sweet_fb_panel_to_canvas(719, 1599, &cx, &cy); printf("panel 719,1599 -> canvas %u,%u (%d)\n", cx, cy, ok);
	/* partial layer: a 200x100 rect at canvas 100,200 from surface origin 0,0 */
	fb.presentScaled(&s, (uintptr_t)canvas.data(), 0, 0, 100, 200, 200, 100);
	FILE *f = fopen(argv[2], "wb"); fprintf(f, "P6 %u %u 255\n", PW, PH);
	for (uint32_t i = 0; i < PW * PH; i++) { uint32_t c = panel[i]; fputc(c >> 16 & 255, f); fputc(c >> 8 & 255, f); fputc(c & 255, f); }
	fclose(f);
	return 0;
}
