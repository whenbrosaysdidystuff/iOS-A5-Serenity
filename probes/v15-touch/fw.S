/* the FT8057M firmware from the phone's own focaltech_ft8057_spi_ts.ko (NOT in git) */
	.section .rodata.fw, "a"
	.balign 16
	.global fw_data, fw_end
fw_data:
	.incbin "fw.bin"
fw_end:
