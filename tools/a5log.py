#!/usr/bin/env python3
"""Print the a5update log read back from the phone's blackbox log area, or cut the collected
files out of it.

  spd_dump (FDL2 prompt):  read_part blackbox 268435456 67108864 a5log.bin
  a5log.py a5log.bin                 print the text (binary data shown as replacement characters)
  a5log.py a5log.bin OUT.txt         write the text to OUT.txt
  a5log.py a5log.bin --extract DIR   write every collected file (last copy wins) under DIR/<phone path>
"""
import os, re, struct, sys

b = open(sys.argv[1], 'rb').read()
if b[:8] != b'A5LOG001':
    raise SystemExit('no a5update log in this file (blackbox + 256 MiB)')
n = struct.unpack('<I', b[8:12])[0]
body = b[12:12 + n]
if len(sys.argv) > 3 and sys.argv[2] == '--extract':
    out = sys.argv[3]
    pat = re.compile(rb'=== (/[^\n]*?) \((\d+) bytes, (\d+) from (\d+)\) ===\n')
    pos, count = 0, 0
    while True:
        m = pat.search(body, pos)
        if not m:
            break
        size = int(m.group(3))
        data = body[m.end():m.end() + size]
        dst = os.path.join(out, m.group(1).decode('utf-8', 'replace').lstrip('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        count += 1
        pos = m.end() + size
    print(f'{count} files -> {out}')
else:
    text = body.decode('utf-8', 'replace')
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w').write(text)
        print(f'{len(text)} characters -> {sys.argv[2]}')
    else:
        sys.stdout.write(text)
