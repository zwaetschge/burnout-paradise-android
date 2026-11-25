#!/usr/bin/env python3
"""x86-64 Linux build of the same static image for the headless capture run
(build host only). Unlike the device build it carries the hot loader: code the
game loads at run time (from its sound bundles) is lifted on the fly, and the
capture stage packs everything that was found for the device.

  build-host.py --gen DIR --dxvk-include DIR [--dyn FILE] [--out DIR] [-j N]

Compiler: Zig (python3 -m ziglang). The LLVM byte/word fixup pass is disabled
(-mllvm -fixup-byte-word-insts=0): it miscompiles some lifted functions.
"""
import argparse, os, subprocess, sys, time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import sources as s                                              # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--gen', type=Path, required=True)
ap.add_argument('--dxvk-include', type=Path, required=True)
ap.add_argument('--dyn', type=Path, default=None)
ap.add_argument('--out', type=Path, default=ROOT / 'work' / 'host')
ap.add_argument('-j', type=int, default=os.cpu_count() or 8)
args = ap.parse_args()

GEN, OUT = args.gen.resolve(), args.out.resolve()
(OUT / 'obj').mkdir(parents=True, exist_ok=True)
ZIG = [sys.executable, '-m', 'ziglang']
INC = ['-I', str(ROOT / 'runtime/recomp_override'), '-I', str(ROOT / 'runtime'), '-I', str(s.RECOMP32), '-I', str(GEN)]
DX = args.dxvk_include.resolve()
CXX_INC = ['-I', str(DX / 'native/headless'), '-I', str(DX / 'native/windows'), '-I', str(DX / 'native'),
           '-I', str(DX / 'native/directx'), '-I', str(ROOT / 'runtime')]
srcs = [p for p in sorted(GEN.glob('recomp_*.c')) if not p.name.startswith(('recomp_dispatch', 'recomp_unsupported'))]
srcs += [GEN / 'recomp_dispatch.c', GEN / 'recomp_unsupported.c']
srcs += [ROOT / 'runtime' / x for x in s.RUNTIME] + [ROOT / 'runtime' / n for n in s.OWN]
srcs += [ROOT / 'runtime' / n for n in s.HOST_EXTRA] + [ROOT / 'runtime' / n for n in s.CXX]
if args.dyn: srcs.append(args.dyn.resolve())
defs = ['-D' + d for d in s.DEFINES]


def one(src):
    obj = OUT / 'obj' / (src.stem + '.o')
    deps = [src] + ([ROOT / 'runtime/d3d9_thunk_gen.inc', ROOT / 'runtime/d3d9_thunk.h'] if src.suffix == '.cpp' else [])
    if obj.exists() and all(obj.stat().st_mtime > d.stat().st_mtime for d in deps): return 0, obj, ''
    if src.suffix == '.cpp':
        cmd = [*ZIG, 'c++', '-O1', '-std=c++17', '-w', *CXX_INC, '-c', str(src), '-o', str(obj)]
    else:
        cmd = [*ZIG, 'cc', '-O1', '-std=gnu11', '-w', '-fno-strict-aliasing', '-mllvm', '-fixup-byte-word-insts=0',
               *INC, *defs, '-c', str(src), '-o', str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return (0, obj, '') if r.returncode == 0 else (1, obj, r.stderr[-1500:])


t = time.time(); objs = []; bad = []; n = 0
print(f'compiling {len(srcs)} TUs for the host capture build ({args.j} jobs)', flush=True)
with ThreadPoolExecutor(args.j) as pool:
    for rc, obj, err in pool.map(one, srcs):
        n += 1; objs.append(obj)
        if rc: bad.append(obj.name); print('FAIL', obj.name, err[-500:], flush=True)
        if n % 200 == 0: print(f'{n}/{len(srcs)} {time.time() - t:.0f}s fails={len(bad)}', flush=True)
if bad: print('BUILD_FAILED', len(bad)); sys.exit(1)
exe = OUT / 'ub_host'
(OUT / 'link.rsp').write_text('\n'.join(str(o) for o in objs))
r = subprocess.run([*ZIG, 'c++', '-O1', '-rdynamic', '-o', str(exe), f'@{OUT / "link.rsp"}', '-lm', '-lpthread', '-ldl'],
                   capture_output=True, text=True)
if r.returncode: print('LINK_FAILED', r.stderr[-3000:]); sys.exit(1)
print(f'LINKED {exe} {exe.stat().st_size} bytes in {time.time() - t:.0f}s', flush=True)
