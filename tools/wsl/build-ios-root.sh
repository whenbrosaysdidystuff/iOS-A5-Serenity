#!/bin/bash
# Build the iOS 16.7.16 (iPhone X, 20H392) root image for the Redmi A5 inside WSL.
#   bash build-ios-root.sh
# Steps (each is skipped when its result already exists, so it can be re-run after a failure):
#   1. tools: apfs-fuse (built from source), hfsprogs, img2simg; HFS+ module loaded
#   2. this repo (branch a5) -> ~/iOS-A5-Serenity
#   3. from Apple's IPSW only the needed members (~7.5 GB, resumable, CRC-checked):
#      OS root, SystemOS + AppOS cryptexes, DeviceTree.d22ap.im4p
#   4. tools/mkiosroot.sh -> ~/ios-root/ios16-root.img (24 GB HFS+, sparse)
#   5. tools/sweet-root-tweaks.py on the mounted image
#   6. img2simg -> Desktop\a5-ios-port\images\ios16-root.simg (for fastboot)
# Nothing here touches the phone. Apple's files stay in ~/ipsw and are never uploaded anywhere.
set -euo pipefail
URL='https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw'
OS_DMG=087-86698-044.dmg        # OS root filesystem (NOTES.md, iPhone10,3 20H392)
SYSOS_DMG=087-86593-047.dmg     # Cryptex1,SystemOS
APPOS_DMG=087-86715-047.dmg     # Cryptex1,AppOS
SIZE=24G
REPO=~/iOS-A5-Serenity
IPSW=~/ipsw/iPhone10,3_16.7.16_20H392
WORK=~/ios-root
IMG=$WORK/ios16-root.img
WINOUT=/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')/Desktop/a5-ios-port/images
say() { printf '\n=== %s\n' "$*"; }

say "1/6 tools"
sudo apt-get install -y -qq hfsprogs rsync git python3 curl fuse3 libfuse3-dev cmake g++ zlib1g-dev libbz2-dev libattr1-dev libicu-dev android-sdk-libsparse-utils >/dev/null
if ! grep -qw hfsplus /proc/filesystems; then
	sudo insmod ~/wsl-hfsplus/hfsplus.ko || { echo "ERROR: HFS+ module missing - run build-hfsplus.sh first" >&2; exit 1; }
fi
if ! command -v apfs-fuse >/dev/null; then
	[[ -d ~/apfs-fuse ]] || git clone -q --recursive https://github.com/sgan81/apfs-fuse ~/apfs-fuse
	cmake -S ~/apfs-fuse -B ~/apfs-fuse/build -DUSE_FUSE3=ON -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint" >/dev/null
	cmake --build ~/apfs-fuse/build -j"$(nproc)" >/dev/null
	sudo install -m 0755 ~/apfs-fuse/build/apfs-fuse /usr/local/bin/apfs-fuse
fi
echo "apfs-fuse: $(command -v apfs-fuse), HFS+: loaded"

say "2/6 repository"
if [[ -d $REPO/.git ]]; then git -C $REPO pull -q --ff-only; else
	git clone -q -b a5 https://github.com/whenbrosaysdidystuff/iOS-A5-Serenity $REPO; fi
git -C $REPO log --oneline -1

say "3/6 iOS 16.7.16 files from Apple (resumable; ~7.5 GB)"
mkdir -p "$IPSW"
for m in $SYSOS_DMG $APPOS_DMG $OS_DMG DeviceTree.d22ap.im4p; do
	if [[ -f $IPSW/$m || -f $IPSW/Firmware/all_flash/$m ]]; then echo "have $m"; continue; fi
	echo "downloading $m ..."
	python3 $REPO/tools/remotezip.py "$URL" "$m" "$IPSW"
done
for m in $OS_DMG $SYSOS_DMG $APPOS_DMG; do
	[[ -f $IPSW/$m ]] || { echo "ERROR: $m not in the IPSW; its disk images are:" >&2
		python3 $REPO/tools/remotezip.py "$URL" | grep -i '\.dmg$' >&2; exit 1; }
done
ls -la "$IPSW"/*.dmg

say "4/6 building the HFS+ root image (30-90 minutes)"
mkdir -p "$WORK"
if [[ ! -f $WORK/.built ]]; then
	rm -f "$IMG"
	bash $REPO/tools/mkiosroot.sh "$IPSW/$OS_DMG" "$IPSW/$SYSOS_DMG" "$IPSW/$APPOS_DMG" "$IMG" $SIZE
	touch "$WORK/.built"
fi

say "5/6 bring-up tweaks"
if [[ ! -f $WORK/.tweaked ]]; then
	sudo fsck.hfsplus -fy "$IMG" | tail -2 || true
	sudo mkdir -p /mnt/iosroot
	sudo mount -t hfsplus -o loop,force "$IMG" /mnt/iosroot
	sudo python3 $REPO/tools/sweet-root-tweaks.py /mnt/iosroot
	df -h /mnt/iosroot | tail -1
	sudo umount /mnt/iosroot
	fsck.hfsplus -fn "$IMG" | tail -2
	touch "$WORK/.tweaked"
fi

say "6/6 sparse image for fastboot -> Windows Desktop"
img2simg "$IMG" "$WORK/ios16-root.simg"
mkdir -p "$WINOUT"
cp "$WORK/ios16-root.simg" "$WINOUT/"
cp "$IPSW/Firmware/all_flash/DeviceTree.d22ap.im4p" "$WINOUT/../" 2>/dev/null || true
ls -la "$WINOUT/ios16-root.simg"
sha256sum "$WORK/ios16-root.simg"
echo "DONE: images\\ios16-root.simg is ready (do NOT flash it until Claude gives the boot image too)"
