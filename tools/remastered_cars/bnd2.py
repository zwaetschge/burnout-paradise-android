#!/usr/bin/env python3
"""Minimal reader for Burnout Paradise 'bnd2' v2 PC bundles (private tool).
Entry (64 bytes): id u64, importHash u64, uncompressedSizeAndAlignment[3],
sizeAndAlignmentOnDisk[3], diskOffset[3], importOffset, resourceTypeId,
importCount u16, flags u8, streamIndex u8."""
import struct, sys, zlib
from collections import Counter

def read(path):
    d = open(path, 'rb').read()
    magic, ver, plat, dbg, count, ent_off, b0, b1, b2, flags = struct.unpack_from('<4sIIIIIIIII', d, 0)
    assert magic == b'bnd2'
    ents = []
    for i in range(count):
        o = ent_off + i * 64
        rid, ihash = struct.unpack_from('<QQ', d, o)
        unc = struct.unpack_from('<3I', d, o + 16)
        disk = struct.unpack_from('<3I', d, o + 28)
        off = struct.unpack_from('<3I', d, o + 40)
        imp_off, rtype, imp_cnt, eflags, stream = struct.unpack_from('<IIHBB', d, o + 52)
        ents.append(dict(id=rid, ihash=ihash, unc_full=list(unc), type=rtype, unc=[u & 0x0FFFFFFF for u in unc], disk=list(disk), off=list(off),
                         imp_off=imp_off, imp_cnt=imp_cnt, flags=eflags, stream=stream))
    return d, dict(ver=ver, plat=plat, count=count, ent_off=ent_off, blocks=(b0, b1, b2), flags=flags), ents

def chunk(d, hdr, e, k):
    """Bytes of chunk k of entry e (decompressed if the bundle is compressed)."""
    if not e['disk'][k]: return b''
    base = hdr['blocks'][k]
    raw = d[base + e['off'][k]: base + e['off'][k] + e['disk'][k]]
    if hdr['flags'] & 1: return zlib.decompress(raw)
    return raw

if __name__ == '__main__':
    d, hdr, ents = read(sys.argv[1])
    print(hdr)
    c = Counter(e['type'] for e in ents)
    print('types:', ', '.join(f'0x{t:X}x{n}' for t, n in sorted(c.items())))
    if len(sys.argv) > 2:
        for e in ents[:int(sys.argv[2])]:
            print(f"  id {e['id']:016X} type 0x{e['type']:X} unc {e['unc']} disk {e['disk']} imports {e['imp_cnt']}")
