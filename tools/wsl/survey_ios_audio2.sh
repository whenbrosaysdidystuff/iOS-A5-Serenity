#!/bin/bash
# Second audio survey: what VirtualAudio (the iOS routing layer, disabled since it crashed mediaserverd)
# needs from a physical output device. Mounts ~/ios-root/ios16-root.img READ-ONLY and writes only
# text (strings, plist contents) to Desktop\a5-ios-port\private\ios-audio\. No binaries are copied.
#   bash survey_ios_audio2.sh
set -uo pipefail
IMG=~/ios-root/ios16-root.img
MNT=/tmp/iosroot-ro
WIN=/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')/Desktop/a5-ios-port/private/ios-audio
mkdir -p "$WIN" "$MNT"
grep -qw hfsplus /proc/filesystems || sudo insmod ~/wsl-hfsplus/hfsplus.ko
mountpoint -q "$MNT" || sudo mount -t hfsplus -o ro,loop "$IMG" "$MNT" || exit 1
trap 'sudo umount "$MNT" 2>/dev/null || true' EXIT
cd "$MNT"

echo "1/4 VirtualAudio strings"
strings -n 4 Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin/VirtualAudio > "$WIN/va-strings.txt"
wc -l "$WIN/va-strings.txt"

echo "2/4 AppleAOPAudioPlugin + AppleUSBAudio + BTAudioHALPlugin strings (physical-device plug-ins)"
for p in AppleAOPAudioPlugin AppleUSBAudio BTAudioHALPlugin; do
	strings -n 4 System/Library/Audio/Plug-Ins/HAL/$p.driver/$p > "$WIN/$p-strings.txt"
	wc -l "$WIN/$p-strings.txt"
done

echo "3/4 feature flags and AOP plug-in config (plists as text)"
python3 - "$WIN/plists2.txt" <<'EOF'
import glob, plistlib, sys
out = open(sys.argv[1], 'w')
for f in sorted(glob.glob('System/Library/FeatureFlags/Domain/*Audio*.plist') +
                glob.glob('System/Library/FeatureFlags/Domain/MediaExperience*.plist') +
                glob.glob('System/Library/Audio/Plug-Ins/HAL/AppleAOPAudioPlugin.driver/*.plist') +
                glob.glob('System/Library/DeviceOMatic/*Audio*.plist')):
    try:
        out.write(f'\n## {f}\n{plistlib.load(open(f, "rb"))!r}\n')
    except Exception as e:
        out.write(f'\n## {f}: {e}\n')
EOF
wc -c "$WIN/plists2.txt"

echo "4/4 CoreAudio HAL / MediaExperience strings about ports and VAD in the shared cache (slow)"
: > "$WIN/cache-strings.txt"
for c in System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.[0-9]*; do
	strings -n 6 "$c" | grep -E 'VirtualAudio|vaem|VAD|PortType|kAudioPort|AudioPort|BuiltInSpeaker|Speaker|HALS_[A-Za-z]*Port|ptyp|vdef' \
		| head -4000 >> "$WIN/cache-strings.txt"
done
sort -u "$WIN/cache-strings.txt" -o "$WIN/cache-strings.txt"
wc -l "$WIN/cache-strings.txt"
echo "done -> $WIN"
