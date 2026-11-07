#!/usr/bin/env python3
"""Build a blackbox update image for userland/a5update (fastboot flash blackbox OUT.img).

  mka5update.py SPEC OUT.img

SPEC is a text file, one operation per line ('#' comments; paths on the phone are absolute;
SRC is a file on this computer, relative to the SPEC's folder):
  FILE    SRC DEST [MODE UID GID]     default 0644 0 0 (0755 if SRC is executable)
  DIR     DEST [MODE UID GID]         default 0755 0 0
  RENAME  PATH NEWPATH
  REMOVE  PATH                        recursive
  SYMLINK PATH TARGET
  COLLECT PATH [KIB]                  copy a file / a directory's files into the log (default 256 KiB each)
  KILL    NAME [SIGNAL]               default 15; launchd restarts daemons
  LIST    PATH                        log a directory listing (every boot)
The serial is the build time, so every new image is applied once.
"""
import os, struct, sys, time, zlib

OPS = {'FILE': 1, 'DIR': 2, 'RENAME': 3, 'REMOVE': 4, 'SYMLINK': 5, 'COLLECT': 6, 'KILL': 7, 'LIST': 8}


def entry(op, path, data=b'', mode=0, uid=0, gid=0):
    p = path.encode()
    assert path.startswith('/') or op == 7, f'{path}: phone paths must be absolute'
    assert 0 < len(p) < 512
    e = struct.pack('<6I', op, mode, uid, gid, len(p), len(data)) + p + data
    return e + bytes(-len(e) % 4)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    spec, out = sys.argv[1], sys.argv[2]
    base = os.path.dirname(os.path.abspath(spec))
    entries = []
    for n, line in enumerate(open(spec), 1):
        w = line.split('#', 1)[0].split()
        if not w:
            continue
        op, a = w[0].upper(), w[1:]
        if op not in OPS:
            raise SystemExit(f'{spec}:{n}: unknown operation {w[0]}')
        if op == 'FILE':
            src = os.path.join(base, a[0])
            data = open(src, 'rb').read()
            mode = int(a[2], 8) if len(a) > 2 else (0o755 if os.access(src, os.X_OK) else 0o644)
            entries.append(entry(1, a[1], data, mode, int(a[3]) if len(a) > 3 else 0, int(a[4]) if len(a) > 4 else 0))
        elif op == 'DIR':
            entries.append(entry(2, a[0], b'', int(a[1], 8) if len(a) > 1 else 0o755,
                                 int(a[2]) if len(a) > 2 else 0, int(a[3]) if len(a) > 3 else 0))
        elif op in ('RENAME', 'SYMLINK'):
            entries.append(entry(OPS[op], a[0], a[1].encode()))
        elif op == 'REMOVE':
            entries.append(entry(4, a[0]))
        elif op == 'COLLECT':
            entries.append(entry(6, a[0], b'', int(a[1]) if len(a) > 1 else 0))
        elif op == 'LIST':
            entries.append(entry(8, a[0]))
        elif op == 'KILL':
            entries.append(entry(7, a[0], b'', int(a[1]) if len(a) > 1 else 15))
    payload = b''.join(entries)
    serial = int(time.time() * 1000)
    hdr = b'A5UPDT01' + struct.pack('<Q4I', serial, len(entries), len(payload), zlib.crc32(payload), 0)
    img = hdr + bytes(4096 - len(hdr)) + payload
    img += bytes(-len(img) % 4096)
    assert len(img) <= 256 << 20, 'update larger than the 256 MiB update area'
    open(out, 'wb').write(img)
    print(f'{out}: {len(entries)} entries, {len(img)} bytes, serial {serial}')


if __name__ == '__main__':
    main()
