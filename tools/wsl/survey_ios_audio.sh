#!/bin/bash
# Survey how iOS 16 (iPhone X root image built by build-ios-root.sh) finds its audio output, so an
# A5 output device can be added. Mounts ~/ios-root/ios16-root.img READ-ONLY and writes only text
# listings (paths, plist contents, matching strings) to Desktop\a5-ios-port\private\ios-audio\.
# No Apple binaries are copied. Nothing here touches the phone.
#   bash survey_ios_audio.sh
set -euo pipefail
IMG=~/ios-root/ios16-root.img
MNT=/tmp/iosroot-ro
WIN=/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')/Desktop/a5-ios-port/private/ios-audio
mkdir -p "$WIN" "$MNT"
grep -qw hfsplus /proc/filesystems || sudo insmod ~/wsl-hfsplus/hfsplus.ko
mountpoint -q "$MNT" || sudo mount -t hfsplus -o ro,loop "$IMG" "$MNT"
trap 'sudo umount "$MNT" 2>/dev/null || true' EXIT
cd "$MNT"

echo "1/5 audio-related paths"
sudo find . -xdev \( -iname '*audio*' -o -iname '*coreaudio*' -o -iname '*mediaserverd*' -o -iname 'audiomxd*' \
	-o -ipath '*Plug-Ins/HAL*' -o -iname '*.driver' -o -iname '*AVHAL*' -o -iname '*AudioDSP*' \) \
	2>/dev/null | grep -v '/Caches/' | sort > "$WIN/paths.txt"
wc -l "$WIN/paths.txt"

echo "2/5 HAL plug-in bundles and audio launch daemons (plists as text)"
python3 - "$WIN/plists.txt" <<'EOF'
import os, plistlib, sys, glob
out = open(sys.argv[1], 'w')
pats = ['System/Library/Audio/Plug-Ins/**/Info.plist', 'System/Library/Audio/**/*.plist',
        'System/Library/LaunchDaemons/*audio*', 'System/Library/LaunchDaemons/*media*',
        'System/Library/LaunchDaemons/*Audio*', 'System/Library/LaunchDaemons/*Media*',
        'System/Cryptexes/*/System/Library/Audio/**/*.plist']
seen = set()
for p in pats:
    for f in glob.glob(p, recursive=True):
        if f in seen or os.path.getsize(f) > 400000:
            continue
        seen.add(f)
        try:
            d = plistlib.load(open(f, 'rb'))
        except Exception as e:
            out.write(f'\n## {f}: {e}\n'); continue
        out.write(f'\n## {f}\n{d!r}\n'[:20000])
print(len(seen), 'plists')
EOF

echo "3/5 IOKit classes and HAL keys the audio daemons and frameworks use"
: > "$WIN/strings.txt"
for b in usr/sbin/mediaserverd usr/libexec/audiomxd System/Library/Frameworks/CoreAudio.framework/CoreAudio \
	System/Library/PrivateFrameworks/AudioToolboxCore.framework/AudioToolboxCore \
	System/Library/Frameworks/AudioToolbox.framework/AudioToolbox \
	System/Library/PrivateFrameworks/CoreAudioOrchestration.framework/CoreAudioOrchestration; do
	for f in "$b" System/Cryptexes/OS/"$b"; do
		[[ -f $f ]] || continue
		echo "## $f ($(stat -c %s "$f") bytes)" >> "$WIN/strings.txt"
		strings -n 6 "$f" | grep -E 'IOService|IOAudio|AppleAudio|AppleHAL|Plug-Ins|\.driver|HALS_|VirtualAudio|AudioDSP|IOUserClient|io_service|AOP|kIOAudio' \
			| sort -u | head -400 >> "$WIN/strings.txt"
	done
done
wc -l "$WIN/strings.txt"

echo "4/5 dyld shared cache: is the audio code there instead?"
ls -la System/Library/Caches/com.apple.dyld/ System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/ 2>/dev/null \
	> "$WIN/dyld.txt" || true
for c in System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64* \
	System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*; do
	[[ -f $c ]] || continue
	strings -n 8 "$c" | grep -E '^/System/Library/(Audio|PrivateFrameworks/[A-Za-z]*Audio|Frameworks/[A-Za-z]*Audio)' \
		| sort -u | head -300 >> "$WIN/dyld.txt"
	strings -n 8 "$c" | grep -E 'AppleAudio[A-Za-z0-9]*|IOAudio2[A-Za-z]*|AppleHALPlugIn|VirtualAudio[A-Za-z]*' -o \
		| sort | uniq -c | sort -rn | head -200 >> "$WIN/dyld.txt"
done
wc -l "$WIN/dyld.txt"

echo "5/5 done -> $WIN"
ls -la "$WIN"
