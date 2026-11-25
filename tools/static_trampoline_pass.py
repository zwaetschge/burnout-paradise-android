#!/usr/bin/env python3
"""Rewrite generated recomp32 C to the port's trampoline call/ret protocol.

See the "trampolined control transfer" block in
runtime/recomp_override/recomp_types.h for the protocol.  This pass only
rewrites the *shape* of call/ret lines the upstream lifter emitted:

  RECOMP_CALL(f); /* 0xA: call .. */      -> RECOMP_CALL_R(0x<A+len>u, f); ..
  RECOMP_ICALL(x); /* 0xA: call .. */     -> RECOMP_ICALL_R(0x<A+len>u, x); ..
  RECOMP_CALL(f); .. goto L_R; /* push R; jmp f */ -> RECOMP_CALL_R(0x<R>u, f); ..
  RECOMP_FLAGS_OUT(); esp += N; {..return;}  (ret)  -> g_next = MEM32(esp) first
  "end of function" / far-return returns  -> named stops

The return address of a call is the address of the next instruction; its
length is decoded from the image bytes at the call's address (or from a
caller-supplied byte source for run-time overlays).  The pass is idempotent
(marker comment) and reports any call line it could not rewrite; such a line
is a named stop at run time (RECOMP_CALL/RECOMP_ICALL in the override header).
"""
import argparse
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

MARK = '/* trampoline-pass v1 */\n'
CALL_RE = re.compile(r'(RECOMP_I?CALL)\((.*)\); (/\* 0x([0-9A-Fa-f]{8}): call )')
PUSHJMP_RE = re.compile(r'(RECOMP_I?CALL)\((.*?)\); (RECOMP_FLAGS_IN\(\); goto L_([0-9A-Fa-f]{8}); /\* 0x[0-9A-Fa-f]{8}: push )')
RET_RE = re.compile(r'RECOMP_FLAGS_OUT\(\); esp \+= (\d+); \{')
END_RE = re.compile(r'\{ (RECOMP_EH_LEAVE\(\); )?RECOMP_REGS_OUT\(\); return; \} /\* (end of function|far return) \*/')
LEFT_RE = re.compile(r'RECOMP_I?CALL\(')

_IMG = None


def _load_image(exe):
    import pefile
    pe = pefile.PE(str(exe))
    base = pe.OPTIONAL_HEADER.ImageBase
    data = Path(exe).read_bytes()
    end = max(s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) for s in pe.sections)
    buf = bytearray(end)
    for s in pe.sections:
        raw = data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
        n = min(len(raw), end - s.VirtualAddress)
        buf[s.VirtualAddress:s.VirtualAddress + n] = raw[:n]
    return base, bytes(buf)


class Sizer:
    """Instruction length at a guest VA, from an image buffer."""

    def __init__(self, base, buf):
        import capstone
        self.base, self.buf = base, buf
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def next_va(self, va):
        off = va - self.base
        for insn in self.md.disasm_lite(bytes(self.buf[off:off + 16]), va, 1):
            if insn[2].split()[-1] != 'call' and insn[2] != 'lcall':
                raise ValueError(f'0x{va:08X} is {insn[2]!r}, not a call')
            return va + insn[1]
        raise ValueError(f'cannot decode call at 0x{va:08X}')


def transform(text, sizer):
    """Return (new_text, stats)."""
    if text.startswith(MARK):
        return text, {'already': 1}
    st = {'call': 0, 'pushjmp': 0, 'ret': 0, 'end': 0, 'left': 0, 'bad': []}

    def pj(m):
        st['pushjmp'] += 1
        return f'{m.group(1)}_R(0x{m.group(4).upper()}u, {m.group(2)}); {m.group(3)}'
    text = PUSHJMP_RE.sub(pj, text)

    def call(m):
        try:
            r = sizer.next_va(int(m.group(4), 16))
        except ValueError as exc:
            st['bad'].append(str(exc))
            return m.group(0)
        st['call'] += 1
        return f'{m.group(1)}_R(0x{r:08X}u, {m.group(2)}); {m.group(3)}'
    text = CALL_RE.sub(call, text)

    def ret(m):
        st['ret'] += 1
        return f'RECOMP_FLAGS_OUT(); g_next = MEM32(esp); esp += {m.group(1)}; {{'
    text = RET_RE.sub(ret, text)

    def end(m):
        st['end'] += 1
        kind = 'end-of-function' if m.group(2) == 'end of function' else 'far-return'
        return '{ %sRECOMP_REGS_OUT(); port_named_stop("%s", g_cur_func); } /* %s */' % (
            m.group(1) or '', kind, m.group(2))
    text = END_RE.sub(end, text)
    st['left'] = len(LEFT_RE.findall(text))
    return MARK + text, st


def _init(exe):
    global _IMG
    _IMG = Sizer(*_load_image(exe))


def _one(path):
    p = Path(path)
    text = p.read_text()
    new, st = transform(text, _IMG)
    if 'already' not in st:
        tmp = p.with_suffix('.c.tmp')
        tmp.write_text(new)
        tmp.replace(p)
    st['file'] = p.name
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gen_dir')
    ap.add_argument('--exe', required=True)
    ap.add_argument('--jobs', type=int, default=6)
    args = ap.parse_args()
    files = sorted(str(p) for p in Path(args.gen_dir).glob('recomp_*.c')
                   if not p.name.startswith(('recomp_dispatch', 'recomp_unsupported', 'recomp_stubs')))
    tot = {'call': 0, 'pushjmp': 0, 'ret': 0, 'end': 0, 'left': 0, 'already': 0}
    bad = []
    with ProcessPoolExecutor(args.jobs, initializer=_init, initargs=(args.exe,)) as pool:
        for st in pool.map(_one, files, chunksize=8):
            for k in tot:
                tot[k] += st.get(k, 0)
            bad += [f"{st['file']}: {b}" for b in st.get('bad', [])]
            if st.get('left'):
                bad.append(f"{st['file']}: {st['left']} untransformed call lines")
    print(f'files={len(files)} {tot}')
    for b in bad[:40]:
        print('NOTE', b)
    print(f'notes={len(bad)}')


if __name__ == '__main__':
    sys.exit(main())
