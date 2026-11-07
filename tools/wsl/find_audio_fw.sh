#!/bin/bash
# Run inside WSL after get_audio_fw.sh. Searches vendor_a, odm_a, vendor_dlkm_a and product_a from
# Desktop\backup\super.bin for the FS1588 amp firmware and Unisoc audio route/param files.
# Only reads backup\. Writes Desktop\a5-ios-port\private\vendor-audio\ and filelist-*.txt there.
D=/mnt/c/Users/yeah69/Desktop
OUT=$D/a5-ios-port/private/vendor-audio
W=$HOME/a5-vendor
mkdir -p "$W" "$OUT"
cd "$W"
command -v fsck.erofs >/dev/null || sudo apt-get install -y erofs-utils
for P in vendor_a odm_a vendor_dlkm_a product_a; do
	echo "== $P"
	[ -f "$W/$P.img" ] || python3 "$D/a5-ios-port/tools/lp_extract.py" "$D/backup/super.bin" $P "$W/$P.img" | tail -1 || continue
	[ -s "$W/$P.img" ] || continue
	rm -rf "$W/x-$P"; mkdir -p "$W/x-$P"
	if [ "$(od -An -tx1 -j1024 -N4 "$W/$P.img" | tr -d ' ')" = "e2e1f5e0" ]; then
		fsck.erofs --extract="$W/x-$P" "$W/$P.img" >/dev/null 2>&1
	else
		debugfs -R "rdump / $W/x-$P" "$W/$P.img" >/dev/null 2>&1
	fi
	(cd "$W/x-$P" && find . -type f) > "$OUT/filelist-$P.txt"
	echo "   $(wc -l < "$OUT/filelist-$P.txt") files"
	cd "$W/x-$P"
	find . -type f -size -8M \( -iname '*frsm*' -o -iname '*fs15*' -o -iname '*fs1588*' -o -iname '*foursemi*' \
		-o -ipath '*audio_param*' -o -ipath '*audio_hw*' -o -iname '*codec_pga*' -o -iname '*audio_route*' \
		-o -iname '*audio_structure*' -o -iname '*dsp_vbc*' -o -iname '*vbc*' -o -iname '*smartamp*' \
		-o -iname '*spk*' -o -iname '*mixer_path*' -o -iname '*audio_para*' \) \
		-exec cp --parents {} "$OUT/" \; -print | sed "s|^\.|   $P|"
	cd "$W"
done
echo "== done"
