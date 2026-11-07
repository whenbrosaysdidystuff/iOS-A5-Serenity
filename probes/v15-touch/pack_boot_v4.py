#!/usr/bin/env python3
"""Wrap a raw arm64 Image into an Android boot.img header v4 (no ramdisk, no signature).
Usage: pack_boot_v4.py Image OUT.img [pagesize]"""
import struct, sys
kernel = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
page = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
hdr = b'ANDROID!'
hdr += struct.pack('<4I', len(kernel), 0, 0, 1584)   # kernel_size, ramdisk_size, os_version, header_size
hdr += bytes(16)                                      # reserved
hdr += struct.pack('<I', 4)                           # header_version
hdr += bytes(1536)                                    # cmdline (empty, like the stock image)
hdr += struct.pack('<I', 0)                           # signature_size
assert len(hdr) == 1584
pad = lambda b: b + bytes(-len(b) % page)
open(out, 'wb').write(pad(hdr) + pad(kernel))
print(f'{out}: {len(pad(hdr)) + len(pad(kernel))} bytes, kernel {len(kernel)}')
