#pragma once
#include <stdint.h>

/* Redmi A5 bootloader splash framebuffer (bootargs lcd_base=9cf2a000, lcd_size=1640x720) */
#define FB_BASE		0x9cf2a000UL
#define FB_W		720
#define FB_H		1640
#define FB_STRIDE	(FB_W * 4)

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
