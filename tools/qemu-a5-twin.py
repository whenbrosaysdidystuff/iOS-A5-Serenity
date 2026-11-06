# A5 ADT -> QEMU virt twin: QEMU GIC + 62.5 MHz timer, cpu reg = MPIDR Aff0 (QEMU) instead of Aff1.
import sys, struct
sys.path.insert(0, sys.argv[3])
import adt
root = adt.parse(open(sys.argv[1], 'rb').read())[0]
n = {'tb': 0, 'cpu': 0, 'gic': 0}
def name(nd):
    for k, f, v in nd.props:
        if k == 'name': return v.rstrip(b'\0').decode()
def walk(nd):
    nm = name(nd)
    for i, (k, f, v) in enumerate(nd.props):
        if len(v) == 4 and struct.unpack('<I', v)[0] == 26000000:
            nd.props[i] = (k, f, struct.pack('<I', 62500000)); n['tb'] += 1
        if nm and nm.startswith('cpu') and nm[3:].isdigit() and k == 'reg':
            nd.props[i] = (k, f, struct.pack('<I', int(nm[3:]))); n['cpu'] += 1
        if nm == 'gic' and k == 'reg':
            nd.props[i] = (k, f, struct.pack('<4Q', 0x08000000 - 0x01000000, 0x10000, 0x080a0000 - 0x01000000, 0xf60000)); n['gic'] += 1
    for c in nd.children: walk(c)
walk(root)
open(sys.argv[2], 'wb').write(adt.serialize(root))
print(n)
