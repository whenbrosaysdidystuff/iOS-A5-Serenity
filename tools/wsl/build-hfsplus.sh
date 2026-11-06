#!/bin/bash
# Build and load the HFS+ filesystem module for the running WSL2 kernel (Microsoft's WSL
# kernel ships without CONFIG_HFSPLUS_FS). Needed by tools/mkiosroot.sh to fill the iOS root
# image. Run inside WSL (Ubuntu) as your normal user:  bash build-hfsplus.sh
# Afterwards (also after every WSL restart):  sudo insmod ~/wsl-hfsplus/hfsplus.ko
set -euo pipefail
KREL=$(uname -r)                                  # e.g. 6.18.58.1-microsoft-standard-WSL2
KVER=${KREL%%-*}                                  # 6.18.58.1
TAG=linux-msft-wsl-$KVER
OUT=~/wsl-hfsplus
SRC=~/wsl-kernel/WSL2-Linux-Kernel-$TAG
say() { printf '\n=== %s\n' "$*"; }

if grep -qw hfsplus /proc/filesystems; then echo "HFS+ is already available."; exit 0; fi
if [[ -f $OUT/hfsplus.ko ]] && sudo insmod $OUT/hfsplus.ko && grep -qw hfsplus /proc/filesystems; then
	echo "HFSPLUS_OK (loaded the module built earlier)"; exit 0
fi

say "1/6 build tools"
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential flex bison bc libelf-dev libssl-dev dwarves python3 cpio rsync curl kmod >/dev/null

say "2/6 kernel source $TAG (about 250 MB)"
mkdir -p ~/wsl-kernel && cd ~/wsl-kernel
if [[ ! -d $SRC ]]; then
	curl -fL --retry 3 -o $TAG.tar.gz https://github.com/microsoft/WSL2-Linux-Kernel/archive/refs/tags/$TAG.tar.gz
	tar xzf $TAG.tar.gz
fi
cd "$SRC"

say "3/6 configuration = the running kernel's, plus HFS+ as a module"
zcat /proc/config.gz > .config
if grep -qE '^CONFIG_RANDSTRUCT_FULL=y|^CONFIG_RANDSTRUCT_PERFORMANCE=y' .config; then
	echo "ERROR: this kernel randomizes struct layouts; an out-of-tree module would not match." >&2; exit 1
fi
scripts/config --module CONFIG_HFSPLUS_FS
make -s olddefconfig
REL=$(make -s kernelrelease)
if [[ $REL != "$KREL" ]]; then
	echo "ERROR: source builds '$REL' but the running kernel is '$KREL'." >&2; exit 1
fi
echo "kernel release matches: $REL"

say "4/6 building the kernel once for its symbol versions ($(nproc) cores; 20-60 minutes)"
make -s -j"$(nproc)" vmlinux
cp vmlinux.symvers Module.symvers

say "5/6 building hfsplus.ko"
make -s -j"$(nproc)" M=fs/hfsplus modules
mkdir -p $OUT && cp fs/hfsplus/hfsplus.ko $OUT/
modinfo $OUT/hfsplus.ko | grep -E 'vermagic|depends'

say "6/6 loading it"
sudo insmod $OUT/hfsplus.ko
grep -qw hfsplus /proc/filesystems && echo "HFSPLUS_OK" || { echo "HFSPLUS_MISSING after insmod"; sudo dmesg | tail -5; exit 1; }
echo "After a WSL restart, load it again with:  sudo insmod $OUT/hfsplus.ko"
