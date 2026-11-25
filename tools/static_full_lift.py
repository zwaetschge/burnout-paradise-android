#!/usr/bin/env python3
"""Static whole-image lift of the recovered BurnoutPR PE32 image to C.

This is a thin driver around the upstream pcrecomp lifter
(``port/upstream/pcrecomp/tools/lift/generate.py``): the disassembly strategy
(``true_extent``, ``linear_disassemble_function``), the per-function emitter
(``lift_function_linear``) and the instruction lifter (``lift32.Lifter``) are
imported and called, not re-implemented.  What this file adds is the project
specific policy that generate.py's own ``main()`` does not have:

  * several code regions instead of one ``.text`` section (the recovered image
    has ``.code``, ``.srdata``, ``.text1`` and ``.ooa``; ``.trace`` -- the large
    protected region -- is deliberately NOT lifted, only measured),
  * function discovery from the PE entry point, the export directory, call and
    tail-jump targets, prologues and (absent here) relocation targets,
  * an honest trap for every instruction the lifter cannot translate instead
    of the upstream ``; /* UNIMPLEMENTED */`` null statement,
  * a second discovery round over indirect tail/call targets that the first
    round lifted code refers to but never catalogued,
  * sharded output plus ``functions.json`` / ``dispatch`` / ``stats.json`` /
    ``trace_edges.json``.

``--v2`` (additive; the v1 path above is untouched) lifts ``.trace`` as a fifth
region, discovers functions through data-section pointers, vtables and code
immediates to a fixpoint, seeds from private execution traces (``--witness``),
lifts in 6 parallel processes, turns ``clflush`` into a memory no-op barrier and
emits a *named trap* (never a silent stub) for every instruction it cannot
translate -- or for a whole function whose bytes do not decode to sane x86.

Nothing here executes or emulates guest code; the output is C source only.
"""

import argparse
import os
import bisect
import json
import re
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM = Path(os.environ.get('PCRECOMP_DIR') or ROOT / 'third_party' / 'pcrecomp')
DEFAULT_EXE = Path(os.environ['UB_EXE']) if os.environ.get('UB_EXE') else None
DEFAULT_OUT = ROOT / 'port' / 'generated' / 'static-full-v1'

# Sections that hold x86 this port translates.  ``.trace`` (113 MB, the
# protected region) is excluded on purpose: it is measured, not lifted.
LIFT_SECTIONS = ('.code', '.srdata', '.text1', '.ooa')
TRACE_SECTION = '.trace'

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_CNT_CODE = 0x00000020

EXTENT_REACH = 0x40000          # generate.EXTENT_REACH, kept in sync by assert
SHARD = 500                     # functions per translation unit
MAX_ROUNDS = 12                 # discovery rounds over indirect targets


def log(msg):
    print(msg, flush=True)


def load_upstream():
    """Import generate/lift32/pe_analyze from the vendored upstream tree."""
    for sub in ('lift', 'disasm', 'pe'):
        p = str(UPSTREAM / 'tools' / sub)
        if p not in sys.path:
            sys.path.insert(0, p)
    import generate
    # generate.find_entries() unpacks a rel32 with struct but never imports it;
    # injecting the module is the smallest wrapper fix that leaves the vendored
    # file untouched.
    generate.struct = struct
    import lift32
    from pe_analyze import analyze_pe, build_iat_map
    return generate, lift32, analyze_pe, build_iat_map


