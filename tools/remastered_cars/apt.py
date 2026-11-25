#!/usr/bin/env python3
"""Minimal reader for Burnout Paradise PC Apt movies (bundle resource 0x1E),
private tool. Layout (checked on the icon libraries): header u32[6]
{0x30, name0 off, apt data off, const file off, const data off, end}; all
offsets inside the "Apt Data" part are relative to the apt data start.
Characters per OpenSAGE (type, 0x09876543 signature, fields); lists are
(count, offset)."""
import struct
SIG = 0x09876543

class Apt:
    def __init__(self, blob):
        self.b = blob
        self.h = struct.unpack_from('<6I', blob, 0)
        self.base = self.h[2]
    def u32(self, off): return struct.unpack_from('<I', self.b, self.base + off)[0]
    def f32(self, off): return struct.unpack_from('<f', self.b, self.base + off)[0]
    def cstr(self, off):
        o = self.base + off; return self.b[o:self.b.index(b'\0', o)].decode('latin1')
    def lst(self, off):
        return self.u32(off), self.u32(off + 4)
    def movie_off(self):
        # constant file: "Apt constant file\x1a" then u32 movie offset (rel. to apt data)
        c = self.h[3]
        return struct.unpack_from('<I', self.b, c + 0x14)[0]
    def character(self, off):
        t, sig = self.u32(off), self.u32(off + 4)
        assert sig == SIG, hex(off)
        return t
    def frames(self, cnt, ptr):
        out = []
        for i in range(cnt):
            fo = ptr + 8 * i
            n, p = self.lst(fo)
            items = []
            for j in range(n):
                io = self.u32(p + 4 * j)
                items.append((io, self.u32(io)))
            out.append((fo, items))
        return out
