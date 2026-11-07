#!/usr/bin/env python3
"""Print the a5update log read back from the phone's blackbox log area.

  spd_dump (FDL2 prompt):  read_part blackbox 268435456 16777216 a5log.bin
  a5log.py a5log.bin [OUT.txt]
"""
import struct, sys

b = open(sys.argv[1], 'rb').read()
if b[:8] != b'A5LOG001':
    raise SystemExit('no a5update log in this file (blackbox + 256 MiB)')
n = struct.unpack('<I', b[8:12])[0]
text = b[12:12 + n].decode('utf-8', 'replace')
if len(sys.argv) > 2:
    open(sys.argv[2], 'w').write(text)
    print(f'{len(text)} characters -> {sys.argv[2]}')
else:
    sys.stdout.write(text)
