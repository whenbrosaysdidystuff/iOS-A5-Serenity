Probe V15: FocalTech FT8057M touch on the Redmi A5 over SPI3 (bare metal, before any OS).
Build needs `fw.bin` = the 258048-byte `fw_file` array from the phone's own
`vendor_boot` ramdisk module `lib/modules/focaltech_ft8057_spi_ts.ko` (.data + 0x4b0).
It is Xiaomi's firmware: never commit it.
