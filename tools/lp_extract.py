#!/usr/bin/env python3
"""Copy one logical partition (default: vendor_a) out of an Android dynamic-partition super image.
Reads only the LP metadata and the partition's extents; the super image is not modified.
Usage: python3 lp_extract.py SUPER.bin [PARTITION] [OUT]"""
import struct, sys, os

src = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else 'vendor_a'
out = sys.argv[3] if len(sys.argv) > 3 else want + '.img'

with open(src, 'rb') as f:
    f.seek(4096)
    g = f.read(4096)
    magic, struct_size = struct.unpack_from('<II', g, 0)
    if magic != 0x616c4467:
        sys.exit('no LP geometry at 4096 (magic %08x) - is this a raw (non-sparse) super image?' % magic)
    max_size, slots, lbs = struct.unpack_from('<III', g, 40)
    f.seek(4096 + 2 * 4096)          # primary metadata, slot 0
    h = f.read(max_size)
    m, major, minor, hsize = struct.unpack_from('<IHHI', h, 0)
    if m != 0x414C5030:
        sys.exit('bad metadata magic %08x' % m)
    tables_size = struct.unpack_from('<I', h, 44)[0]
    def desc(i):
        return struct.unpack_from('<III', h, 80 + 12 * i)
    p_off, p_n, p_sz = desc(0)
    e_off, e_n, e_sz = desc(1)
    t = h[hsize:hsize + tables_size]
    parts = {}
    for i in range(p_n):
        b = t[p_off + i * p_sz: p_off + (i + 1) * p_sz]
        name = b[:36].split(b'\0')[0].decode()
        attr, first, num, grp = struct.unpack_from('<IIII', b, 36)
        parts[name] = (first, num)
    print('partitions:', ', '.join('%s(%d)' % (k, v[1]) for k, v in parts.items()))
    if want not in parts:
        sys.exit('partition %s not found' % want)
    first, num = parts[want]
    total = 0
    with open(out, 'wb') as o:
        for i in range(first, first + num):
            b = t[e_off + i * e_sz: e_off + (i + 1) * e_sz]
            nsec, ttype, tdata, tsrc = struct.unpack_from('<QIQI', b, 0)
            if ttype == 0:
                f.seek(tdata * 512)
                left = nsec * 512
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    if not chunk:
                        sys.exit('short read')
                    o.write(chunk)
                    left -= len(chunk)
            else:
                o.write(bytes(nsec * 512))
            total += nsec * 512
    print('wrote %s: %d bytes (%.1f MB)' % (out, total, total / 1e6))
