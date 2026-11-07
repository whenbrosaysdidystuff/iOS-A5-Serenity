#!/bin/bash
# Build a5update (iOS 16 arm64 executable) on Linux: clang + ld64.lld, libc headers from a macOS SDK.
#   SDK=/path/MacOSX11.3.sdk ./build.sh   -> out/a5update
set -euo pipefail
cd "$(dirname "$0")"
: "${SDK:?set SDK to a MacOSX*.sdk}"
mkdir -p out
clang -target arm64-apple-ios16.0 -O2 -Wall -Wextra -fno-stack-protector -isystem "$SDK/usr/include" \
	-c a5update.c -o out/a5update.o
LD=$(command -v ld64.lld || true)
[ -n "$LD" ] || { mkdir -p out/bin; ln -sf "$(command -v lld)" out/bin/ld64.lld; LD=out/bin/ld64.lld; }
"$LD" -arch arm64 -platform_version ios 16.0 16.0 -execute -o out/a5update out/a5update.o libSystem.tbd \
	-dead_strip -adhoc_codesign
echo "out/a5update: $(stat -c %s out/a5update) bytes"
