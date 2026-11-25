#!/usr/bin/env python3
"""Hot-discovery lifter for the FAST engine (development host only).

runtime/static_hot.c pipes a guest VA here whenever the trampoline dispatches
to an address that static-full-v2 never lifted as a function (an indirect
`.trace` target).  For each VA we lift exactly that one function with the same
generator as tools/static_full_lift.py, run the trampoline pass, and -- so the
shard needs no other `sub_<VA>` at link time -- rewrite every *direct* call
`RECOMP_CALL_R(ret, sub_X)` into the VA-dispatched form
`RECOMP_ICALL_R(ret, 0xX)`.  Every control transfer then bounces back through
port_dispatch_next, which hot-discovers the next function in turn.

Protocol (one line each way on stdin/stdout):
    request:  0x%08X            the VA to lift
    reply:    OK <so> <name> <n>   shard built, dlsym `name`, n addresses
              FAIL <reason>        lifted nothing / compile error -> named stop

Every VA handled is appended to the run's seed file so one later static re-lift
of static-full-v2 can bake the whole set in ahead-of-time (the shipped image
must not depend on this process).  Nothing is patched in the game image.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import static_full_lift as sfl            # noqa: E402
import static_trampoline_pass as stp      # noqa: E402
import capstone                           # noqa: E402

import os
EXE = Path(os.environ['PORT_HOT_EXE'])
GEN = Path(os.environ['PORT_HOT_GEN'])
OUT = Path(os.environ['PORT_HOT_OUT'])
OUT.mkdir(parents=True, exist_ok=True)
SEEDS = OUT / 'hot-seeds.txt'

# Same WITH_* macros and include roots the fast engine is built with.
import sources as shr             # noqa: E402
INCLUDES = ['-I', str(ROOT / 'runtime/recomp_override'), '-I', str(ROOT / 'runtime'),
            '-I', str(shr.RECOMP32), '-I', str(GEN)]
DEFINES = ['-D' + d for d in shr.DEFINES]

# One direct call -> VA dispatch.  The trampoline pass emits, e.g.
#   RECOMP_CALL_R(0x00401005u, sub_015F4510); /* 0x00401000: call .. */
CALL_R = re.compile(r'RECOMP_CALL_R\((0x[0-9A-Fa-f]+u?),\s*sub_([0-9A-Fa-f]{8})\)')


def selfcontain(text):
    """Make the shard independent of other sub_<VA> symbols."""
    text, n = CALL_R.subn(
        lambda m: f'RECOMP_ICALL_R({m.group(1)}, 0x{m.group(2)}u)', text)
    # Any surviving bare sub_<VA> reference (address-of in a jump table, say)
    # would be an undefined symbol; surface it rather than ship a broken shard.
    leftover = sorted(set(re.findall(r'\bsub_([0-9A-Fa-f]{8})\b', text))
                      - {cur[0]})
    return text, n, leftover


class Lift:
    def __init__(self):
        self.gen, self.lift32, analyze_pe, build_iat_map = sfl.load_upstream()
        _secs = os.environ.get('PORT_HOT_SECTIONS')
        if _secs:
            sfl.LIFT_SECTIONS = tuple(x for x in _secs.split(',') if x)
            self.img = sfl.Image(str(EXE), sfl.LIFT_SECTIONS)
        else:
            self.img = sfl.Image(str(EXE), sfl.LIFT_SECTIONS + (sfl.TRACE_SECTION,))
        self.iat_map = build_iat_map(analyze_pe(str(EXE)))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        # Every known function entry bounds a hot function's extent so it stops
        # where the next function begins instead of running into it.
        fns = json.load(open(GEN / 'functions.json'))
        fl = fns['functions'] if isinstance(fns, dict) else fns

        def fva(f):
            v = f['va'] if isinstance(f, dict) else f[0]
            return int(v, 16) if isinstance(v, str) else v
        self.entries = {fva(f) for f in fl}
        self.lifter = self.lift32.Lifter(iat_map=self.iat_map, lifted=self.entries)
        self.serial = 0

    def one(self, va):
        global cur
        cur = (f'{va:08X}',)
        if self.img.region_of(va) is None and (self.img.base <= va < self.img.base + len(self.img.buf)):
            return None, f'not-in-lift-region 0x{va:08X}'
        if not (self.img.base <= va < self.img.base + len(self.img.buf)):
            # Not in the game image at all. The proven execution never computes
            # such a target in the startup prefix: this is a divergence of the
            # fast engine's state, not a liftable function. Report, do not guess.
            return None, f'out-of-image target 0x{va:08X} (fast-engine divergence)'
        stats = sfl.Stats()
        boundary = set(self.entries)
        boundary.add(va)
        text, _called, info = sfl.lift_one(self.gen, self.lift32, self.md, self.img,
                                           self.lifter, va, boundary, stats)
        if text is None:
            return None, f'lift_one: {info}'
        text, _tst = stp.transform(text, stp.Sizer(self.img.base, self.img.buf))
        if text.startswith(stp.MARK):
            text = text[len(stp.MARK):]
        text, _n, leftover = selfcontain(text)
        if leftover:
            return None, f'unresolved sub_ refs: {leftover[:4]}'
        self.serial += 1
        name = f'sub_{va:08X}'
        # recomp_funcs.h (14 MB of prototypes) is not needed: the shard refers
        # to no other sub_<VA>. Shards are cached by content, so a lifter fix
        # only recompiles the functions whose translation actually changed.
        body = ('#define RECOMP_GENERATED_CODE\n'
                '#include "recomp_types.h"\n'
                '#include "recomp_unsupported.h"\n#include <math.h>\n#include <string.h>\n\n'
                f'void {name}(void);\n' + text + '\n')
        digest = hashlib.sha1(body.encode()).hexdigest()[:16]
        src = OUT / f'hot_{va:08X}_{digest}.c'
        so = src.with_suffix('.so')
        if so.exists():
            return (so, name), None
        src.write_text(body)
        r = subprocess.run(
            [os.environ.get('UB_HOST_CC', 'zig-cc'), '-O1', '-std=gnu11', '-w', '-fno-strict-aliasing', '-mllvm', '-fixup-byte-word-insts=0',
             '-fPIC', '-shared', *INCLUDES, *DEFINES, str(src), '-o', str(so)],
            capture_output=True, text=True)
        if r.returncode:
            return None, 'cc: ' + r.stderr.strip().replace('\n', ' ')[-240:]
        return (so, name), None


class MemImage:
    """A window of guest memory as a lift image: code the game loaded into its
    heap at run time (AEMS sound banks carry relocated x86 code)."""

    def __init__(self, base, data):
        self.base = base
        self.code_start = base
        self.buf = bytearray(data)
        self.regions = [{'name': 'dyn', 'start': base, 'end': base + len(data)}]
        self.trace = None

    def region_of(self, va):
        r = self.regions[0]
        return r if r['start'] <= va < r['end'] else None

    def in_trace(self, va):
        return False


DYN = OUT / 'dyn'


def lift_dynamic(lift, va, base, path):
    """Lift one function out of a guest-memory window. Returns
    ((so, name, lo, hi), None) or (None, reason)."""
    global cur
    cur = (f'{va:08X}',)
    data = Path(path).read_bytes()
    img = MemImage(base, data)
    if img.region_of(va) is None:
        return None, f'dynamic target 0x{va:08X} outside the window'
    stats = sfl.Stats()
    stats.covered = {}
    lifter = lift.lift32.Lifter(iat_map=lift.iat_map, lifted=set())
    text, _called, info = sfl.lift_one(lift.gen, lift.lift32, lift.md, img, lifter, va, {va}, stats)
    if text is None:
        return None, f'lift_one(dynamic): {info}'
    if 'recomp_unsupported(' in text:
        return None, 'dynamic code contains an untranslatable instruction (not code?)'
    text, _tst = stp.transform(text, stp.Sizer(img.base, img.buf))
    if text.startswith(stp.MARK):
        text = text[len(stp.MARK):]
    text, _n, leftover = selfcontain(text)
    if leftover:
        return None, f'unresolved sub_ refs: {leftover[:4]}'
    addrs = sorted(int(a, 16) for a in re.findall(r'/\* 0x([0-9A-Fa-f]{8}): ', text))
    if not addrs:
        return None, 'dynamic lift produced no instructions'
    lo, hi = addrs[0], min(addrs[-1] + 16, base + len(data))
    code = bytes(data[lo - base:hi - base])
    name = f'sub_{va:08X}'
    body = ('#define RECOMP_GENERATED_CODE\n'
            '#include "recomp_types.h"\n'
            '#include "recomp_unsupported.h"\n#include <math.h>\n#include <string.h>\n\n'
            f'void {name}(void);\n' + text + '\n')
    digest = hashlib.sha1(body.encode() + code).hexdigest()[:16]
    DYN.mkdir(parents=True, exist_ok=True)
    src = DYN / f'dyn_{va:08X}_{digest}.c'
    so = src.with_suffix('.so')
    if not so.exists():
        src.write_text(body)
        src.with_suffix('.bin').write_bytes(code)
        src.with_suffix('.json').write_text(json.dumps({'va': va, 'lo': lo, 'hi': hi, 'sha256': hashlib.sha256(code).hexdigest(),
                                                        'instructions': len(addrs), 'source': src.name}))
        r = subprocess.run(
            [os.environ.get('UB_HOST_CC', 'zig-cc'), '-O1', '-std=gnu11', '-w', '-fno-strict-aliasing', '-mllvm', '-fixup-byte-word-insts=0',
             '-fPIC', '-shared', *INCLUDES, *DEFINES, str(src), '-o', str(so)],
            capture_output=True, text=True)
        if r.returncode:
            return None, 'cc: ' + r.stderr.strip().replace('\n', ' ')[-240:]
    return (so, name, lo, hi), None


cur = ('00000000',)


def main():
    lift = Lift()
    seeds = open(SEEDS, 'a')
    sys.stdout.write('READY\n')
    sys.stdout.flush()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if line.startswith('DYN '):
            # DYN <va> <window base> <window file>: code in guest memory
            try:
                _, a, b, path = line.split(' ', 3)
                res, err = lift_dynamic(lift, int(a, 16), int(b, 16), path)
            except Exception as exc:  # noqa: BLE001
                res, err = None, f'{type(exc).__name__}: {exc}'
            if res is None:
                sys.stdout.write(f'FAIL {err}\n')
            else:
                sys.stdout.write(f'OK {res[0]} {res[1]} 1 {res[2]:08X} {res[3]:08X}\n')
            sys.stdout.flush()
            continue
        try:
            va = int(line, 16) if line.startswith('0x') else int(line, 16)
        except ValueError:
            sys.stdout.write('FAIL bad-request\n')
            sys.stdout.flush()
            continue
        try:
            res, err = lift.one(va)
        except Exception as exc:  # noqa: BLE001
            res, err = None, f'{type(exc).__name__}: {exc}'
        if res is None:
            sys.stdout.write(f'FAIL {err}\n')
        else:
            so, name = res
            seeds.write(f'{va:08X}\n')
            seeds.flush()
            sys.stdout.write(f'OK {so} {name} 1\n')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
