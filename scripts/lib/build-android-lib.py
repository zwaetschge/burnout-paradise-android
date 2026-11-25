#!/usr/bin/env python3
"""ARM64 (Android, API 29) build of the static image libburnout_static.so:
lifted .text (from your BurnoutParadise.exe) + the runtime + the D3D9/D3DX/
DirectSound layers + Android audio/window glue. There is no JIT on the device:
every function has to be in the static lift or the dynamic-code pack.

  build-android-lib.py --gen DIR --dxvk-include DIR [--dyn FILE] [--out DIR] [-j N]

Environment: ANDROID_NDK_ROOT (r27).
"""
import argparse, os, subprocess, sys, time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import sources as s                                              # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--gen', type=Path, required=True, help='lifted C sources (lift stage output)')
ap.add_argument('--dxvk-include', type=Path, required=True, help='DXVK source include/ directory')
ap.add_argument('--dyn', type=Path, default=None, help='dynamic-code pack static_dyn.c (capture stage)')
ap.add_argument('--out', type=Path, default=ROOT / 'work' / 'android-lib')
ap.add_argument('-j', type=int, default=os.cpu_count() or 8)
ap.add_argument('--opt', default='-O2')
args = ap.parse_args()

NDK = Path(os.environ.get('ANDROID_NDK_ROOT', '/opt/android-ndk')) / 'toolchains/llvm/prebuilt/linux-x86_64'
CC, CXX = NDK / 'bin/aarch64-linux-android29-clang', NDK / 'bin/aarch64-linux-android29-clang++'
GEN, OUT = args.gen.resolve(), args.out.resolve()
(OUT / 'obj').mkdir(parents=True, exist_ok=True)
INC = ['-I', str(ROOT / 'runtime/recomp_override'), '-I', str(ROOT / 'runtime'), '-I', str(s.RECOMP32), '-I', str(GEN)]
DX = args.dxvk_include.resolve()
CXX_INC = ['-I', str(DX / 'native/windows'), '-I', str(DX / 'native'), '-I', str(DX / 'native/directx'), '-I', str(ROOT / 'runtime')]
lifted = [p for p in sorted(GEN.glob('recomp_*.c')) if not p.name.startswith(('recomp_dispatch', 'recomp_unsupported'))]
lifted += [GEN / 'recomp_dispatch.c', GEN / 'recomp_unsupported.c']
runtime = [ROOT / 'runtime' / x for x in s.RUNTIME]
runtime += [ROOT / 'runtime' / n for n in s.OWN if n != 'static_host_main.c']
runtime += [ROOT / 'runtime' / n for n in s.ANDROID_EXTRA]
if args.dyn: runtime.append(args.dyn.resolve())
cxx = [ROOT / 'runtime' / n for n in s.CXX]
defs = ['-D' + d for d in s.DEFINES]
COMMON = ['-fPIC', '-fno-strict-aliasing', '-w', '-fvisibility=default']
# The guest CPU state pointer lives in x28 for lifted code (recomp_types.h,
# RECOMP_CPU_REG); no C file of the image may allocate x28.
CREG = ['-ffixed-x28', '-DRECOMP_CPU_REG']
lifted_set = set(lifted)


def one(src):
    obj = OUT / 'obj' / (src.stem + '.o')
    deps = [src] + ([ROOT / 'runtime/d3d9_thunk_gen.inc', ROOT / 'runtime/d3d9_thunk.h'] if src.suffix == '.cpp' else [])
    if obj.exists() and all(obj.stat().st_mtime > d.stat().st_mtime for d in deps): return 0, obj, ''
    if src.suffix == '.cpp': cmd = [str(CXX), args.opt, '-std=c++17', *COMMON, *CXX_INC, '-c', str(src), '-o', str(obj)]
    else: cmd = [str(CC), args.opt if src in lifted_set else '-O2', '-std=gnu11', *COMMON, *CREG, *INC, *defs, '-c', str(src), '-o', str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return (0, obj, '') if r.returncode == 0 else (1, obj, r.stderr[-1500:])


srcs = lifted + runtime + cxx
t = time.time(); objs = []; bad = []; n = 0
print(f'compiling {len(srcs)} TUs for arm64 ({args.opt}, {args.j} jobs)', flush=True)
with ThreadPoolExecutor(args.j) as pool:
    for rc, obj, err in pool.map(one, srcs):
        n += 1; objs.append(obj)
        if rc: bad.append(obj.name); print('FAIL', obj.name, err[-700:], flush=True)
        if n % 100 == 0: print(f'{n}/{len(srcs)} {time.time() - t:.0f}s fails={len(bad)}', flush=True)
if bad: print('BUILD_FAILED', len(bad), bad[:10]); sys.exit(1)
so = OUT / 'libburnout_static.so'
(OUT / 'link.rsp').write_text('\n'.join(str(o) for o in objs))
r = subprocess.run([str(CXX), '-shared', '-o', str(so), f'@{OUT / "link.rsp"}', '-Wl,-z,max-page-size=16384',
                    '-Wl,-z,common-page-size=16384', '-Wl,--no-undefined', '-Wl,-Bsymbolic-functions',
                    '-static-libstdc++', '-llog', '-landroid', '-laaudio', '-lm', '-ldl'], capture_output=True, text=True)
if r.returncode: print('LINK_FAILED', r.stderr[-4000:]); sys.exit(1)
print(f'LINKED {so} {so.stat().st_size} bytes in {time.time() - t:.0f}s', flush=True)
