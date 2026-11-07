#!/bin/bash
# Put A5Audio.driver (userland/A5Audio, the iOS audio output device for /dev/a5audio) into the iOS root
# image and make a new sparse image for fastboot:
#   bash install_a5audio.sh
# Reads  Desktop\a5-ios-port\userland\A5Audio.driver\{A5Audio,Info.plist}
# Writes ~/ios-root/ios16-root.img (in place) and Desktop\a5-ios-port\images\ios16-root.simg
# Nothing here touches the phone.
set -euo pipefail
WINHOME=/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')
SRC=$WINHOME/Desktop/a5-ios-port/userland/A5Audio.driver
IMG=~/ios-root/ios16-root.img
WINOUT=$WINHOME/Desktop/a5-ios-port/images
MNT=/mnt/iosroot
DST=System/Library/Audio/Plug-Ins/HAL/A5Audio.driver

[[ -f $SRC/A5Audio && -f $SRC/Info.plist ]] || { echo "ERROR: $SRC/A5Audio and Info.plist missing" >&2; exit 1; }
grep -qw hfsplus /proc/filesystems || sudo insmod ~/wsl-hfsplus/hfsplus.ko

echo "1/4 check + mount the root image read-write"
sudo umount /tmp/iosroot-ro 2>/dev/null || true
sudo fsck.hfsplus -fy "$IMG" | tail -2 || true
sudo mkdir -p $MNT
sudo mount -t hfsplus -o loop,force "$IMG" $MNT
trap 'sudo umount $MNT 2>/dev/null || true' EXIT

echo "2/4 install $DST"
sudo mkdir -p "$MNT/$DST"
sudo install -o 0 -g 0 -m 0755 "$SRC/A5Audio" "$MNT/$DST/A5Audio"
sudo install -o 0 -g 0 -m 0644 "$SRC/Info.plist" "$MNT/$DST/Info.plist"
sudo chown 0:0 "$MNT/$DST"; sudo chmod 0755 "$MNT/$DST"
ls -la "$MNT/$DST" "$MNT/System/Library/Audio/Plug-Ins/HAL/"
sha256sum "$MNT/$DST/A5Audio"
sync
sudo umount $MNT
trap - EXIT

echo "3/4 filesystem check"
fsck.hfsplus -fn "$IMG" | tail -2

echo "4/4 sparse image -> $WINOUT (several minutes)"
img2simg "$IMG" ~/ios-root/ios16-root.simg
cp ~/ios-root/ios16-root.simg "$WINOUT/ios16-root.simg"
ls -la "$WINOUT/ios16-root.simg"
echo "DONE: images\\ios16-root.simg now contains A5Audio.driver"