class Image:
    """The PE laid out at its virtual addresses, plus the region table."""

    def __init__(self, exe, lift_sections=LIFT_SECTIONS):
        import pefile
        self.path = Path(exe)
        pe = pefile.PE(str(self.path))
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        data = self.path.read_bytes()
        self.file_size = len(data)
        self.entry_va = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint

        end = 0
        for s in pe.sections:
            end = max(end, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
        self.buf = bytearray(end)          # RVA indexed: buf[rva]
        self.sections = {}
        for s in pe.sections:
            name = s.Name.rstrip(b'\x00').decode('ascii', 'replace')
            raw = data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
            n = min(len(raw), len(self.buf) - s.VirtualAddress)
            if n > 0:
                self.buf[s.VirtualAddress:s.VirtualAddress + n] = raw[:n]
            self.sections[name] = {
                'name': name,
                'va': self.base + s.VirtualAddress,
                'vsize': s.Misc_VirtualSize,
                'raw_size': s.SizeOfRawData,
                'raw_offset': s.PointerToRawData,
                'characteristics': s.Characteristics,
                'executable': bool(s.Characteristics & IMAGE_SCN_MEM_EXECUTE),
                'code': bool(s.Characteristics & IMAGE_SCN_CNT_CODE),
            }

        self.regions = []
        for name in lift_sections:
            sec = self.sections.get(name)
            if sec is None:
                continue
            size = max(sec['vsize'], sec['raw_size'])
            self.regions.append({'name': name, 'start': sec['va'],
                                 'end': sec['va'] + size, 'size': size})
        self.regions.sort(key=lambda r: r['start'])
        self.region_by_name = {r['name']: r for r in self.regions}

        trace = self.sections.get(TRACE_SECTION)
        self.trace = None
        if trace:
            self.trace = {'name': TRACE_SECTION, 'start': trace['va'],
                          'end': trace['va'] + max(trace['vsize'], trace['raw_size'])}

    @property
    def code_start(self):
        return self.base        # buffer is RVA indexed: va - code_start == rva

    @property
    def code_end(self):
        return self.base + len(self.buf)

    def region_of(self, va):
        for r in self.regions:
            if r['start'] <= va < r['end']:
                return r
        return None

    def in_code(self, va):
        return self.region_of(va) is not None

    def in_trace(self, va):
        return bool(self.trace and self.trace['start'] <= va < self.trace['end'])

    def bytes_at(self, va, n):
        off = va - self.base
        return bytes(self.buf[off:off + n])


def all_exports(img):
    """Every export as (va, name). The export table stores RVAs."""
    out = []
    pe = img.pe
    try:
        directory = pe.DIRECTORY_ENTRY_EXPORT
    except AttributeError:
        return out
    for sym in getattr(directory, 'symbols', ()):
        name = sym.name.decode('ascii', 'replace') if sym.name else f'ordinal_{sym.ordinal}'
        out.append((img.base + sym.address, name))
    return out


def export_targets(img):
    """(va, name) for every export that points into a lifted code region."""
    return [(va, name) for va, name in all_exports(img) if img.in_code(va)]


def relocation_targets(img):
    """Relocation targets inside code regions.

    The recovered image carries no base relocation directory (the wrapper
    resolved them), so this is empty -- reported as such rather than assumed.
    """
    pe = img.pe
    if not pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size:
        return []
    targets = []
    for block in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', ()):
        for entry in block.entries:
            if entry.type in (3, 10):        # HIGHLOW, DIR64
                va = img.base + entry.rva
                if img.in_code(va):
                    targets.append(va)
    return targets


def scan_tail_jumps(img, gen):
    """E9 rel32 targets: MSVC tail calls are real function entries too."""
    found = []
    for r in img.regions:
        blob = img.bytes_at(r['start'], r['size'])
        start = r['start']
        for i in range(len(blob) - 5):
            if blob[i] != 0xE9:
                continue
            rel = struct.unpack_from('<i', blob, i + 1)[0]
            target = (start + i + 5 + rel) & 0xFFFFFFFF
            if img.in_code(target):
                found.append(target)
    return found


def discover_entries(img, gen):
    """Function entries: entry point, exports, calls, tail jumps, prologues."""
    entries = {}
    reasons = {}

    def add(va, why):
        if va in entries or not img.in_code(va):
            return
        entries[va] = why
        reasons.setdefault(why.split(':')[0], 0)
        reasons[why.split(':')[0]] += 1

    add(img.entry_va, 'entry_point')
    for r in img.regions:
        add(r['start'], 'region_start')
    for va, name in export_targets(img):
        add(va, 'export:' + name)
    for r in img.regions:
        blob = img.bytes_at(r['start'], r['size'])
        for va in gen.find_entries(blob, r['start'], r['end']):
            add(va, 'call_or_prologue')
    for va in scan_tail_jumps(img, gen):
        add(va, 'tail_jump')
    relocs = relocation_targets(img)
    for va in relocs:
        add(va, 'relocation')
    return entries, reasons, relocs


# ---------------------------------------------------------------- lifting ---

UNIMPL_RE = re.compile(
    r';\s*/\* UNIMPLEMENTED: (\S+) (.*?) \*/ /\* 0x([0-9A-Fa-f]{8}): [^\n]*?\*/')
# lift32_sse.py emits a second, op_str-less form:
#     ; /* UNIMPLEMENTED: punpckldq */ /* 0x004B0B53: punpckldq xmm3, xmm0 */
# v1's UNIMPL_RE does not match it, so those SSE/AVX instructions stayed as
# silent null statements (105 in the v1 .code lift).  v2 traps them by name.
UNIMPL_SSE_RE = re.compile(
    r';\s*/\* UNIMPLEMENTED: (\S+) \*/ /\* 0x([0-9A-Fa-f]{8}): [^\n]*?\*/')
INDIRECT_RE = re.compile(r'RECOMP_I(?:TAIL|CALL)\(0x([0-9A-Fa-f]{8})u\)')

TRAP_HEADER = '''/* Auto-generated by tools/static_full_lift.py - DO NOT EDIT */
#ifndef RECOMP_UNSUPPORTED_H
#define RECOMP_UNSUPPORTED_H
#include <stdint.h>
/* A guest instruction the lifter cannot translate. It must stop the guest
 * with a named trap -- never fall through as a silent no-op. */
void recomp_unsupported(uint32_t va, const char *what);
#endif
'''

TRAP_SOURCE = '''/* Auto-generated by tools/static_full_lift.py - DO NOT EDIT */
#include "recomp_unsupported.h"
#include <stdio.h>
#include <stdlib.h>

void recomp_unsupported(uint32_t va, const char *what) {
    fprintf(stderr, "RECOMP: unsupported guest instruction '%s' at 0x%08X\\n",
            what, (unsigned)va);
    abort();
}
'''


class Stats:
    def __init__(self):
        self.functions = 0
        self.instructions = 0
        self.unsupported = {}         # mnemonic -> count
        self.unsupported_samples = {}  # mnemonic -> [va, ...]
        self.lift_errors = []
        self.decoded = set()           # every lifted instruction address
        self.covered = {}              # region name -> bytearray of covered bytes
        self.trimmed_addresses = set()
        self.reached = set()
        self.empty_entries = 0
        self.trace_edges = []
        self.indirect_targets = set()
        self.clean = 0
        self.unclean = 0
        self.trimmed = 0

    def note_unsupported(self, mnem, va):
        self.unsupported[mnem] = self.unsupported.get(mnem, 0) + 1
        samples = self.unsupported_samples.setdefault(mnem, [])
        if len(samples) < 20:
            samples.append(va)


def keep_reached(instructions, leaders):
    """Instructions decoded from a reachability set are all reachable.

    The one after a `ret`, an unconditional `jmp` or an `int3` is then a block
    start even when no direct branch names it -- a switch arm reached only
    through `jmp [reg*4 + table]`, or code after MSVC's __assume(0) int3.
    trim_instructions() would drop such a block as filler (the audio mixer at
    0x008CCB00 lost the arm 0x008CCC0A and tail-dispatched to an address nobody
    had lifted), or cut the function at its first int3.
    """
    prev = None
    for insn in instructions:
        if prev is not None and (prev.is_ret or prev.is_uncond_jump or prev.mnemonic == 'int3'):
            leaders.add(insn.address)
        prev = insn
    return list(instructions)


def trim_instructions(instructions, leaders):
    """generate.py's main() trim: stop at int3, drop filler after a ret."""
    trimmed = []
    seen_ret = False
    for insn in instructions:
        if insn.mnemonic == 'int3':
            break
        if seen_ret:
            if insn.address not in leaders:
                continue
            seen_ret = False
        trimmed.append(insn)
        if insn.is_ret:
            seen_ret = True
    return trimmed


def lift_one(gen, lift32, md, img, lifter, va, entry_set, stats, use_extent=True):
    """Lift one function. Returns (c_text, called_targets, info) or (None, ..)."""
    region = img.region_of(va)
    reached = set()
    called = set()
    if use_extent:
        hard_end = min(va + EXTENT_REACH, region['end'])
        try:
            top, clean = gen.true_extent(md, img.buf, img.code_start, va, hard_end,
                                         entry_set, reached=reached, called=called)
        except Exception as exc:                              # pragma: no cover
            stats.lift_errors.append({'va': va, 'stage': 'extent', 'error': repr(exc)})
            return None, called, {'error': repr(exc)}
        if not reached or top <= va:
            stats.empty_entries += 1
            return None, called, {'empty': True}
        body_start = min(min(reached), va)
        try:
            instructions, leaders = gen.linear_disassemble_function(
                md, img.buf, img.code_start, body_start, top, reached=reached)
        except Exception as exc:                              # pragma: no cover
            stats.lift_errors.append({'va': va, 'stage': 'disasm', 'error': repr(exc)})
            return None, called, {'error': repr(exc)}
        clean = clean
    else:
        # generate.py main()'s policy: clamp at the next catalogued entry.
        nxt = min([e for e in entry_set if e > va] + [region['end']])
        top = min(nxt, va + 65536)
        instructions, leaders = gen.linear_disassemble_function(
            md, img.buf, img.code_start, va, top)
        clean = True
    if not instructions:
        stats.empty_entries += 1
        return None, called, {'empty': True}

    # A body can start below its entry (MSVC places strchr's shared exit in
    # front of the function). The entry is then not the first instruction and
    # no branch names it, so it must be made a leader explicitly -- otherwise
    # the block is dropped as filler after the preceding `ret` and the
    # function tail-jumps to itself forever (0x009A4700, original image).
    leaders.add(va)
    trimmed = keep_reached(instructions, leaders) if reached else trim_instructions(instructions, leaders)
    if len(trimmed) != len(instructions):
        stats.trimmed += len(instructions) - len(trimmed)
        stats.trimmed_addresses |= {i.address for i in instructions} - {i.address for i in trimmed}
    if not trimmed:
        stats.empty_entries += 1
        return None, called, {'empty': True}

    name = f'sub_{va:08X}'
    try:
        text = gen.lift_function_linear(lifter, name, trimmed, leaders, va)
    except Exception as exc:                                  # pragma: no cover
        stats.lift_errors.append({'va': va, 'stage': 'lift', 'error': repr(exc)})
        text = (f'/* LIFT ERROR: {name} at 0x{va:08X}: {exc} */\n'
                f'void {name}(void) {{ '
                f'recomp_unsupported(0x{va:08X}u, "lift-error"); abort(); }}\n')

    # Unsupported instructions become named traps, counted per mnemonic.
    def _trap(m):
        mnem, opstr, addr = m.group(1), m.group(2), int(m.group(3), 16)
        stats.note_unsupported(mnem, addr)
        return ('{ RECOMP_REGS_OUT(); recomp_unsupported(0x%08Xu, "%s"); abort(); }'
                ' /* UNSUPPORTED: %s %s */' % (addr, mnem, mnem, opstr))


    text = UNIMPL_RE.sub(_trap, text)

    cover = stats.covered.get(region['name'])
    for insn in trimmed:
        stats.decoded.add(insn.address)
        if cover is not None:
            lo = insn.address - region['start']
            cover[lo:lo + insn.size] = b'\x01' * insn.size
        if (insn.is_jump or insn.is_call) and insn.operands:
            from capstone.x86 import X86_OP_IMM
            if insn.operands[0].type == X86_OP_IMM:
                target = insn.operands[0].imm & 0xFFFFFFFF
                if img.in_trace(target):
                    stats.trace_edges.append({
                        'from': insn.address,
                        'function': va,
                        'kind': 'call' if insn.is_call else
                                ('jump' if insn.is_uncond_jump else 'cond_jump'),
                        'mnemonic': insn.mnemonic,
                        'target': target,
                    })
    stats.reached |= reached
    for m in INDIRECT_RE.finditer(text):
        stats.indirect_targets.add(int(m.group(1), 16))

    if clean:
        stats.clean += 1
    else:
        stats.unclean += 1
    stats.functions += 1
    stats.instructions += len(trimmed)
    body_start = min(trimmed[0].address, va)
    return text, called, {'size': top - body_start, 'instructions': len(trimmed),
                          'clean': clean, 'region': region['name']}


def write_shard(out_dir, index, funcs):
    path = out_dir / f'recomp_{index:04d}.c'
    with open(path, 'w') as f:
        f.write('/* Auto-generated by tools/static_full_lift.py - DO NOT EDIT */\n')
        f.write(f'/* File {index}: {len(funcs)} functions */\n\n')
        f.write('#define RECOMP_GENERATED_CODE\n')
        f.write('#include "recomp_types.h"\n')
        f.write('#include "recomp_funcs.h"\n')
        f.write('#include "recomp_unsupported.h"\n')
        f.write('#include <math.h>\n')
        f.write('#include <string.h>\n\n')
        for text, va, name in funcs:
            f.write(text)
            f.write('\n\n')
    return path


DEFAULT_NDK = Path(os.environ.get('ANDROID_NDK_ROOT', '/opt/android-ndk'))


def find_clang(ndk=None):
    """A host clang for -fsyntax-only: PATH first, then the NDK's host clang.

    The NDK clang is a host binary; with --target=aarch64-linux-android21 it
    also resolves the libc headers, which a bare host clang without a sysroot
    cannot. Zig's `cc` is the last resort.
    """
    import shutil
    for name in ('clang', 'clang-19', 'clang-18', 'clang-17', 'clang-16'):
        path = shutil.which(name)
        if path:
            return [path], None
    root = Path(ndk) if ndk else DEFAULT_NDK
    cand = root / 'toolchains' / 'llvm' / 'prebuilt' / 'linux-x86_64' / 'bin' / 'clang'
    if cand.exists():
        return [str(cand), '--target=aarch64-linux-android21'], str(root)
    zig = ROOT / 'tools' / 'cc'
    if zig.exists():
        return [str(zig), 'cc'], None
    return None, None


def syntax_check(out_dir, tu_paths, include_dirs, limit=3, ndk=None):
    """-fsyntax-only on a sample of TUs with the host clang."""
    import subprocess
    compiler, ndk_root = find_clang(ndk)
    if not compiler:
        return {'compiler': None, 'checked': [], 'ok': False,
                'error': 'no host clang on PATH and no NDK clang'}
    sample = [tu_paths[0]] if tu_paths else []
    if len(tu_paths) > 2:
        sample.append(tu_paths[len(tu_paths) // 2])
    if len(tu_paths) > 1:
        sample.append(tu_paths[-1])
    results = []
    for path in sample[:limit]:
        cmd = [*compiler, '-fsyntax-only', '-std=gnu11', '-w',
               *[f'-I{d}' for d in include_dirs], str(path)]
        started = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        results.append({'file': path.name, 'returncode': proc.returncode,
                        'seconds': round(time.time() - started, 1),
                        'stderr': proc.stderr[-4000:]})
    return {'compiler': ' '.join(compiler), 'ndk': ndk_root, 'checked': results,
            'ok': bool(results) and all(r['returncode'] == 0 for r in results)}


# ============================================================================
# v2: data-pointer discovery, .trace as a lift region, opaque functions
# ============================================================================
# Everything below is additive.  ``main()`` dispatches to ``run_v2`` only when
# ``--v2`` is given; the v1 code path above is untouched and produces the same
# output as before.

V2_OUT = ROOT / 'port' / 'generated' / 'static-full-v2'
V1_TRACE_EDGES = ROOT / 'port' / 'generated' / 'static-full-v1' / 'trace_edges.json'
V1_STATS = ROOT / 'port' / 'generated' / 'static-full-v1' / 'stats.json'

OPAQUE_TRAP_FRAC = 0.25      # >25% untranslatable instructions -> opaque
OPAQUE_COVER_FRAC = 0.50     # <50% of the extent span decodes -> opaque
OPAQUE_MIN_INSNS = 6
REGION_ORDER = {'.code': 0, '.srdata': 1, '.text1': 2, '.ooa': 3, '.trace': 4}

_E8_RE = re.compile(b'\xe8')
_E9_RE = re.compile(b'\xe9')
_PROLOGUE_RE = re.compile(b'\x55\x8b\xec')
_SUB_ESP_RE = re.compile(b'[\xcc\x90\xc3]\x83\xec')


def fast_find_entries(blob, start, end):
    """generate.find_entries() semantics, vectorised for a 113 MB section.

    The upstream version loops over every byte in Python; on ``.trace`` that
    is 113 M iterations.  The rules are identical -- ``E8 rel32`` call
    targets, ``55 8B EC`` prologues, ``83 EC`` after a pad byte -- only the
    byte scan is done by ``re``.
    """
    out = set()
    n = len(blob)
    for m in _E8_RE.finditer(blob):
        i = m.start()
        if i + 5 > n:
            break
        t = (start + i + 5 + struct.unpack_from('<i', blob, i + 1)[0]) & 0xFFFFFFFF
        if start <= t < end:
            out.add(t)
    for m in _PROLOGUE_RE.finditer(blob):
        out.add(start + m.start())
    for m in _SUB_ESP_RE.finditer(blob):
        out.add(start + m.start() + 1)
    return out


def fast_tail_jumps(blob, start, end):
    """E9 rel32 targets inside [start, end) (MSVC tail calls are entries)."""
    out = set()
    n = len(blob)
    for m in _E9_RE.finditer(blob):
        i = m.start()
        if i + 5 > n:
            break
        t = (start + i + 5 + struct.unpack_from('<i', blob, i + 1)[0]) & 0xFFFFFFFF
        if start <= t < end:
            out.add(t)
    return out


class Coverage:
    """Per-region byte maps: bytes covered by a lifted instruction, and which
    of those bytes are an instruction start (to reject mid-instruction starts)."""

    def __init__(self, regions):
        self.regions = regions
        self.cover = {r['name']: bytearray(r['size']) for r in regions}
        self.start = {r['name']: bytearray(r['size']) for r in regions}

    def mark(self, region, va, size):
        off = va - region['start']
        self.cover[region['name']][off:off + size] = b'\x01' * size
        self.start[region['name']][off] = 1

    def is_mid(self, region, va):
        off = va - region['start']
        return bool(self.cover[region['name']][off]) and \
            not self.start[region['name']][off]


def scan_data_pointers(img, md, cover, stats):
    """4-byte aligned dwords in every non-code section that point into code.

    ``.link``/``.sdata``/``.data``/``.rdata``/``.edata``/``.text``/``.rsrc``/
    ``.sbss`` initialisers are scanned (raw bytes only -- uninitialised tails
    are not invented).  A value is accepted when it lands in a lift region, is
    not in the middle of an already lifted instruction, is not ``00``/``CC``
    padding and decodes as at least one instruction.  Runs of >= 3
    consecutive code pointers are counted as vtables and reported separately.
    """
    import array
    import bisect
    lift_names = {r['name'] for r in img.regions}
    starts = [r['start'] for r in img.regions]

    def region_of(va):
        i = bisect.bisect_right(starts, va) - 1
        if i >= 0 and va < img.regions[i]['end']:
            return img.regions[i]
        return None

    found = {}
    for name, sec in sorted(img.sections.items()):
        if name in lift_names:
            continue
        off = sec['va'] - img.base
        n = min(sec['raw_size'], len(img.buf) - off) & ~3
        if n < 4:
            continue
        words = array.array('I')
        words.frombytes(bytes(img.buf[off:off + n]))
        if sys.byteorder != 'little':
            words.byteswap()
        count = len(words)
        is_code = bytearray(count)
        for i in range(count):
            if region_of(words[i]) is not None:
                is_code[i] = 1
        code_words = sum(is_code)
        stats.dp_scanned += count
        stats.dp_code_words += code_words
        stats.dp_by_section[name] = {'dwords': count, 'code_pointers': code_words}

        # vtable heuristic: a run of >= 3 consecutive code pointers
        vtable = bytearray(count)
        i = 0
        while i < count - 2:
            if is_code[i] and is_code[i + 1] and is_code[i + 2]:
                j = i
                while j < count and is_code[j]:
                    vtable[j] = 1
                    j += 1
                i = j
            else:
                i += 1

        for i in range(count):
            if not is_code[i]:
                continue
            va = words[i]
            r = region_of(va)
            if cover.is_mid(r, va):
                stats.dp_rejected['mid_instruction'] += 1
                continue
            first = img.buf[va - img.base]
            if first in (0x00, 0xCC):
                stats.dp_rejected['int3_or_zero'] += 1
                continue
            ok = False
            view = memoryview(img.buf)
            for _ in md.disasm(view[va - img.base:va - img.base + 16], va):
                ok = True
                break
            if not ok:
                stats.dp_rejected['undecodable'] += 1
                continue
            found.setdefault(va, 'vtable' if vtable[i] else 'data_pointer')
    return found


def load_v1_trace_edges():
    """Targets of the .trace edges measured by the v1 run, if it exists."""
    if not V1_TRACE_EDGES.exists():
        return []
    try:
        doc = json.loads(V1_TRACE_EDGES.read_text())
    except (ValueError, OSError):
        return []
    out = []
    for e in doc.get('edges', ()):
        try:
            out.append(int(e['target'], 16))
        except (KeyError, TypeError, ValueError):
            continue
    return out


# ------------------------------------------------------------ v2 plumbing ---
# The v2 driver lifts four compiler-generated regions plus the 113 MB .trace
# region, in parallel, and iterates discovery to a fixpoint.  Everything a
# worker learns about a chunk is returned as plain, small data; the C text goes
# straight to a shard file on disk.  Nothing per-instruction crosses a process
# boundary except the instruction-start array the parent needs for the
# mid-instruction test and the witness coverage report.

EDGE_LIST_CAP = 200000          # stored cross-region edges (counts stay exact)
WITNESS_RE = re.compile(rb'"(0x[0-9a-fA-F]{6,8})"\s*:\s*\{\s*"size"\s*:\s*(\d+)')
WITNESS_CACHE = 'witness-union.bin'


class V2Stats:
    """Per-chunk accumulator; merged into the parent's totals after each chunk."""

    __slots__ = ('functions', 'instructions', 'clean', 'unclean', 'trimmed',
                 'empty_entries', 'unsupported', 'unsupported_samples',
                 'lift_errors', 'cross_edges', 'cross_edges_total', 'edge_kinds',
                 'code_immediates', 'indirect_targets', 'opaque', 'opaque_reasons',
                 'opaque_unsupported', 'opaque_samples', 'clflush',
                 'clflush_samples', 'trace_instructions',
                 'trace_immediates_ignored', 'sparse_extents', 'sparse_samples')

    def __init__(self):
        self.functions = 0
        self.instructions = 0
        self.clean = 0
        self.unclean = 0
        self.trimmed = 0
        self.empty_entries = 0
        self.unsupported = {}
        self.unsupported_samples = {}
        self.lift_errors = []
        self.cross_edges = []
        self.cross_edges_total = 0
        self.edge_kinds = {}
        self.code_immediates = set()
        self.indirect_targets = set()
        self.opaque = 0
        self.opaque_reasons = {}
        self.opaque_unsupported = 0
        self.opaque_samples = []
        self.clflush = 0
        self.clflush_samples = []
        self.trace_instructions = 0
        self.trace_immediates_ignored = 0
        self.sparse_extents = 0
        self.sparse_samples = []

    def note_unsupported(self, mnem, va):
        self.unsupported[mnem] = self.unsupported.get(mnem, 0) + 1
        samples = self.unsupported_samples.setdefault(mnem, [])
        if len(samples) < 12:
            samples.append(va)

    def edge(self, key):
        self.edge_kinds[key] = self.edge_kinds.get(key, 0) + 1

    def to_dict(self):
        return {
            'functions': self.functions, 'instructions': self.instructions,
            'clean': self.clean, 'unclean': self.unclean, 'trimmed': self.trimmed,
            'empty_entries': self.empty_entries,
            'unsupported': self.unsupported,
            'unsupported_samples': self.unsupported_samples,
            'lift_errors': self.lift_errors[:20],
            'cross_edges': self.cross_edges,
            'cross_edges_total': self.cross_edges_total,
            'edge_kinds': self.edge_kinds,
            'code_immediates': list(self.code_immediates),
            'indirect_targets': list(self.indirect_targets),
            'opaque': self.opaque, 'opaque_reasons': self.opaque_reasons,
            'opaque_unsupported': self.opaque_unsupported,
            'opaque_samples': self.opaque_samples,
            'clflush': self.clflush, 'clflush_samples': self.clflush_samples,
            'trace_instructions': self.trace_instructions,
            'trace_immediates_ignored': self.trace_immediates_ignored,
            'sparse_extents': self.sparse_extents,
            'sparse_samples': self.sparse_samples,
        }

    def merge(self, d):
        self.functions += d['functions']
        self.instructions += d['instructions']
        self.clean += d['clean']
        self.unclean += d['unclean']
        self.trimmed += d['trimmed']
        self.empty_entries += d['empty_entries']
        for k, v in d['unsupported'].items():
            self.unsupported[k] = self.unsupported.get(k, 0) + v
        for k, vs in d['unsupported_samples'].items():
            have = self.unsupported_samples.setdefault(k, [])
            if len(have) < 12:
                have.extend(vs[:12 - len(have)])
        self.lift_errors.extend(d['lift_errors'])
        self.cross_edges.extend(d['cross_edges'])
        self.cross_edges_total += d['cross_edges_total']
        for k, v in d['edge_kinds'].items():
            self.edge_kinds[k] = self.edge_kinds.get(k, 0) + v
        self.code_immediates.update(d['code_immediates'])
        self.indirect_targets.update(d['indirect_targets'])
        self.opaque += d['opaque']
        for k, v in d['opaque_reasons'].items():
            self.opaque_reasons[k] = self.opaque_reasons.get(k, 0) + v
        self.opaque_unsupported += d['opaque_unsupported']
        if len(self.opaque_samples) < 40:
            self.opaque_samples.extend(d['opaque_samples'][:40 - len(self.opaque_samples)])
        self.clflush += d['clflush']
        if len(self.clflush_samples) < 20:
            self.clflush_samples.extend(
                d['clflush_samples'][:20 - len(self.clflush_samples)])
        self.trace_instructions += d['trace_instructions']
        self.trace_immediates_ignored += d['trace_immediates_ignored']
        self.sparse_extents += d['sparse_extents']
        if len(self.sparse_samples) < 8:
            self.sparse_samples.extend(
                d['sparse_samples'][:8 - len(self.sparse_samples)])


def merge_intervals(pairs):
    """Sorted, merged [start, end) byte intervals (coverage maps are dense)."""
    if not pairs:
        return []
    pairs.sort()
    out = []
    s, e = pairs[0]
    for a, b in pairs[1:]:
        if a <= e:
            if b > e:
                e = b
        else:
            out.append((s, e))
            s, e = a, b
    out.append((s, e))
    return out


class V2Ctx:
    """Everything one chunk's lifting needs.  Coverage is collected as
    intervals + instruction-start addresses instead of per-byte maps so the
    result can be handed back to the parent cheaply."""

    def __init__(self, img, gen, md, stats, entry_set, lifter,
                 witness=None, witness_sorted=None):
        self.img, self.gen, self.md = img, gen, md
        self.stats, self.entry_set, self.lifter = stats, entry_set, lifter
        self.witness = witness
        self.witness_sorted = witness_sorted
        self.cover_iv = {}
        self.starts = {}
        self.witness_hits = []
        self.witness_conflicts = []

    def mark(self, region, va, size):
        off = va - region['start']
        self.cover_iv.setdefault(region['name'], []).append((off, off + size))
        self.starts.setdefault(region['name'], []).append(va)


def lift_one_v2(ctx, va):
    """Lift one function with the v2 policies.

    Differences from ``lift_one``: clflush becomes a memory no-op barrier,
    bodies whose bytes/instructions do not decode sensibly become a single
    named ``opaque-function`` trap, cross-region edges and code immediates are
    collected, and coverage is reported to the parent as intervals.
    """
    img, gen, md = ctx.img, ctx.gen, ctx.md
    stats = ctx.stats
    region = img.region_of(va)
    reached = set()
    called = set()
    hard_end = min(va + EXTENT_REACH, region['end'])
    try:
        top, clean = gen.true_extent(md, img.buf, img.code_start, va, hard_end,
                                     ctx.entry_set, reached=reached, called=called)
    except Exception as exc:                                  # pragma: no cover
        stats.lift_errors.append({'va': va, 'stage': 'extent', 'error': repr(exc)})
        return None, called, {'error': repr(exc)}
    if not reached or top <= va:
        stats.empty_entries += 1
        return None, called, {'empty': True}
    body_start = min(min(reached), va)
    try:
        instructions, leaders = gen.linear_disassemble_function(
            md, img.buf, img.code_start, body_start, top, reached=reached)
    except Exception as exc:                                  # pragma: no cover
        stats.lift_errors.append({'va': va, 'stage': 'disasm', 'error': repr(exc)})
        return None, called, {'error': repr(exc)}
    if not instructions:
        stats.empty_entries += 1
        return None, called, {'empty': True}

    # A body can start below its entry (MSVC places strchr's shared exit in
    # front of the function). The entry is then not the first instruction and
    # no branch names it, so it must be made a leader explicitly -- otherwise
    # the block is dropped as filler after the preceding `ret` and the
    # function tail-jumps to itself forever (0x009A4700, original image).
    leaders.add(va)
    trimmed = keep_reached(instructions, leaders) if reached else trim_instructions(instructions, leaders)
    if len(trimmed) != len(instructions):
        stats.trimmed += len(instructions) - len(trimmed)
    if not trimmed:
        stats.empty_entries += 1
        return None, called, {'empty': True}

    name = f'sub_{va:08X}'
    try:
        text = gen.lift_function_linear(ctx.lifter, name, trimmed, leaders, va)
    except Exception as exc:                                  # pragma: no cover
        stats.lift_errors.append({'va': va, 'stage': 'lift', 'error': repr(exc)})
        text = (f'/* LIFT ERROR: {name} at 0x{va:08X}: {exc} */\n'
                f'void {name}(void) {{ '
                f'recomp_unsupported(0x{va:08X}u, "lift-error"); abort(); }}\n')

    unimpl = list(UNIMPL_RE.finditer(text))
    sse_unimpl = list(UNIMPL_SSE_RE.finditer(text))
    traps = len(unimpl) + len(sse_unimpl)
    span = max(1, top - body_start)
    decoded_bytes = sum(i.size for i in trimmed)
    opaque = None
    # Opaque means "the bytes do not decode to sane x86": a high share of
    # instructions the lifter cannot translate (invalid/obsolete opcodes are
    # what encrypted or garbage bytes decode to).  A *sparse* extent is NOT
    # opaque -- a switch function with a 150-byte jump table between its
    # instructions is perfectly liftable, so that case is only counted.
    if len(trimmed) >= OPAQUE_MIN_INSNS:
        if traps / len(trimmed) > OPAQUE_TRAP_FRAC:
            opaque = 'unsupported_fraction'
    if not opaque and decoded_bytes / span < OPAQUE_COVER_FRAC:
        stats.sparse_extents += 1
        if len(stats.sparse_samples) < 8:
            stats.sparse_samples.append({
                'va': va, 'region': region['name'],
                'instructions': len(trimmed), 'traps': traps,
                'decoded_bytes': decoded_bytes, 'span': span})

    if opaque:
        stats.opaque += 1
        stats.opaque_reasons[opaque] = stats.opaque_reasons.get(opaque, 0) + 1
        stats.opaque_unsupported += traps
        if len(stats.opaque_samples) < 8:
            stats.opaque_samples.append({
                'va': va, 'region': region['name'], 'reason': opaque,
                'instructions': len(trimmed), 'traps': traps,
                'decoded_bytes': decoded_bytes, 'span': span})
        text = (f'/* OPAQUE: {name} at 0x{va:08X} ({region["name"]}): {opaque} '
                f'-- {traps}/{len(trimmed)} instructions untranslatable, '
                f'{decoded_bytes}/{span} bytes of the extent decode.  Emitted as '
                f'a named trap; the bytes are not guessed. */\n'
                f'void {name}(void) {{ '
                f'recomp_unsupported(0x{va:08X}u, "opaque-function"); abort(); }}\n')
    else:
        def _trap(m):
            mnem, opstr, addr = m.group(1), m.group(2), int(m.group(3), 16)
            if mnem == 'clflush':
                # CLFLUSH only invalidates a cache line.  For guest data values
                # it is a no-op; the fence keeps the ordering a barrier implies.
                stats.clflush += 1
                if len(stats.clflush_samples) < 20:
                    stats.clflush_samples.append(addr)
                return ('{ __atomic_thread_fence(__ATOMIC_SEQ_CST); }'
                        ' /* CLFLUSH %s at 0x%08X: memory no-op barrier */'
                        % (opstr, addr))
            stats.note_unsupported(mnem, addr)
            return ('{ RECOMP_REGS_OUT(); recomp_unsupported(0x%08Xu, "%s"); abort(); }'
                    ' /* UNSUPPORTED: %s %s */' % (addr, mnem, mnem, opstr))

        text = UNIMPL_RE.sub(_trap, text)

        def _trap_sse(m):
            mnem, addr = m.group(1), int(m.group(2), 16)
            stats.note_unsupported(mnem, addr)
            return ('{ RECOMP_REGS_OUT(); recomp_unsupported(0x%08Xu, "%s"); abort(); }'
                    ' /* UNSUPPORTED: %s */' % (addr, mnem, mnem))

        text = UNIMPL_SSE_RE.sub(_trap_sse, text)

    # coverage, edges, code immediates, witness coverage
    from capstone.x86 import X86_OP_IMM
    witness = ctx.witness
    wsorted = ctx.witness_sorted
    for insn in trimmed:
        ctx.mark(region, insn.address, insn.size)
        if witness is not None:
            if insn.address in witness:
                ctx.witness_hits.append(insn.address)
            elif wsorted:
                i = bisect.bisect_left(wsorted, insn.address + 1)
                if i < len(wsorted) and wsorted[i] < insn.address + insn.size:
                    ctx.witness_conflicts.append(wsorted[i])
        if insn.is_jump or insn.is_call:
            if insn.operands and insn.operands[0].type == X86_OP_IMM:
                target = insn.operands[0].imm & 0xFFFFFFFF
                tr = img.region_of(target)
                if tr is None:
                    continue
                kind = ('call' if insn.is_call else
                        'jump' if insn.is_uncond_jump else 'cond_jump')
                if tr['name'] == region['name']:
                    stats.edge('intra:' + region['name'])
                else:
                    stats.cross_edges_total += 1
                    if len(stats.cross_edges) < EDGE_LIST_CAP:
                        stats.cross_edges.append(
                            (insn.address, va, kind, insn.mnemonic, target,
                             region['name'], tr['name']))
                    stats.edge(f'{region["name"]}->{tr["name"]}')
        elif region['name'] != TRACE_SECTION:
            # Immediate operands that point into code are real function
            # pointers in compiler-generated code (callback tables, jump
            # tables, vtable slots).  They are NOT evidence in the protected
            # .trace region: that code is obfuscated (pushfd/popfd/xchg
            # [esp]/ret blocks, random constants), so a 32-bit constant
            # landing in the 108 MB .trace range is chance, not a pointer.
            # .trace entries come from exports, trace edges, call/tail
            # targets, data pointers and witness seeds -- not from immediates.
            # The ignored count is reported, not hidden.
            for op in insn.operands:
                if op.type == X86_OP_IMM:
                    v = op.imm & 0xFFFFFFFF
                    if img.region_of(v) is not None:
                        stats.code_immediates.add(v)
        else:
            for op in insn.operands:
                if op.type == X86_OP_IMM:
                    if img.region_of(op.imm & 0xFFFFFFFF) is not None:
                        stats.trace_immediates_ignored += 1

    for m in INDIRECT_RE.finditer(text):
        stats.indirect_targets.add(int(m.group(1), 16))

    if clean:
        stats.clean += 1
    else:
        stats.unclean += 1
    stats.functions += 1
    stats.instructions += len(trimmed)
    return text, called, {'size': top - body_start, 'instructions': len(trimmed),
                          'clean': clean, 'region': region['name'],
                          'opaque': opaque, 'traps': traps,
                          'body_start': body_start, 'top': top}


# ------------------------------------------------------- witness seeds ------

def expand_witness_paths(patterns):
    """Files named by --witness: literal path, directory or glob."""
    import glob as _glob
    files = []
    for pat in patterns:
        p = str(pat)
        if any(ch in p for ch in '*?['):
            files.extend(sorted(_glob.glob(p)))
            continue
        path = Path(p)
        if path.is_dir():
            cand = path / 'trace.json'
            if cand.exists():
                files.append(str(cand))
            else:
                files.extend(sorted(str(x) for x in path.glob('*/trace.json')))
        elif path.exists():
            files.append(str(p))
        else:
            log(f'[!] witness path not found: {p}')
    return files


def dedupe_files(files):
    """Drop byte-identical duplicates (291 traces -> 82 unique on this box)."""
    import hashlib
    by_size = {}
    for f in files:
        try:
            by_size.setdefault(Path(f).stat().st_size, []).append(f)
        except OSError:
            continue
    uniq = []
    for size, group in sorted(by_size.items()):
        if len(group) == 1:
            uniq.append(group[0])
            continue
        seen = set()
        for f in group:
            h = hashlib.md5(Path(f).read_bytes()).hexdigest()
            if h not in seen:
                seen.add(h)
                uniq.append(f)
    return uniq


def load_witness_union(patterns, out_dir):
    """Union of every executed instruction address in the given traces.

    Only the ``instructions`` / ``segment_instructions`` maps are read (each
    entry is ``"0xADDR": {"size": N, ...}``), which is why a 46 MB trace parses
    in 0.15 s instead of a full json.load.  The union is cached next to the
    output so a restart does not re-read 3 GB of traces.
    """
    files = expand_witness_paths(patterns)
    if not files:
        return {}
    uniq = dedupe_files(files)
    log(f'[*] witness: {len(files)} trace files, {len(uniq)} unique by content')
    cache = Path(out_dir) / WITNESS_CACHE
    newest = max(Path(f).stat().st_mtime for f in uniq)
    if cache.exists() and cache.stat().st_mtime >= newest:
        raw = cache.read_bytes()
        import array as _array
        arr = _array.array('I')
        arr.frombytes(raw)
        if sys.byteorder != 'little':
            arr.byteswap()
        out = {arr[i]: arr[i + 1] for i in range(0, len(arr), 2)}
        log(f'[*] witness: {len(out):,} addresses from cache {cache.name}')
        return out
    out = {}
    for n, f in enumerate(uniq):
        try:
            data = Path(f).read_bytes()
        except OSError as exc:
            log(f'[!] witness read failed {f}: {exc}')
            continue
        for m in WITNESS_RE.finditer(data):
            va = int(m.group(1), 16)
            size = int(m.group(2))
            if 0 < size <= 15:
                old = out.get(va)
                if old is None or size > old:
                    out[va] = size
        if (n + 1) % 20 == 0:
            log(f'    witness {n + 1}/{len(uniq)} files, {len(out):,} addresses')
    import array as _array
    arr = _array.array('I')
    for va in sorted(out):
        arr.append(va)
        arr.append(out[va])
    if sys.byteorder != 'little':
        arr.byteswap()
    tmp = cache.with_suffix('.tmp')
    tmp.write_bytes(arr.tobytes())
    tmp.replace(cache)
    log(f'[*] witness: {len(out):,} addresses from {len(uniq)} unique traces')
    return out


def witness_leaders(addr_sizes):
    """Block leaders of an execution trace.

    An address is a leader when the previously executed instruction does not
    end exactly at it -- i.e. control arrived there from a branch, a call, a
    return or an indirect jump rather than by falling through.
    """
    items = sorted(addr_sizes)
    leaders = set()
    prev = None
    for a in items:
        if prev is None or prev + addr_sizes[prev] != a:
            leaders.add(a)
        prev = a
    return leaders


# ------------------------------------------------------- worker process -----

_W = {}


def _v2_worker_init():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    _W['md'] = md
    _W['lifter'] = _W['lift32'].Lifter(iat_map=_W['iat_map'],
                                       lifted=_W['entry_set'])


def _v2_worker_lift(task):
    """Lift one chunk of entries; write its C shard(s); return small metadata."""
    chunk_id, vas = task
    img, gen, md = _W['img'], _W['gen'], _W['md']
    stats = V2Stats()
    ctx = V2Ctx(img, gen, md, stats, _W['entry_set'], _W['lifter'],
                witness=_W.get('witness'), witness_sorted=_W.get('witness_sorted'))
    out_dir = _W['out_dir']
    max_funcs = _W['shard_funcs']
    max_bytes = _W['shard_bytes']

    funcs = []
    called = set()
    cands = {}
    files = []
    buf = []
    buf_bytes = 0
    file_idx = 0

    def flush():
        nonlocal buf, buf_bytes, file_idx
        if not buf:
            return
        name = f'recomp_{chunk_id:05d}_{file_idx:02d}.c'
        write_shard_named(out_dir, name, buf)
        files.append(name)
        file_idx += 1
        buf = []
        buf_bytes = 0

    for va, reason in vas:
        r = img.region_of(va)
        text, c, meta = lift_one_v2(ctx, va)
        called |= c
        if text is None:
            if meta.get('error'):
                name = f'sub_{va:08X}'
                text = (f'/* LIFT ERROR: {name} at 0x{va:08X}: {meta["error"]} */\n'
                        f'void {name}(void) {{ '
                        f'recomp_unsupported(0x{va:08X}u, "lift-error"); abort(); }}\n')
                funcs.append((va, name, 0, 0, False, r['name'], va, va, None, 0,
                              len(files), reason))
                buf.append((text, va, name))
                buf_bytes += len(text)
                if len(buf) >= max_funcs or buf_bytes >= max_bytes:
                    flush()
            continue
        name = f'sub_{va:08X}'
        if r['name'] == TRACE_SECTION:
            stats.trace_instructions += meta['instructions']
        funcs.append((va, name, meta['size'], meta['instructions'], meta['clean'],
                      meta['region'], meta['body_start'], meta['top'],
                      meta['opaque'], meta['traps'], len(files), reason))
        buf.append((text, va, name))
        buf_bytes += len(text)
        if len(buf) >= max_funcs or buf_bytes >= max_bytes:
            flush()
    flush()

    for t in called:
        cands.setdefault('call_target', []).append(t)
    for t in stats.indirect_targets:
        cands.setdefault('indirect_target', []).append(t)
    for t in stats.code_immediates:
        cands.setdefault('code_immediate', []).append(t)

    return {
        'chunk': chunk_id,
        'files': files,
        'funcs': funcs,
        'called': list(called),
        'cands': cands,
        'stats': stats.to_dict(),
        'cover': {k: merge_intervals(v) for k, v in ctx.cover_iv.items()},
        'starts': {k: v for k, v in ctx.starts.items()},
        'witness_hits': ctx.witness_hits,
        'witness_conflicts': ctx.witness_conflicts,
    }


def write_shard_named(out_dir, name, funcs):
    path = Path(out_dir) / name
    with open(path, 'w') as f:
        f.write('/* Auto-generated by tools/static_full_lift.py --v2 - DO NOT EDIT */\n')
        f.write(f'/* {name}: {len(funcs)} functions */\n\n')
        f.write('#define RECOMP_GENERATED_CODE\n')
        f.write('#include "recomp_types.h"\n')
        f.write('#include "recomp_funcs.h"\n')
        f.write('#include "recomp_unsupported.h"\n')
        f.write('#include <math.h>\n')
        f.write('#include <string.h>\n\n')
        for text, va, fname in funcs:
            f.write(text)
            f.write('\n\n')
    return path


def fixup_calls(tu_paths, emitted):
    """RECOMP_CALL to a function with no body -> RECOMP_ICALL (no dead symbol)."""
    call_re = re.compile(r'RECOMP_CALL\((sub_[0-9A-F]{8})\)')
    rewritten = 0

    def _fix(m):
        nonlocal rewritten
        if m.group(1) in emitted:
            return m.group(0)
        rewritten += 1
        return 'RECOMP_ICALL(0x%08Xu)' % int(m.group(1)[4:], 16)

    for path in tu_paths:
        text = path.read_text()
        if 'RECOMP_CALL(' not in text:
            continue
        new_text = call_re.sub(_fix, text)
        if new_text != text:
            path.write_text(new_text)
    return rewritten


def syntax_check_all(tu_paths, include_dirs, ndk=None, jobs=6):
    """-fsyntax-only on EVERY translation unit, NDK clang, max 6 in parallel."""
    import concurrent.futures
    import subprocess
    root = Path(ndk) if ndk else DEFAULT_NDK
    clang = root / 'toolchains' / 'llvm' / 'prebuilt' / 'linux-x86_64' / 'bin' / 'clang'
    if clang.exists():
        compiler = [str(clang), '--target=aarch64-linux-android29']
        ndk_root = str(root)
    else:
        compiler, ndk_root = find_clang(ndk)
        if compiler and any(a.startswith('--target=') for a in compiler):
            compiler = [a.replace('android21', 'android29') for a in compiler]
    if not compiler:
        return {'compiler': None, 'ok': False, 'checked': 0,
                'error': 'no clang (NDK or PATH) for the syntax check'}

    def run(path):
        cmd = [*compiler, '-fsyntax-only', '-std=gnu11', '-w',
               *[f'-I{d}' for d in include_dirs], str(path)]
        started = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        return {'file': path.name, 'returncode': proc.returncode,
                'seconds': round(time.time() - started, 1),
                'stderr': proc.stderr[-2000:]}

    workers = max(1, min(6, int(jobs or 1)))
    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(run, tu_paths))
    failed = [r for r in results if r['returncode'] != 0]
    return {'compiler': ' '.join(compiler), 'ndk': ndk_root, 'jobs': workers,
            'checked': len(results), 'failed': len(failed),
            'seconds': round(time.time() - started, 1),
            'ok': not failed,
            'failures': failed[:10]}


def _atomic_write(path, text):
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(text)
    tmp.replace(path)
    return path


def run_v2(args):
    import multiprocessing as mpool
    gen, lift32, analyze_pe, build_iat_map = load_upstream()
    assert gen.EXTENT_REACH == EXTENT_REACH, (gen.EXTENT_REACH, EXTENT_REACH)
    started = time.time()

    def logt(msg):
        log(f'[{time.time() - started:7.1f}s] {msg}')

    img = Image(args.exe, LIFT_SECTIONS + (TRACE_SECTION,))
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    # A previous lift into the same directory can leave shards this run does
    # not write again (different chunking); a build that globs recomp_*.c then
    # links the stale bodies as duplicate symbols.
    for stale in out_dir.glob('recomp_*.c'):
        stale.unlink()
    log(f'[*] v2 image 0x{img.base:08X} entry 0x{img.entry_va:08X} '
        f'{img.file_size:,} bytes')
    for r in img.regions:
        log(f'    lift region {r["name"]:<8} 0x{r["start"]:08X}-0x{r["end"]:08X} '
            f'({r["size"]:,} bytes)')
    region_start = {r['name']: r['start'] for r in img.regions}

    info = analyze_pe(str(args.exe))
    iat_map = build_iat_map(info)
    log(f'[*] {len(iat_map)} IAT entries')

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    cover = Coverage(img.regions)
    tot = V2Stats()
    dp_stats = type('DP', (), {})()
    dp_stats.dp_scanned = 0
    dp_stats.dp_code_words = 0
    dp_stats.dp_by_section = {}
    dp_stats.dp_rejected = {'mid_instruction': 0, 'int3_or_zero': 0,
                            'undecodable': 0}

    # ---- witness seeds -----------------------------------------------------
    witness_sizes = {}
    if args.witness:
        witness_sizes = load_witness_union(args.witness, out_dir)
    witness_sizes = {va: s for va, s in witness_sizes.items()
                     if img.region_of(va) is not None}
    witness_set = set(witness_sizes) if witness_sizes else None
    witness_sorted = sorted(witness_sizes) if witness_sizes else None
    wleaders = witness_leaders(witness_sizes) if witness_sizes else set()
    if witness_sizes:
        by_region = {}
        for va in witness_sizes:
            r = img.region_of(va)
            by_region[r['name']] = by_region.get(r['name'], 0) + 1
        log(f'[*] witness: {len(witness_sizes):,} executed addresses in the image '
            f'({", ".join(f"{k}={v:,}" for k, v in sorted(by_region.items()))}), '
            f'{len(wleaders):,} block leaders')

    # ---- seeds -------------------------------------------------------------
    entries = {}
    reasons = {}

    def add(va, why):
        if va in entries or img.region_of(va) is None:
            return False
        entries[va] = why
        key = why.split(':')[0]
        reasons[key] = reasons.get(key, 0) + 1
        return True

    def rank(va):
        r = img.region_of(va)
        return (REGION_ORDER.get(r['name'], 9), va)

    add(img.entry_va, 'entry_point')
    for r in img.regions:
        add(r['start'], 'region_start')
    for va, name in all_exports(img):
        add(va, 'export:' + name)
    view = memoryview(img.buf)
    for r in img.regions:
        blob = view[r['start'] - img.base:r['end'] - img.base]
        for va in fast_find_entries(blob, r['start'], r['end']):
            add(va, 'call_or_prologue')
        for va in fast_tail_jumps(blob, r['start'], r['end']):
            add(va, 'tail_jump')
    v1_edges = load_v1_trace_edges()
    for va in v1_edges:
        add(va, 'trace_edge_v1')
    for va in sorted(wleaders):
        add(va, 'witness')
    # Explicit entry seeds (one hex VA per line): targets of indirect jumps,
    # calls and returns observed by the precise engine (PROVEN_ITARGETS census)
    # and by the fast engine's hot-discovery loader.
    for path in (getattr(args, 'seed_file', None) or []):
        n_seed = 0
        for line in open(path):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            n_seed += bool(add(int(line, 16), 'seed'))
        log(f'[*] seed file {path}: {n_seed:,} new entries')
    logt(f'seed: {len(entries):,} entries '
         f'({", ".join(f"{k}={v:,}" for k, v in sorted(reasons.items()))}); '
         f'{len(v1_edges):,} v1 .trace edge targets')

    entry_set = set(entries)

    # ---- parallel lifting --------------------------------------------------
    _W.update(img=img, gen=gen, lift32=lift32, iat_map=iat_map,
              out_dir=out_dir, shard_funcs=args.shard,
              shard_bytes=args.shard_bytes, witness=witness_set,
              witness_sorted=witness_sorted)
    workers = max(1, min(6, int(args.jobs or 1)))

    functions = []          # tuples, see below
    F_VA, F_NAME, F_SIZE, F_INS, F_CLEAN, F_REGION, F_START, F_TOP, \
        F_OPAQUE, F_TRAPS, F_FILE, F_REASON = range(12)
    called_all = set()
    cands = {}
    witness_hit = set()
    witness_conflict = set()
    chunk_seq = 0
    trace_skipped = 0

    def merge_result(res):
        fnames = res['files']
        for fn in res['funcs']:
            idx = fn[10]
            functions.append(fn[:10] + (fnames[idx] if idx < len(fnames) else '',
                                        fn[11]))
        tot.merge(res['stats'])
        for name, ivs in res['cover'].items():
            c = cover.cover[name]
            for s, e in ivs:
                c[s:e] = b'\x01' * (e - s)
        for name, arr in res['starts'].items():
            s = cover.start[name]
            base = region_start[name]
            for a in arr:
                s[a - base] = 1
        witness_hit.update(res['witness_hits'])
        witness_conflict.update(res['witness_conflicts'])
        called_all.update(res['called'])
        for k, vs in res['cands'].items():
            cands.setdefault(k, set()).update(vs)

    def lift_round(pending, rnd):
        nonlocal chunk_seq, trace_skipped
        groups = {}
        for va in pending:
            r = img.region_of(va)
            groups.setdefault(r['name'], []).append((va, entries.get(va, '')))
        tasks = []
        for name in sorted(groups, key=lambda n: REGION_ORDER.get(n, 9)):
            vas = groups[name]
            for i in range(0, len(vas), args.chunk):
                tasks.append((chunk_seq, vas[i:i + args.chunk], name))
                chunk_seq += 1

        def gen_tasks():
            nonlocal trace_skipped
            for cid, vas, name in tasks:
                if (args.trace_budget and name == TRACE_SECTION
                        and tot.trace_instructions >= args.trace_budget):
                    trace_skipped += len(vas)
                    continue
                yield cid, vas

        done = 0
        t_round = time.time()
        _W['entry_set'] = entry_set
        with mpool.get_context('fork').Pool(workers,
                                            initializer=_v2_worker_init) as pool:
            for res in pool.imap_unordered(_v2_worker_lift, gen_tasks(),
                                           chunksize=1):
                merge_result(res)
                done += 1
                if done % 10 == 0 or time.time() - t_round > 30:
                    rate = tot.functions / max(1e-9, time.time() - t_round)
                    logt(f'  round {rnd + 1}: {done}/{len(tasks)} chunks, '
                         f'{tot.functions:,} functions, '
                         f'{tot.instructions:,} instructions ({rate:.0f} fn/s)')
                    t_round = time.time()
        logt(f'round {rnd + 1} done: {tot.functions:,} functions, '
             f'{tot.instructions:,} instructions')

    def plausible(va):
        r = img.region_of(va)
        if r is None:
            return False
        off = va - r['start']
        if cover.cover[r['name']][off]:
            tot_candidates_rejected['already_decoded'] += 1
            return False
        first = img.buf[va - img.base]
        if first in (0x00, 0xCC):
            tot_candidates_rejected['int3_or_zero'] += 1
            return False
        for _ in md.disasm(view[va - img.base:va - img.base + 16], va):
            return True
        tot_candidates_rejected['undecodable'] += 1
        return False

    tot_candidates_rejected = {'already_decoded': 0, 'int3_or_zero': 0,
                               'undecodable': 0}

    def accept(cand_dict):
        new = []
        for why, vas in cand_dict.items():
            for va in vas:
                if va in entry_set:
                    continue
                if not plausible(va):
                    continue
                if add(va, why):
                    new.append(va)
        return new

    pending = sorted(entries, key=rank)
    round_no = 0
    fixpoint = False
    while pending and round_no < args.max_rounds:
        if args.limit:
            pending = pending[:max(0, args.limit - len(functions))]
            if not pending:
                break
        log(f'[*] round {round_no + 1}: {len(pending):,} entries')
        lift_round(pending, round_no)
        round_no += 1
        if args.limit:
            break
        cand = {k: set(v) for k, v in cands.items()}
        cands.clear()
        cand['indirect_target'] = set(tot.indirect_targets)
        cand['code_immediate'] = set(tot.code_immediates)
        if round_no == 1:
            dp = scan_data_pointers(img, md, cover, dp_stats)
            log(f'[*] data-pointer scan: {dp_stats.dp_scanned:,} dwords, '
                f'{dp_stats.dp_code_words:,} code pointers, {len(dp)} accepted '
                f'(rejected {dp_stats.dp_rejected})')
            for va, why in dp.items():
                cand.setdefault(why, set()).add(va)
        new = accept(cand)
        if not new:
            log('[*] fixpoint reached: no new entries')
            fixpoint = True
            break
        entry_set |= set(new)
        pending = sorted(set(new), key=rank)
        log(f'[*] {len(new):,} new entries -> round {round_no + 1}')

    # ---- code nobody references statically ----------------------------------
    # A device image has no hot loader: a function that is only reached through
    # a table built at run time must already be in the image. Every uncovered
    # run that decodes as code becomes an entry (after its int3/nop padding);
    # repeated until the uncovered code-like runs stop shrinking.
    if getattr(args, 'gap_entries', False):
        tried = set()
        for _ in range(12):
            proposals = []
            for r in img.regions:
                cb = cover.cover[r['name']]
                pos, L = 0, len(cb)
                while pos < L:
                    z = cb.find(b'\x00', pos)
                    if z < 0:
                        break
                    e = cb.find(b'\x01', z)
                    if e < 0:
                        e = L
                    pos = e
                    off = r['start'] - img.base
                    while z < e and img.buf[off + z] in (0xCC, 0x90):
                        z += 1
                    if e - z < 8:
                        continue
                    va = r['start'] + z
                    if va in tried or va in entry_set:
                        continue
                    tried.add(va)
                    decoded = 0
                    for insn in md.disasm(bytes(img.buf[off + z:off + min(e, z + 96)]), va):
                        if insn.mnemonic == 'int3' or insn.size == 0:
                            break
                        decoded += 1
                        if decoded >= 6 or insn.mnemonic in ('ret', 'jmp'):
                            break
                    if decoded >= 6 or (decoded >= 1 and insn.mnemonic in ('ret', 'jmp')):
                        proposals.append(va)
            new = [va for va in proposals if add(va, 'gap_code')]
            if not new:
                break
            entry_set |= set(new)
            log(f'[*] {len(new):,} entries from unreferenced code-like runs -> round {round_no + 1}')
            lift_round(sorted(new, key=rank), round_no)
            round_no += 1

    # ---- witness coverage: iterate until every executed address is an
    # instruction start of some lifted body (or reported as a conflict) ------
    def witness_status():
        covered, uncovered, conflict = 0, [], []
        for va in witness_sizes:
            r = img.region_of(va)
            off = va - r['start']
            if cover.start[r['name']][off]:
                covered += 1
            elif cover.cover[r['name']][off]:
                conflict.append(va)
            else:
                uncovered.append(va)
        return covered, uncovered, conflict

    w_covered, w_uncovered, w_conflict = (0, [], [])
    if witness_sizes:
        for extra in range(max(0, args.witness_rounds) + 1):
            w_covered, w_uncovered, w_conflict = witness_status()
            log(f'[*] witness coverage: {w_covered:,}/{len(witness_sizes):,} '
                f'covered, {len(w_uncovered):,} not lifted, '
                f'{len(w_conflict):,} mid-instruction')
            if not w_uncovered or extra == args.witness_rounds:
                break
            if args.limit:
                break
            add_set = [va for va in w_uncovered if plausible(va)]
            if not add_set:
                break
            for va in add_set:
                add(va, 'witness_uncovered')
            entry_set |= set(add_set)
            log(f'[*] witness round {extra + 1}: lifting {len(add_set):,} '
                f'additional executed addresses')
            lift_round(sorted(add_set, key=rank), round_no)
            round_no += 1

    # ---- every discovered entry must be dispatch-reachable ----------------
    # An entry whose extent walk reached no instruction (int3 padding, data, a
    # byte the decoder rejects) still has to exist as a dispatch row: an
    # indirect jump/call into it must find something, and a RECOMP_CALL to it
    # must resolve.  It becomes a named trap stub -- never a silent no-op.
    emitted = {fn[F_NAME] for fn in functions}
    stub_funcs = []
    stub_vas = []
    for va in sorted(entry_set):
        name = f'sub_{va:08X}'
        if name in emitted:
            continue
        r = img.region_of(va)
        stub_vas.append(va)
        stub_funcs.append((
            f'/* NO BODY: {name} at 0x{va:08X} ({r["name"]}) -- the extent walk '
            f'reached no instruction.  Emitted as a named trap so the address '
            f'stays dispatch-reachable and RECOMP_CALL resolves. */\n'
            f'void {name}(void) {{ '
            f'recomp_unsupported(0x{va:08X}u, "no-body-entry"); abort(); }}\n',
            va, name))
        functions.append((va, name, 0, 0, False, r['name'], va, va, None, 0,
                          'recomp_stubs.c', 'no_body_stub'))
    if stub_funcs:
        write_shard_named(out_dir, 'recomp_stubs.c', stub_funcs)
    log(f'[*] {len(stub_vas):,} entries without a body -> named trap stubs '
        f'(of {len(entry_set):,} entries)')

    # ---- call fixup (safety net): RECOMP_CALL to a name with no body -------
    missing = {f'sub_{va:08X}' for va in entry_set} - {fn[F_NAME] for fn in functions}
    rewritten = 0
    if missing:
        call_re = re.compile(r'RECOMP_CALL\((sub_[0-9A-F]{8})\)')

        def _fix(m):
            if m.group(1) in missing:
                return 'RECOMP_ICALL(0x%08Xu)' % int(m.group(1)[4:], 16)
            return m.group(0)

        for fn_file in sorted({fn[F_FILE] for fn in functions}):
            path = out_dir / fn_file
            try:
                text = path.read_text()
            except OSError:
                continue
            if 'RECOMP_CALL(' not in text:
                continue
            new_text, n = call_re.subn(_fix, text)
            if n:
                rewritten += n
                path.write_text(new_text)
    log(f'[*] {rewritten} RECOMP_CALL targets had no body -> RECOMP_ICALL '
        f'({len(missing)} unresolved names)')

    # ---- coverage / uncovered runs ----------------------------------------
    coverage = {}
    uncovered_runs = []
    uncovered_kinds = {}
    uncovered_by_region = {}
    total_runs = 0
    total_bytes = 0
    for r in img.regions:
        cb = bytes(cover.cover[r['name']])
        covered = cb.count(1)
        coverage[r['name']] = {'bytes': r['size'],
                               'covered_by_instructions': covered,
                               'fraction': round(covered / max(1, r['size']), 4),
                               'data_or_padding': r['size'] - covered}
        pos = 0
        L = len(cb)
        while pos < L:
            z = cb.find(b'\x00', pos)
            if z < 0:
                break
            e = cb.find(b'\x01', z)
            if e < 0:
                e = L
            ln = e - z
            total_runs += 1
            total_bytes += ln
            if ln >= 16:
                blob = bytes(img.buf[r['start'] - img.base + z:
                                     r['start'] - img.base + e])
                if blob.count(0) == ln:
                    kind = 'zero'
                elif blob.count(0xCC) == ln:
                    kind = 'int3_padding'
                else:
                    kind = 'other'
                    decoded = 0
                    for insn in md.disasm(blob[:64], r['start'] + z):
                        if insn.mnemonic == 'int3' or insn.size == 0:
                            break
                        decoded += 1
                        if decoded >= 8:
                            break
                    if decoded >= 8:
                        kind = 'code_like'
                k = uncovered_kinds.setdefault(kind, {'runs': 0, 'bytes': 0})
                k['runs'] += 1
                k['bytes'] += ln
                rk = uncovered_by_region.setdefault(
                    r['name'], {}).setdefault(kind, {'runs': 0, 'bytes': 0})
                rk['runs'] += 1
                rk['bytes'] += ln
                uncovered_runs.append({'region': r['name'],
                                       'start': r['start'] + z,
                                       'length': ln, 'kind': kind,
                                       'preview': blob[:16].hex()})
            else:
                k = uncovered_kinds.setdefault('small_gap', {'runs': 0, 'bytes': 0})
                k['runs'] += 1
                k['bytes'] += ln
                rk = uncovered_by_region.setdefault(
                    r['name'], {}).setdefault('small_gap', {'runs': 0, 'bytes': 0})
                rk['runs'] += 1
                rk['bytes'] += ln
            pos = e
    uncovered_runs.sort(key=lambda u: -u['length'])

    # ---- headers, dispatch, function table --------------------------------
    _atomic_write(out_dir / 'recomp_unsupported.h', TRAP_HEADER)
    _atomic_write(out_dir / 'recomp_unsupported.c', TRAP_SOURCE)

    tmp = out_dir / 'recomp_funcs.h.tmp'
    with open(tmp, 'w') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write(f'/* {len(functions)} recompiled functions */\n\n')
        for fn in functions:
            f.write(f'void {fn[F_NAME]}(void);  /* 0x{fn[F_VA]:08X} */\n')
    tmp.replace(out_dir / 'recomp_funcs.h')

    tmp = out_dir / 'recomp_dispatch.c.tmp'
    with open(tmp, 'w') as f:
        f.write('/* Auto-generated by tools/static_full_lift.py --v2 - DO NOT EDIT */\n')
        f.write('#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write('const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for fn in sorted(functions, key=lambda d: d[F_VA]):
            f.write(f'    {{ 0x{fn[F_VA]:08X}u, {fn[F_NAME]} }},\n')
        f.write('};\n\n')
        f.write(f'const uint32_t recomp_dispatch_count = {len(functions)};\n')
    tmp.replace(out_dir / 'recomp_dispatch.c')

    tmp = out_dir / 'functions.json.tmp'
    with open(tmp, 'w') as f:
        f.write('[\n')
        for i, fn in enumerate(functions):
            rec = {'va': f'0x{fn[F_VA]:08X}', 'va_int': fn[F_VA],
                   'name': fn[F_NAME], 'size': fn[F_SIZE],
                   'instructions': fn[F_INS], 'tu': fn[F_FILE],
                   'clean_extent': fn[F_CLEAN], 'region': fn[F_REGION],
                   'body_start': f'0x{fn[F_START]:08X}',
                   'extent_end': f'0x{fn[F_TOP]:08X}',
                   'discovery_reason': fn[F_REASON],
                   'opaque': fn[F_OPAQUE], 'traps': fn[F_TRAPS]}
            f.write(('' if i == 0 else ',\n') + json.dumps(rec))
        f.write('\n]\n')
    tmp.replace(out_dir / 'functions.json')

    # ---- cross-region edges (.code <-> .trace in both directions) ---------
    edge_counts = {}
    for _, _, _, _, _, a, b in tot.cross_edges:
        edge_counts[f'{a}->{b}'] = edge_counts.get(f'{a}->{b}', 0) + 1
    edges_doc = {
        'note': 'direct call/jump/conditional-jump edges between lift regions '
                '(.code <-> .trace in both directions). Intra-region edges are '
                'counted only, not listed.',
        'count': tot.cross_edges_total,
        'listed': len(tot.cross_edges),
        'truncated': tot.cross_edges_total > len(tot.cross_edges),
        'by_region_pair': dict(sorted(edge_counts.items())),
        'all_edges_by_kind': dict(sorted(tot.edge_kinds.items())),
        'edges': [{'from': f'0x{s:08X}', 'function': f'0x{fn:08X}',
                   'kind': k, 'mnemonic': m, 'target': f'0x{t:08X}',
                   'from_region': a, 'to_region': b}
                  for s, fn, k, m, t, a, b in
                  sorted(tot.cross_edges, key=lambda e: e[0])],
    }
    _atomic_write(out_dir / 'edges.json', json.dumps(edges_doc, indent=1))
    trace_edges = [e for e in edges_doc['edges']
                   if e['to_region'] == TRACE_SECTION or e['from_region'] == TRACE_SECTION]
    _atomic_write(out_dir / 'trace_edges.json', json.dumps({
        'trace_section': {'name': TRACE_SECTION,
                          'start': img.trace['start'] if img.trace else 0,
                          'end': img.trace['end'] if img.trace else 0},
        'note': 'every direct cross-region edge that touches .trace, in both '
                'directions; .trace is lifted like any other region here',
        'count': len(trace_edges),
        'edges': trace_edges}, indent=1))

    # ---- stats.json (v2) ---------------------------------------------------
    elapsed = time.time() - started
    by_reason = {}
    for fn in functions:
        key = fn[F_REASON].split(':')[0] or 'unknown'
        by_reason[key] = by_reason.get(key, 0) + 1
    v1_cov = None
    if V1_STATS.exists():
        try:
            v1_cov = json.loads(V1_STATS.read_text()).get('coverage_by_region')
        except (ValueError, OSError):
            v1_cov = None

    # entries that live inside another function's body: they are dispatch
    # entries of their own (that is how an indirect jump into the middle of a
    # function stays reachable through recomp_lookup), not alias stubs.
    spans = sorted((fn[F_START], fn[F_TOP], fn[F_VA]) for fn in functions)
    span_starts = [s[0] for s in spans]
    inside = 0
    for fn in functions:
        i = bisect.bisect_right(span_starts, fn[F_VA]) - 1
        if i >= 0 and spans[i][0] < fn[F_VA] < spans[i][1] and spans[i][2] != fn[F_VA]:
            inside += 1

    stats_doc = {
        'tool': 'tools/static_full_lift.py --v2',
        'image': str(img.path.relative_to(ROOT)) if str(img.path).startswith(str(ROOT))
                 else str(img.path),
        'image_base': f'0x{img.base:08X}',
        'entry_point': f'0x{img.entry_va:08X}',
        'lift_regions': [{'name': r['name'], 'start': f'0x{r["start"]:08X}',
                          'end': f'0x{r["end"]:08X}', 'bytes': r['size']}
                         for r in img.regions],
        'workers': workers,
        'discovery': {
            'entries_total': len(entry_set),
            'by_reason': reasons,
            'functions_by_reason': dict(sorted(by_reason.items())),
            'relocation_targets_in_code': len(relocation_targets(img)),
            'relocation_directory_present': bool(
                img.pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size),
            'fixpoint_reached': fixpoint,
            'rounds': round_no,
        },
        'witness_uncovered_file': 'witness_uncovered.json',
        'witness': {
            'note': 'executed instruction addresses from the private guest '
                    'traces; block leaders are seeded as entries and every '
                    'address must end up as an instruction start of a lifted '
                    'function',
            'sources': [str(p) for p in (args.witness or [])],
            'witness_total': len(witness_sizes),
            'witness_leaders': len(wleaders),
            'witness_covered': w_covered,
            'witness_uncovered': len(w_uncovered),
            'witness_mid_instruction_conflicts': len(w_conflict),
            'covered_fraction': round(w_covered / max(1, len(witness_sizes)), 6),
            'by_region': ({name: sum(1 for va in witness_sizes
                                     if img.region_of(va)['name'] == name)
                           for name in sorted({img.region_of(va)['name']
                                               for va in witness_sizes})}
                          if witness_sizes else {}),
        },
        'data_pointers': {
            'dwords_scanned': dp_stats.dp_scanned,
            'code_pointers_found': dp_stats.dp_code_words,
            'accepted_by_reason': {
                k: sum(1 for fn in functions if fn[F_REASON].split(':')[0] == k)
                for k in ('data_pointer', 'vtable')},
            'rejected': dp_stats.dp_rejected,
            'by_section': dict(sorted(dp_stats.dp_by_section.items())),
        },
        'exports': {
            'total': len(all_exports(img)),
            'in_lift_regions': sum(1 for va, _ in all_exports(img)
                                   if img.region_of(va) is not None),
            'in_trace': sum(1 for va, _ in all_exports(img) if img.in_trace(va)),
            'outside_image_or_forwarder': sum(
                1 for va, _ in all_exports(img) if img.region_of(va) is None),
        },
        'functions': len(functions),
        'entries_without_body': tot.empty_entries,
        'entries_without_body_stubs': len(stub_vas),
        'entries_pending_at_round_cap': (0 if (fixpoint or args.limit)
                                         else len(pending)),
        'recall_rewritten_to_icall': rewritten,
        'functions_by_region': {
            name: sum(1 for fn in functions if fn[F_REGION] == name)
            for name in sorted({fn[F_REGION] for fn in functions})},
        'instructions_by_region': {
            name: sum(fn[F_INS] for fn in functions if fn[F_REGION] == name)
            for name in sorted({fn[F_REGION] for fn in functions})},
        'functions_inside_other_bodies': {
            'count': inside,
            'note': 'entries whose start lies inside another function extent. '
                    'Each is its own dispatch entry (recomp_dispatch_table), so '
                    'an indirect jump/call into the middle of a function is '
                    'reachable through recomp_lookup; alias_stub is not used.',
        },
        'translation_units': len({fn[F_FILE] for fn in functions}),
        'functions_per_tu': args.shard,
        'instructions': tot.instructions,
        'instructions_clean_extent': tot.clean,
        'functions_unclean_extent': tot.unclean,
        'instructions_trimmed_after_ret_or_int3': tot.trimmed,
        'trace_region': {
            'bytes': img.trace['end'] - img.trace['start'] if img.trace else 0,
            'functions': sum(1 for fn in functions if fn[F_REGION] == TRACE_SECTION),
            'instructions': tot.trace_instructions,
            'skipped_by_budget': trace_skipped,
            'budget': args.trace_budget,
        },
        'opaque_functions': {
            'total': tot.opaque,
            'by_reason': tot.opaque_reasons,
            'untranslatable_instructions_inside': tot.opaque_unsupported,
            'by_region': {
                name: sum(1 for fn in functions
                          if fn[F_OPAQUE] and fn[F_REGION] == name)
                for name in sorted({fn[F_REGION] for fn in functions})},
            'samples': [dict(s, va=f'0x{s["va"]:08X}') for s in tot.opaque_samples],
        },
        'unsupported_instructions': {
            'note': 'every untranslatable instruction is a named trap '
                    '(recomp_unsupported + abort), including the SSE/AVX form '
                    'that v1 left as a silent null statement',
            'total': sum(tot.unsupported.values()),
            'by_mnemonic': dict(sorted(tot.unsupported.items(),
                                       key=lambda kv: -kv[1])),
            'sample_vas': {k: [f'0x{v:08X}' for v in vs]
                           for k, vs in sorted(tot.unsupported_samples.items())},
        },
        'sparse_extents': {
            'note': 'functions whose extent span is less than 50% instruction '
                    'bytes (embedded jump tables / data between instructions). '
                    'They are lifted normally, not trapped.',
            'count': tot.sparse_extents,
            'samples': [dict(x, va=f'0x{x["va"]:08X}') for x in tot.sparse_samples],
        },
        'clflush_barriers': {
            'total': tot.clflush,
            'emitted_as': 'memory no-op: __atomic_thread_fence(__ATOMIC_SEQ_CST)',
            'sample_vas': [f'0x{v:08X}' for v in tot.clflush_samples],
        },
        'edges': {
            'cross_region_total': tot.cross_edges_total,
            'listed': len(tot.cross_edges),
            'by_region_pair': dict(sorted(edge_counts.items())),
            'all_by_kind': dict(sorted(tot.edge_kinds.items())),
        },
        'coverage_note': 'byte coverage counts only bytes that belong to a '
                          'lifted instruction.  .code is 9,097,216 bytes of '
                          'which ~3.27 MB is int3 alignment padding and ~2.4 MB '
                          'is data (jump tables/strings/vtables inside the code '
                          'section), so 35% byte coverage is ~98% of the bytes '
                          'that are function extents.  .trace is partly '
                          'encrypted/obfuscated payload: the unexecuted part does '
                          'not decode to sane x86 and is not invented.',
        'coverage_by_region': coverage,
        'coverage_v1_comparison': (
            {name: {'v1_fraction': round(v1_cov.get(name, {}).get(
                        'covered_by_instructions', 0)
                    / max(1, v1_cov.get(name, {}).get('bytes', 1)), 4),
                    'v2_fraction': coverage.get(name, {}).get('fraction')}
             for name in sorted(set(v1_cov or {}) | set(coverage))}
            if v1_cov else None),
        'uncovered_runs': {
            'runs': total_runs,
            'bytes': total_bytes,
            'by_kind': dict(sorted(uncovered_kinds.items())),
            'by_region_kind': {k: dict(sorted(v.items()))
                               for k, v in sorted(uncovered_by_region.items())},
            'largest': [{'region': u['region'], 'start': f'0x{u["start"]:08X}',
                         'length': u['length'], 'kind': u['kind'],
                         'preview': u['preview']}
                        for u in uncovered_runs[:25]],
        },
        'lift_errors': tot.lift_errors[:50],
        'lift_errors_total': len(tot.lift_errors),
        'code_immediate_targets': len(tot.code_immediates),
        'code_immediates_note': 'immediate operands pointing into code are '
                                'discovery evidence only in compiler-generated '
                                'regions (.code/.srdata/.text1/.ooa); the '
                                'obfuscated .trace region contributes none',
        'code_immediates_ignored_in_trace': tot.trace_immediates_ignored,
        'candidates_rejected': tot_candidates_rejected,
        'indirect_targets_seen': len(tot.indirect_targets),
        'seconds': round(elapsed, 1),
    }
    _atomic_write(out_dir / 'witness_uncovered.json', json.dumps({
        'note': 'executed addresses that are not an instruction start of any '
                'lifted function, and executed addresses that fall inside a '
                'lifted instruction (a real disagreement between the trace and '
                'the linear disassembly, reported not patched)',
        'witness_total': len(witness_sizes),
        'covered': w_covered,
        'uncovered': [{'va': f'0x{va:08X}',
                       'region': img.region_of(va)['name']}
                      for va in sorted(w_uncovered)],
        'mid_instruction_conflicts': [{'va': f'0x{va:08X}',
                                       'region': img.region_of(va)['name']}
                                      for va in sorted(w_conflict)],
    }, indent=1))
    _atomic_write(out_dir / 'stats.json', json.dumps(stats_doc, indent=1))

    log(f'[*] === V2 COMPLETE === {len(functions):,} functions, '
        f'{tot.instructions:,} instructions, {stats_doc["translation_units"]} TUs, '
        f'{tot.opaque} opaque, {tot.clflush} clflush barriers, '
        f'{sum(tot.unsupported.values())} traps, '
        f'{tot.cross_edges_total:,} cross-region edges, {elapsed:.0f}s')
    for name, c in coverage.items():
        log(f'    coverage {name:<8} {c["fraction"] * 100:5.1f}% '
            f'({c["covered_by_instructions"]:,}/{c["bytes"]:,} bytes)')

    # ---- syntax check (after every artifact is on disk) --------------------
    if args.syntax_check:
        tu_paths = sorted({out_dir / fn[F_FILE] for fn in functions})
        include_dirs = [UPSTREAM / 'runtime' / 'recomp32', out_dir]
        check_paths = list(tu_paths) + [out_dir / 'recomp_dispatch.c',
                                        out_dir / 'recomp_unsupported.c']
        log(f'[*] syntax check of {len(check_paths)} TUs ({workers} parallel)')
        res = syntax_check_all(check_paths, include_dirs, ndk=args.android_ndk,
                               jobs=workers)
        stats_doc['syntax_check'] = res
        _atomic_write(out_dir / 'stats.json', json.dumps(stats_doc, indent=1))
        log(f'[*] syntax check: ok={res.get("ok")} '
            f'({res.get("checked")} TUs, {res.get("failed", "?")} failed, '
            f'{res.get("seconds")}s)')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', type=Path, default=DEFAULT_EXE, required=DEFAULT_EXE is None)
    ap.add_argument('--out', type=Path, default=None,
                    help='output directory (default: static-full-v1, or '
                         'static-full-v2 with --v2)')
    ap.add_argument('--shard', type=int, default=SHARD,
                    help='functions per translation unit (default %(default)s)')
    ap.add_argument('--limit', type=int, default=0,
                    help='lift at most N entries (benchmarking)')
    ap.add_argument('--no-extent', action='store_true',
                    help="use generate.py main()'s next-entry clamp instead of true_extent")
    ap.add_argument('--syntax-check', action='store_true', default=None,
                    help='compile a sample of TUs with host clang -fsyntax-only '
                         '(v2: on by default, use --no-syntax-check to skip)')
    ap.add_argument('--android-ndk', type=Path, default=None,
                    help='NDK root for the syntax check (default %s)' % DEFAULT_NDK)
    ap.add_argument('--max-rounds', type=int, default=None,
                    help='discovery rounds (default %d, v2: 20)' % MAX_ROUNDS)
    ap.add_argument('--v2', action='store_true',
                    help='v2: data-pointer discovery, .trace as a lift region, '
                         'opaque-function traps, clflush barriers')
    ap.add_argument('--jobs', type=int, default=6,
                    help='parallel lift workers / syntax-check jobs (capped at '
                         '6, default 6)')
    ap.add_argument('--trace-budget', type=int, default=0,
                    help='v2: stop lifting .trace after N instructions (0 = no cap)')
    ap.add_argument('--witness', action='append', default=[],
                    metavar='TRACE',
                    help='v2: a guest trace.json (or a directory/glob of them) '
                         'whose executed instruction addresses seed discovery '
                         'and are checked for coverage; repeatable')
    ap.add_argument('--seed-file', action='append', default=[],
                    help='file with one hex VA per line; each becomes a function entry')
    ap.add_argument('--witness-rounds', type=int, default=3,
                    help='v2: extra rounds lifting executed addresses that are '
                         'still uncovered (default 3)')
    ap.add_argument('--gap-entries', action='store_true',
                    help='also lift unreferenced code-like runs (for images without a hot loader)')
    ap.add_argument('--chunk', type=int, default=200,
                    help='v2: entries per parallel lift task (default 200)')
    ap.add_argument('--shard-bytes', type=int, default=4 << 20,
                    help='v2: also close a shard file at this many C bytes '
                         '(default 4 MiB)')
    ap.add_argument('--no-syntax-check', dest='syntax_check',
                    action='store_false',
                    help='skip the syntax check (default: run it when --v2)')
    ap.add_argument('--sections', default='',
                    help='comma list of PE sections to lift instead of the EXE defaults (e.g. .text for a DLL)')
    ap.add_argument('--dispatch-prefix', default='recomp',
                    help='symbol prefix of the dispatch table (use e.g. dbdata for a second image)')
    args = ap.parse_args(argv)
    if args.sections:
        global LIFT_SECTIONS
        LIFT_SECTIONS = tuple(x for x in args.sections.split(',') if x)
        REGION_ORDER.update({n: i for i, n in enumerate(LIFT_SECTIONS)})
    if args.out is None:
        args.out = V2_OUT if args.v2 else DEFAULT_OUT
    if args.v2:
        if args.syntax_check is None:
            args.syntax_check = True
        if args.max_rounds is None:
            args.max_rounds = 20
        return run_v2(args)
    if args.max_rounds is None:
        args.max_rounds = MAX_ROUNDS
    if args.syntax_check is None:
        args.syntax_check = False

    gen, lift32, analyze_pe, build_iat_map = load_upstream()
    assert gen.EXTENT_REACH == EXTENT_REACH, (gen.EXTENT_REACH, EXTENT_REACH)

    started = time.time()
    img = Image(args.exe)
    out_dir = args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    log(f'[*] image 0x{img.base:08X} entry 0x{img.entry_va:08X} '
        f'{img.file_size:,} bytes, {len(img.sections)} sections')
    for r in img.regions:
        log(f'    lift region {r["name"]:<8} 0x{r["start"]:08X}-0x{r["end"]:08X} '
            f'({r["size"]:,} bytes)')
    if img.trace:
        log(f'    skip region {img.trace["name"]:<8} '
            f'0x{img.trace["start"]:08X}-0x{img.trace["end"]:08X} '
            f'({img.trace["end"] - img.trace["start"]:,} bytes, not lifted)')

    info = analyze_pe(str(args.exe))
    iat_map = build_iat_map(info)
    log(f'[*] {len(iat_map)} IAT entries')

    entries, reasons, relocs = discover_entries(img, gen)
    log(f'[*] {len(entries)} function entries '
        f'({", ".join(f"{k}={v}" for k, v in sorted(reasons.items()))})')
    if not relocs:
        log('[*] no base relocations in the image (reloc directory empty)')

    md = __import__('capstone').Cs(__import__('capstone').CS_ARCH_X86,
                                   __import__('capstone').CS_MODE_32)
    md.detail = True
    lifter = lift32.Lifter(iat_map=iat_map, lifted=set(entries))

    stats = Stats()
    stats.covered = {r['name']: bytearray(r['size']) for r in img.regions}
    functions = []          # (va, name, tu, size, instructions, clean, region, round)
    shard_funcs = []
    shard_index = 0
    tu_paths = []

    def flush():
        nonlocal shard_funcs, shard_index
        if not shard_funcs:
            return
        tu_paths.append(write_shard(out_dir, shard_index, shard_funcs))
        shard_index += 1
        shard_funcs = []

    pending = sorted(entries)
    done = set()
    entry_set = set(entries)
    for rnd in range(max(1, args.max_rounds)):
        if not pending:
            break
        log(f'[*] round {rnd + 1}: {len(pending)} entries')
        round_called = set()
        t_round = time.time()
        for n, va in enumerate(pending):
            if args.limit and stats.functions >= args.limit:
                break
            done.add(va)
            text, called, meta = lift_one(gen, lift32, md, img, lifter, va,
                                         entry_set, stats,
                                         use_extent=not args.no_extent)
            round_called |= called
            if text is None:
                if meta.get('error'):
                    name = f'sub_{va:08X}'
                    text = (f'/* LIFT ERROR: {name} at 0x{va:08X}: {meta["error"]} */\n'
                            f'void {name}(void) {{ '
                            f'recomp_unsupported(0x{va:08X}u, "lift-error"); abort(); }}\n')
                    functions.append((va, name, shard_index, 0, 0, False,
                                      img.region_of(va)['name'], rnd))
                    shard_funcs.append((text, va, name))
                    if len(shard_funcs) >= args.shard:
                        flush()
                continue
            name = f'sub_{va:08X}'
            functions.append((va, name, shard_index, meta['size'],
                              meta['instructions'], meta['clean'],
                              meta['region'], rnd))
            shard_funcs.append((text, va, name))
            if len(shard_funcs) >= args.shard:
                flush()
            if (n + 1) % 2000 == 0:
                rate = stats.functions / max(1e-9, time.time() - t_round)
                log(f'    {n + 1}/{len(pending)} entries, {stats.functions} functions, '
                    f'{stats.instructions:,} instructions, {rate:.0f} fn/s')
        flush()
        log(f'[*] round {rnd + 1} done in {time.time() - t_round:.0f}s: '
            f'{stats.functions} functions, {stats.instructions:,} instructions')

        # Next round: indirect tail/call targets the lifted code refers to that
        # no walk decoded and no catalog listed.
        if args.limit:
            break
        candidates = set(stats.indirect_targets) | round_called
        new = sorted(t for t in candidates
                     if img.in_code(t) and t not in entry_set
                     and t not in stats.decoded)
        if not new:
            break
        log(f'[*] {len(new)} indirect targets not catalogued -> next round')
        entry_set |= set(new)
        lifter.lifted = entry_set
        pending = new

    # ---- indirect calls to functions that produced no body must not stay
    # RECOMP_CALL: the symbol would be undeclared. Degrade them to the dispatch
    # path, which reports an unresolved VA at runtime instead of failing to link.
    emitted = {name for _, name, *_ in functions}
    call_re = re.compile(r'RECOMP_CALL\((sub_[0-9A-F]{8})\)')
    rewritten = 0

    def _fix(m):
        nonlocal rewritten
        name = m.group(1)
        if name in emitted:
            return m.group(0)
        rewritten += 1
        return 'RECOMP_ICALL(0x%08Xu)' % int(name[4:], 16)

    for path in tu_paths:
        text = path.read_text()
        if 'RECOMP_CALL(' not in text:
            continue
        new_text = call_re.sub(_fix, text)
        if new_text != text:
            path.write_text(new_text)
    log(f'[*] {rewritten} RECOMP_CALL targets had no body -> RECOMP_ICALL')

    # ---- decode coverage / undecodable instructions
    decode_failures = sorted(stats.reached - stats.decoded - stats.trimmed_addresses)
    coverage = {}
    uncovered_runs = []
    undecoded_bytes = 0
    for r in img.regions:
        cover = stats.covered.get(r['name'], bytearray(r['size']))
        covered = cover.count(1)
        coverage[r['name']] = {'bytes': r['size'], 'covered_by_instructions': covered,
                               'data_or_padding': r['size'] - covered}
        undecoded_bytes += r['size'] - covered
        # Runs of bytes no lifted instruction covers: data, padding, jump
        # tables -- or code no discovery rule found. Classified, not assumed.
        pos = 0
        while pos < r['size']:
            if cover[pos]:
                pos += 1
                continue
            start = pos
            while pos < r['size'] and not cover[pos]:
                pos += 1
            blob = bytes(img.buf[r['start'] - img.base + start:
                                 r['start'] - img.base + pos])
            if blob.count(0) == len(blob):
                kind = 'zero'
            elif blob.count(0xCC) == len(blob):
                kind = 'int3_padding'
            else:
                kind = 'other'
                # Does the run start with a straight-line instruction sequence?
                # Then it is code no discovery rule reached (a function only
                # called through a pointer), not data -- reported, not lifted.
                decoded = 0
                for insn in md.disasm(blob[:64], r['start'] + start):
                    if insn.mnemonic == 'int3' or insn.size == 0:
                        break
                    decoded += 1
                    if decoded >= 8:
                        break
                if decoded >= 8:
                    kind = 'code_like'
            uncovered_runs.append({'region': r['name'], 'start': r['start'] + start,
                                   'length': pos - start, 'kind': kind,
                                   'preview': blob[:16].hex()})

    uncovered_runs.sort(key=lambda u: -u['length'])
    uncovered_total = sum(u['length'] for u in uncovered_runs)
    uncovered_kinds = {}
    for u in uncovered_runs:
        k = uncovered_kinds.setdefault(u['kind'], {'runs': 0, 'bytes': 0})
        k['runs'] += 1
        k['bytes'] += u['length']

    # ---- header + dispatch
    (out_dir / 'recomp_unsupported.h').write_text(TRAP_HEADER)
    (out_dir / 'recomp_unsupported.c').write_text(TRAP_SOURCE)

    with open(out_dir / 'recomp_funcs.h', 'w') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write(f'/* {len(functions)} recompiled functions */\n\n')
        for va, name, *_ in functions:
            f.write(f'void {name}(void);  /* 0x{va:08X} */\n')

    with open(out_dir / 'recomp_dispatch.c', 'w') as f:
        f.write('/* Auto-generated by tools/static_full_lift.py - DO NOT EDIT */\n')
        f.write('#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write(f'const recomp_dispatch_entry_t {args.dispatch_prefix}_dispatch_table[] = {{\n')
        for va, name, *_ in sorted(functions):
            f.write(f'    {{ 0x{va:08X}u, {name} }},\n')
        f.write('};\n\n')
        f.write(f'const uint32_t {args.dispatch_prefix}_dispatch_count = {len(functions)};\n')

    # ---- functions.json
    with open(out_dir / 'functions.json', 'w') as f:
        json.dump([{
            'va': f'0x{va:08X}',
            'va_int': va,
            'name': name,
            'size': size,
            'instructions': insns,
            'tu': f'recomp_{tu:04d}.c',
            'clean_extent': clean,
            'region': region,
            'discovery_round': rnd,
        } for va, name, tu, size, insns, clean, region, rnd in functions], f, indent=1)

    # ---- trace_edges.json
    trace_edges = sorted(stats.trace_edges, key=lambda e: e['from'])
    with open(out_dir / 'trace_edges.json', 'w') as f:
        json.dump({
            'trace_section': img.trace,
            'note': 'direct call/jump/conditional-jump edges from lifted code into '
                    '.trace; .trace itself is not lifted',
            'count': len(trace_edges),
            'edges': [dict(e, **{'from': f'0x{e["from"]:08X}',
                                 'function': f'0x{e["function"]:08X}',
                                 'target': f'0x{e["target"]:08X}'})
                      for e in trace_edges],
        }, f, indent=1)

    # ---- stats.json
    elapsed = time.time() - started
    stats_doc = {
        'image': str(img.path.relative_to(ROOT)) if str(img.path).startswith(str(ROOT))
                 else str(img.path),
        'image_sha256_note': 'see port/modules.sha256',
        'image_base': f'0x{img.base:08X}',
        'entry_point': f'0x{img.entry_va:08X}',
        'lift_regions': [{'name': r['name'], 'start': f'0x{r["start"]:08X}',
                          'end': f'0x{r["end"]:08X}', 'bytes': r['size']}
                         for r in img.regions],
        'not_lifted': ([{'name': img.trace['name'],
                         'start': f'0x{img.trace["start"]:08X}',
                         'end': f'0x{img.trace["end"]:08X}',
                         'bytes': img.trace['end'] - img.trace['start'],
                         'reason': 'protected region, measured only'}]
                       if img.trace else []),
        'discovery': {
            'entries_total': len(entry_set),
            'by_reason': reasons,
            'relocation_targets_in_code': len(relocs),
            'relocation_directory_present': bool(
                img.pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size),
        },
        'exports': {
            'total': len(all_exports(img)),
            'in_lift_regions': len(export_targets(img)),
            'in_trace_not_lifted': sum(1 for va, _ in all_exports(img)
                                       if img.in_trace(va)),
            'outside_image_or_forwarder': sum(
                1 for va, _ in all_exports(img)
                if not img.in_code(va) and not img.in_trace(va)),
        },
        'functions': len(functions),
        'entries_without_body': stats.empty_entries,
        'recall_rewritten_to_icall': rewritten,
        'functions_by_region': {
            name: sum(1 for f in functions if f[6] == name)
            for name in sorted({f[6] for f in functions})
        },
        'functions_by_round': {
            str(r): sum(1 for f in functions if f[7] == r)
            for r in sorted({f[7] for f in functions})
        },
        'translation_units': len(tu_paths),
        'functions_per_tu': args.shard,
        'instructions': stats.instructions,
        'instructions_clean_extent': stats.clean,
        'functions_unclean_extent': stats.unclean,
        'instructions_trimmed_after_ret_or_int3': stats.trimmed,
        'unsupported_instructions': {
            'total': sum(stats.unsupported.values()),
            'by_mnemonic': dict(sorted(stats.unsupported.items(),
                                       key=lambda kv: -kv[1])),
            'sample_vas': {k: [f'0x{v:08X}' for v in vs]
                           for k, vs in sorted(stats.unsupported_samples.items())},
        },
        'undecodable': {
            'reached_but_not_decoded': len(decode_failures),
            'bytes_not_covered_by_lifted_instructions': undecoded_bytes,
            'sample_addresses': [f'0x{v:08X}' for v in decode_failures[:50]],
        },
        'coverage_by_region': coverage,
        'uncovered_runs': {
            'runs': len(uncovered_runs),
            'bytes': uncovered_total,
            'by_kind': uncovered_kinds,
            'largest': [{'region': u['region'],
                         'start': f'0x{u["start"]:08X}',
                         'length': u['length'],
                         'kind': u['kind'],
                         'preview': u['preview']}
                        for u in uncovered_runs[:25]],
        },
        'lift_errors': stats.lift_errors[:50],
        'lift_errors_total': len(stats.lift_errors),
        'trace_edges': len(trace_edges),
        'trace_edges_from_functions': len({e['function'] for e in trace_edges}),
        'indirect_targets_seen': len(stats.indirect_targets),
        'seconds': round(elapsed, 1),
    }
    if args.syntax_check:
        include_dirs = [UPSTREAM / 'runtime' / 'recomp32', out_dir]
        stats_doc['syntax_check'] = syntax_check(out_dir, tu_paths, include_dirs,
                                                 ndk=args.android_ndk)
        log(f'[*] syntax check: {stats_doc["syntax_check"]["ok"]}')
    with open(out_dir / 'stats.json', 'w') as f:
        json.dump(stats_doc, f, indent=1)

    log(f'[*] === COMPLETE === {len(functions)} functions, '
        f'{stats.instructions:,} instructions, {len(tu_paths)} TUs, '
        f'{sum(stats.unsupported.values())} unsupported, '
        f'{len(decode_failures)} undecodable, {len(trace_edges)} .trace edges, '
        f'{elapsed:.0f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
