#!/bin/sh
# MMU is off in the probe: every access is Device memory, so unaligned accesses fault -> -mstrict-align.
set -e
F="--target=aarch64-none-elf -ffreestanding -nostdlib -fno-pic -O2 -mgeneral-regs-only -mstrict-align -fno-stack-protector"
clang $F -DFRSM_PATH="\"${FRSM_BIN:?set FRSM_BIN to frsm-spk1.bin (private, not in git)}\"" -c probe.c -o p.o
clang $F -c start.S -o s.o
ld.lld -T link.ld -nostdlib s.o p.o -o p.elf
llvm-objcopy -O binary p.elf Image
python3 pack_boot_v4.py Image boot_probe.img
rm -f s.o p.o p.elf Image
