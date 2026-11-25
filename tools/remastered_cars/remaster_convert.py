#!/usr/bin/env python3
"""Convert a Remastered PC (D3D11-layout) graphics/attribute bundle into the
Ultimate Box PC (D3D9-layout) form. Private tool: input = the user's own
Remastered files, output = port-mods overlay (never shipped).

  remaster_convert.py <in.BIN> <out.BIN> [--max-dim 1024] [--drop 1]

Per resource type: Texture header D3D11->D3D9 (top mips dropped), Material
copied with imports remapped, Renderable layout rebuilt, VertexDescriptor /
TextureState / MaterialState translated (remaster_states), everything else
copied unchanged (identical in both editions for cars present in both)."""
import sys, os, struct, zlib, json, argparse
sys.path.insert(0, os.path.dirname(__file__))
import bnd2, remaster_states as rs

LEARNED = os.environ.get('REMASTER_LEARNED', 'learned-maps.json')

def align(n, a): return (n + a - 1) // a * a

def body_imports(d, h, e):
    c = bnd2.chunk(d, h, e, 0)
    imps = [struct.unpack_from('<QI', c, e['imp_off'] + 16 * i) for i in range(e['imp_cnt'])]
    return (c[:e['imp_off']] if e['imp_cnt'] else c), imps

def zero_slots(body, imps):
    b = bytearray(body)
    for _, off in imps: b[off:off + 4] = b'\0\0\0\0'
    return bytes(b)

# ---------------------------------------------------------------- textures
FOURCC = {0x47: b'DXT1', 0x48: b'DXT1', 0x4D: b'DXT5', 0x4E: b'DXT5'}

def mip_size(w, h, fmt):
    bs = 8 if fmt == b'DXT1' else 16
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs

