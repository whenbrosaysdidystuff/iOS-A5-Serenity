#!/bin/bash
# Build A5Audio.driver (CoreAudio AudioServerPlugIn for iOS 16 arm64) on Linux with clang + ld64.lld.
# Headers: CoreAudio + CoreFoundation + usr/include from a macOS SDK (SDK=..., not in git);
# libraries: the hand-written .tbd stubs in stubs/ (only the symbols used). lld signs ad hoc.
#   SDK=/path/MacOSX11.3.sdk ./build.sh   -> out/A5Audio.driver
set -euo pipefail
cd "$(dirname "$0")"
: "${SDK:?set SDK to a MacOSX*.sdk}"
mkdir -p out/inc out/A5Audio.driver
for f in CoreAudio CoreFoundation CoreAudioTypes; do
	ln -sfn "$SDK/System/Library/Frameworks/$f.framework/Versions/A/Headers" out/inc/$f
done
clang -target arm64-apple-ios16.0 -O2 -Wall -Wextra -Wno-unused-parameter -fno-stack-protector \
	-isystem "$SDK/usr/include" -Iout/inc -c A5Audio.c -o out/A5Audio.o
LD=$(command -v ld64.lld || true)
[ -n "$LD" ] || { mkdir -p out/bin; ln -sf "$(command -v lld)" out/bin/ld64.lld; LD=out/bin/ld64.lld; }
"$LD" -arch arm64 -platform_version ios 16.0 16.0 -bundle -o out/A5Audio.driver/A5Audio \
	out/A5Audio.o stubs/libSystem.tbd stubs/CoreFoundation.tbd -dead_strip -adhoc_codesign
cp Info.plist out/A5Audio.driver/Info.plist
echo "out/A5Audio.driver: $(stat -c %s out/A5Audio.driver/A5Audio) bytes"
