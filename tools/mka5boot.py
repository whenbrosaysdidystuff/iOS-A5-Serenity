#!/usr/bin/env python3
"""Redmi A5: build the boot_b image = A5 shim + appended xnu payload, as an Android boot.img v4.

  mka5boot.py shim.elf Image [payload.bin] out.img

The A5's U-Boot (a) refuses `fastboot boot`, so the image is flashed to boot_b, and (b) takes the
ramdisk from init_boot/vendor_boot, so the payload rides inside the kernel slot instead: the shim
Image is padded to its _end (bss + stacks), aligned to 16 KiB, the payload (tools/mkpayload.py)
appended, and the Image header's res2/res3 set to the payload's offset/size. image_size covers
everything. Without a payload the image still boots and reports "no payload appended".
"""
import struct, subprocess, sys

PAGE, HDR_V4 = 4096, 4


def sym(elf, name):
    out = subprocess.run(['llvm-nm', elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f'{elf}: no symbol {name}')


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    elf, image, out = sys.argv[1], sys.argv[2], sys.argv[-1]
    payload = open(sys.argv[3], 'rb').read() if len(sys.argv) == 5 and sys.argv[3] else b''
    img = bytearray(open(image, 'rb').read())
    assert img[56:60] == b'ARM\x64', 'not an arm64 Image'
    end = sym(elf, '_end')
    assert len(img) <= end
    img += bytes(end - len(img))
    off = 0
    if payload:
        img += bytes(-len(img) % 0x4000)
        off = len(img)
        img += payload
    struct.pack_into('<Q', img, 0x10, len(img))           # image_size
    struct.pack_into('<QQ', img, 0x20, off, len(payload))  # res2, res3: payload offset, size
    hdr = b'ANDROID!' + struct.pack('<4I', len(img), 0, 0, 1584) + bytes(16) + struct.pack('<I', HDR_V4)
    hdr += bytes(1536) + struct.pack('<I', 0)
    assert len(hdr) == 1584
    pad = lambda b: bytes(b) + bytes(-len(b) % PAGE)
    data = pad(hdr) + pad(img)
    if len(data) > 64 << 20:
        raise SystemExit(f'{out}: {len(data)} bytes does not fit the 64 MiB boot_b partition')
    open(out, 'wb').write(data)
    print(f'{out}: {len(data)} bytes (shim {end:#x}, payload {len(payload)} at {off:#x})')


if __name__ == '__main__':
    main()
