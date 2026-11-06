#!/bin/bash
# Build the Redmi A5 shim (clang + lld, no GCC cross toolchain needed) and, given a payload
# from tools/mkpayload.py, the flashable boot image:  ./build.sh [payload.bin]  -> out/a5-boot.img
set -euo pipefail
cd "$(dirname "$0")"
CF="-target aarch64-none-elf -march=armv8.2-a -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c loader.c pmic_wdt.c; do
	clang $CF -c $f -o out/${f%.*}.o
done
# Runs wherever the bootloader puts it: only the header's image_size may be absolute.
abs=$(llvm-readelf -r out/shim.o out/console.o out/loader.o out/pmic_wdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -T link.ld --build-id=none -z norelro -o out/shim.elf out/start.o out/shim.o out/console.o out/loader.o out/pmic_wdt.o
llvm-objcopy -O binary out/shim.elf out/Image
python3 ../tools/mka5boot.py out/shim.elf out/Image "${1:-}" out/a5-boot.img