def texture(body, sec, max_dim, drop, gui=False):
    fmt_dx = struct.unpack_from('<I', body, 0x1C)[0]
    w, h = struct.unpack_from('<HH', body, 0x24)
    depth, arr = struct.unpack_from('<HH', body, 0x28)
    top, mips = body[0x2C], body[0x2D]
    dim = struct.unpack_from('<I', body, 0x08)[0]
    if fmt_dx not in FOURCC or depth != 1 or arr != 1 or top != 0 or dim != 7:
        raise ValueError(f'texture fmt {fmt_dx} depth {depth} array {arr} top {top} dim {dim} unsupported')
    fcc = FOURCC[fmt_dx]
    sizes = []; ww, hh = w, h
    for _ in range(mips):
        sizes.append((ww, hh, mip_size(ww, hh, fcc))); ww, hh = max(1, ww // 2), max(1, hh // 2)
    if sum(s for _, _, s in sizes) > len(sec):
        raise ValueError('texture data shorter than its mip chain')
    # wanted reduction: `drop` levels for big textures, then down to max_dim
    want = drop if max(w, h) >= 512 else 0
    while max(w >> want, h >> want) > max_dim: want += 1
    if want <= mips - 1:
        k = want
        skip = sum(s for _, _, s in sizes[:k])
        nw, nh, _ = sizes[k]; nm = mips - k
        data = sec[skip:skip + sum(s for _, _, s in sizes[k:])]
    else:
        # not enough stored mips (e.g. single-level livery maps): resample
        nw, nh = max(4, w >> want), max(4, h >> want)
        nm = 1 if mips == 1 else max(1, mips - want)
        data = resample(sec[:sizes[0][2]], w, h, fcc, nw, nh, nm)
    # 32 bytes as in the UB. Vehicle textures: bytes 0xE/0xF = 1 and flag 0x8
    # ("applied to a model"); GUI textures (Apt icon atlases) have neither.
    hdr = struct.pack('<IIIHBB4sHHBBBBI', 0, 0, 0, 1, 0 if gui else 1, 0 if gui else 1, fcc, nw, nh, 1, nm, 0, 0 if gui else 8, 0)
    data = data + b'\0' * (-len(data) % 0x80)                                          # UB pads pixel data to 0x80
    return hdr, data

def resample(top, w, h, fcc, nw, nh, nm):
    import numpy as np, texture2ddecoder, etcpak
    dec = texture2ddecoder.decode_bc1 if fcc == b'DXT1' else texture2ddecoder.decode_bc3
    enc = etcpak.compress_bc1 if fcc == b'DXT1' else etcpak.compress_bc3
    img = np.frombuffer(dec(top, w, h), np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]].astype(np.float32)
    out = bytearray(); cw, ch = nw, nh
    for _ in range(nm):
        fy, fx = h // ch, w // cw
        small = img[:ch * fy, :cw * fx].reshape(ch, fy, cw, fx, 4).mean(axis=(1, 3))
        pw, ph = max(4, cw), max(4, ch)
        tile = np.zeros((ph, pw, 4), np.float32); tile[:ch, :cw] = small
        out += enc(np.clip(tile + 0.5, 0, 255).astype(np.uint8).tobytes(), pw, ph)
        cw, ch = max(1, cw // 2), max(1, ch // 2)
    return bytes(out)

# ------------------------------------------------------------- renderables
def renderable(body, imps, sec):
    """Remastered 32-bit Renderable -> UB layout. Returns (body, imports)."""
    bsphere = body[0:16]
    ver, nmesh, pmeshes, _objscope, flags = struct.unpack_from('<HHIIH', body, 0x10)
    pib, pvb = struct.unpack_from('<II', body, 0x20)
    if ver != 11: raise ValueError(f'renderable version {ver}')
    _ib_if, _ib_use, _ib_type, ib_off, ib_size, ib_isz = struct.unpack_from('<6I', body, pib)
    _vb_if, _vb_use, _vb_type, vb_off, vb_size = struct.unpack_from('<5I', body, pvb)
    if ib_isz != 2: raise ValueError(f'index size {ib_isz}')
    nidx = ib_size // 2
    idx = struct.unpack_from(f'<{nidx}H', sec, ib_off)
    mesh_ptrs = struct.unpack_from(f'<{nmesh}I', body, pmeshes)
    imp_at = {off: rid for rid, off in imps}
    # header
    out = bytearray(bsphere + struct.pack('<HHIIHH', ver, nmesh, 0x30, 0, flags, 0) + b'\0' * 8)
    out += b'\0' * 8                                   # 0x28..0x30
    mesh_tab = len(out); out += b'\0' * (4 * nmesh)
    while len(out) % 8: out += b'\0'
    ib_at = len(out); out += struct.pack('<4I', nidx, ib_off, 0, 0x65)    # D3DFMT_INDEX16
    vb_at = len(out); out += struct.pack('<4I', vb_off, 0, vb_size, 0)
    struct.pack_into('<II', out, 0x20, ib_at, vb_at)
    new_imps = []
    for mi, mp in enumerate(mesh_ptrs):
        while len(out) % 16: out += b'\0'
        mo = len(out); struct.pack_into('<I', out, mesh_tab + 4 * mi, mo)
        bbox = body[mp:mp + 0x40]
        topo, base, start, count = struct.unpack_from('<iiII', body, mp + 0x40)
        nvd, inst, nvb, mflags = body[mp + 0x54:mp + 0x58]
        if topo not in (4, 5) or nvb != 1 or inst not in (0, 1):
            raise ValueError(f'mesh topology {topo} vb {nvb} inst {inst}')
        rng = idx[start:start + count]
        lo, hi = (min(rng), max(rng)) if rng else (0, 0)
        prims = count // 3 if topo == 4 else max(0, count - 2)
        out += bbox + struct.pack('<6i', topo, base, start, hi - lo + 1, lo, prims)
        mat_slot = mp + 0x50
        out += struct.pack('<I', 0) + bytes([nvd, inst, nvb, mflags])
        out += struct.pack('<II', ib_at, vb_at)
        if mat_slot in imp_at: new_imps.append((imp_at[mat_slot], mo + 0x58))
        for v in range(nvd):
            slot = mp + 0x60 + 4 * v
            if slot not in imp_at: raise ValueError('vertex descriptor slot without import')
            new_imps.append((imp_at[slot], len(out)))
            out += b'\0' * 4
    out += b'\0' * 0x28                    # UB renderables carry 0x28+ zero bytes here
    while len(out) % 16: out += b'\0'
    return bytes(out), new_imps

# -------------------------------------------------------------- the bundle
def convert(src, dst, max_dim=1024, drop=1, report=None):
    d, h, ents = bnd2.read(src)
    learned = json.load(open(LEARNED)) if os.path.exists(LEARNED) else {}
    byid = {e['id']: e for e in ents}
    res = []          # (id, type, [chunk0, chunk1, chunk2], imports, align exps)
    stats = {}
    for e in ents:
        t = e['type']
        body, imps = body_imports(d, h, e)
        secs = [bnd2.chunk(d, h, e, 1), bnd2.chunk(d, h, e, 2)]
        al = [u >> 28 for u in e.get('unc_raw', [0, 0, 0])] if 'unc_raw' in e else None
        if t == 0x0:
            body, s1 = texture(body, secs[0], max_dim, drop); secs = [s1, secs[1]]
            al = (2, 4, 0)
        elif t == 0xC:
            body, imps = renderable(body, imps, secs[0]); al = (4, 4, 0)
        elif t in (0xA, 0xE, 0xF):
            z = zero_slots(body, imps)
            key = z.hex() + '|' + ','.join(f'{a:x}' for a, _ in imps)
            hit = learned.get(f'{t:x}', {}).get(key)
            if t == 0xA: body = rs.vd(z); al = (2, 0, 0)
            elif t == 0xE: body = rs.ts(z); imps = [(a, 0x3C) for a, _ in imps]; al = (4, 0, 0)
            else:
                body = rs.ms(z); al = (4, 0, 0)
            # Pure translation keeps the remaster's own semantics (the learned
            # UB variants disagree between cars, e.g. colour-write on the paint
            # pass). Only the D3D9 alpha-test reference, which D3D11 assets do
            # not carry, is taken from the most common UB value for this state.
            if hit and t == 0xF:
                refs = {}
                for k2, n2 in hit.items():
                    ub = bytes.fromhex(k2.split('|')[0])
                    if len(ub) == 240: refs[ub[0x40]] = refs.get(ub[0x40], 0) + n2
                if refs and body[0x38] == 1:          # alpha test enabled
                    b = bytearray(body); b[0x40:0x44] = struct.pack('<I', max(refs, key=refs.get)); body = bytes(b)
            stats[t] = stats.get(t, 0) + 1
        elif t == 0x1:
            al = (4, 0, 0)
        else:
            al = None
        res.append([e, body, secs, imps, al])
    specialise_passes(res)
    remap_ids(res)
    write(dst, h, res)
    return stats

_UB_IDS = None
def ub_ids():
    """Every resource id defined by the Ultimate Box's vehicle, wheel and global
    bundles. The engine keeps one resource per id; a converted resource that
    reuses an id with different content (the remaster re-authored materials and
    textures under the same ids) makes bundles share - and later free - each
    other's resources."""
    global _UB_IDS
    if _UB_IDS is None:
        import glob as _g
        ids = set()
        base = os.environ.get('UB_DIR', '')
        for path in (_g.glob(f'{base}/VEHICLES/*.BIN') + _g.glob(f'{base}/WHEELS/*.BNDL') +
                     _g.glob(f'{base}/*.BIN') + _g.glob(f'{base}/*.BNDL') + _g.glob(f'{base}/*.BUNDLE')):
            try:
                _, _, ents = bnd2.read(path)
            except Exception:
                continue
            ids.update(e['id'] for e in ents)
        _UB_IDS = ids
    return _UB_IDS

def new_id(old):
    """Deterministic replacement id (same old id -> same new id in every converted
    bundle, so resources shared between converted bundles stay shared)."""
    taken = ub_ids()
    nid = zlib.crc32(struct.pack('<Q', old) + b'remaster')
    while nid in taken or nid == old:
        nid = zlib.crc32(struct.pack('<I', nid) + b'remaster')
    return nid

def remap_ids(res):
    taken = ub_ids()
    defined = {r[0]['id'] for r in res}
    remap = {i: new_id(i) for i in defined if i in taken}
    for r in res:
        if r[0]['id'] in remap:
            e = dict(r[0]); e['id'] = remap[r[0]['id']]; r[0] = e
        r[3] = [(remap.get(a, a), off) for a, off in r[3]]
    return remap

def specialise_passes(res):
    """UB materials import up to four MaterialStates, one per render pass:
    passes 0/1 write colour, passes 2/3 (depth/shadow) do not (true for every
    material of 200 UB cars). Remastered materials may share one state across
    passes, so give each pass the colour-write value UB expects, cloning the
    state under a derived id where needed."""
    import zlib as _z
    byid = {r[0]['id']: r for r in res}
    taken = set(byid)
    clones = {}
    for r in res:
        e, body, secs, imps, al = r
        if e['type'] != 0x1: continue
        new_imps = []; idx = 0
        for rid, off in imps:
            ms = byid.get(rid)
            if ms is None or ms[0]['type'] != 0xF:
                new_imps.append((rid, off)); continue
            want = 0xF if idx < 2 else 0x0
            idx += 1
            have = struct.unpack_from('<I', ms[1], 0x10)[0]
            if have == want:
                new_imps.append((rid, off)); continue
            key = (rid, want)
            if key not in clones:
                nid = _z.crc32(struct.pack('<QI', rid, want))          # 32-bit like every game id
                while nid in taken: nid += 1
                taken.add(nid)
                b = bytearray(ms[1]); struct.pack_into('<I', b, 0x10, want)
                ne = dict(ms[0]); ne['id'] = nid
                clones[key] = [ne, bytes(b), ms[2], ms[3], ms[4]]
            new_imps.append((clones[key][0]['id'], off))
        r[3] = new_imps
    res.extend(clones.values())

def write(dst, h, res):
    res = sorted(res, key=lambda r: r[0]['id'])
    n = len(res)
    ent_off = 0x30
    blocks = [bytearray(), bytearray(), bytearray()]
    entries = bytearray()
    for e, body, secs, imps, al in res:
        c0 = bytearray(body)
        if imps:
            while len(c0) % 16: c0 += b'\0'
            imp_off = len(c0)
            for rid, off in imps: c0 += struct.pack('<QII', rid, off, 0)
        else:
            imp_off = 0
        chunks = [bytes(c0), secs[0], secs[1]]
        orig_al = [(u >> 28) for u in e['unc_full']]
        a = al if al else orig_al
        unc = []; disk = []; offs = []
        for k in range(3):
            raw = chunks[k]
            if not raw:
                unc.append(0 | (a[k] << 28) if a[k] else 0); disk.append(0); offs.append(0); continue
            z = zlib.compress(raw, 9)
            blk = blocks[k]
            while len(blk) % 16: blk += b'\0'
            offs.append(len(blk)); blk += z
            unc.append(len(raw) | (a[k] << 28)); disk.append(len(z))
        ihash = 0
        for rid, _ in imps: ihash |= rid           # entry import hash = OR of import ids
        entries += struct.pack('<QQ3I3I3IIIHBB', e['id'], ihash, *unc, *disk, *offs,
                               imp_off, e['type'], len(imps), e['flags'], e['stream'])
    hdr_len = ent_off + len(entries)
    b0 = hdr_len
    b1 = align(b0 + len(blocks[0]), 0x80)
    b2 = align(b1 + len(blocks[1]), 0x80)
    end = align(b2 + len(blocks[2]), 0x80)
    out = bytearray(struct.pack('<4sIIIIIIIII', b'bnd2', 2, 1, ent_off, n, ent_off, b0, b1, b2, 7))
    out += b'\0' * (ent_off - len(out)) + entries
    out += blocks[0]; out += b'\0' * (b1 - len(out))
    out += blocks[1]; out += b'\0' * (b2 - len(out))
    out += blocks[2]; out += b'\0' * (end - len(out))
    open(dst, 'wb').write(out)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst')
    ap.add_argument('--max-dim', type=int, default=1024); ap.add_argument('--drop', type=int, default=1)
    a = ap.parse_args()
    print(convert(a.src, a.dst, a.max_dim, a.drop))
