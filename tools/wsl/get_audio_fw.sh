#!/bin/bash
# Run inside WSL. Pulls the speaker-amp firmware and audio configs out of the vendor partition in
# Desktop\backup\super.bin. Nothing on the phone or in backup\ is changed. Output (private, not for git):
#   Desktop\a5-ios-port\private\vendor-audio\
set -e
D=/mnt/c/Users/yeah69/Desktop
OUT=$D/a5-ios-port/private/vendor-audio
W=$HOME/a5-vendor
mkdir -p "$W" "$OUT"
cd "$W"
echo "== 1/3 cutting vendor_a out of super.bin"
python3 "$D/a5-ios-port/tools/lp_extract.py" "$D/backup/super.bin" vendor_a "$W/vendor.img"
echo "== 2/3 unpacking the vendor filesystem"
rm -rf "$W/v"; mkdir -p "$W/v"
if [ "$(od -An -tx1 -j1024 -N4 vendor.img | tr -d ' ')" = "e2e1f5e0" ]; then
	command -v fsck.erofs >/dev/null || sudo apt-get install -y erofs-utils
	fsck.erofs --extract="$W/v" vendor.img >/dev/null
else
	command -v debugfs >/dev/null || sudo apt-get install -y e2fsprogs
	debugfs -R "rdump / $W/v" vendor.img >/dev/null 2>&1
fi
echo "== 3/3 copying amp firmware and audio configs"
cd "$W/v"
find . \( -iname 'frsm*' -o -iname '*fs15*' -o -iname '*foursemi*' \) -type f -exec cp --parents {} "$OUT" \;
find . -path '*etc*' -type f \( -iname '*audio*' -o -iname '*codec*' -o -iname '*mixer*' -o -iname '*route*' -o -iname '*smartamp*' \) -size -4M -exec cp --parents {} "$OUT" \;
find . -path '*firmware*' -type f \( -iname '*vbc*' -o -iname '*audio*' -o -iname '*dsp*' \) -size -8M -exec cp --parents {} "$OUT" \;
echo "== done, files copied:"
find "$OUT" -type f | sed "s|$OUT/||" | head -100
echo "TOTAL: $(find "$OUT" -type f | wc -l) files"
