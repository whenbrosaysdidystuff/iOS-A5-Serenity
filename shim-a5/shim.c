/*
 * Redmi A5 (serenity) boot shim. The bootloader enters at EL2 with the MMU off;
 * start.S has already dropped to EL1 (values proven by the a5-ios-port V12 probe).
 * Here: show where we are, then hand over to xnu (loader.c).
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

extern char _head[];
extern uint64_t payload_off[2];		/* start.S header: offset, size */

void irq_handler(void)
{
	/* IRQs stay masked in the shim; acknowledge anything that arrives anyway */
	uint64_t iar = rsr("icc_iar1_el1");
	if ((iar & 0xffffff) < 1020)
		__asm__ volatile("msr icc_eoir1_el1, %0; isb" : : "r"(iar));
}

void exc_report(uint64_t slot)
{
	uint64_t el = rsr("CurrentEL") >> 2;
	con_clear(BLUE);
	con_hex("EXC SLOT", slot);
	con_hex("EL", el);
	con_hex("ESR", el == 2 ? rsr("esr_el2") : rsr("esr_el1"));
	con_hex("ELR", el == 2 ? rsr("elr_el2") : rsr("elr_el1"));
	con_hex("FAR", el == 2 ? rsr("far_el2") : rsr("far_el1"));
}

void shim_main(uint64_t dtb, uint64_t load, uint64_t entry_el)
{
	con_clear(BLACK);
	con_str("SHIM", "redmi a5 -> xnu");
	con_hex2("EL", "entry=", (uint32_t)entry_el, "now=", (uint32_t)(rsr("CurrentEL") >> 2));
	con_hex("LOAD", load);
	con_hex("DTB", dtb);
	uint64_t off = payload_off[0], size = payload_off[1];
	con_hex2("PAYLOAD", "off=", (uint32_t)off, "size=", (uint32_t)size);
	const char *why = loader_boot((uintptr_t)_head + off, size, dtb);
	con_str("STOP", why);
	for (;;)
		__asm__ volatile("wfe");
}
