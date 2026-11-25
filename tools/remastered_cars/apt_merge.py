#!/usr/bin/env python3
"""Append car icons from other Apt icon libraries to the junkyard's B5CarsIcon
library (private tool). The junkyard shows a car by gotoAndStop("CAR_<id>") on
the sprite "CarsMovieclipInst" (cars_mc) of the tab's library; a label it lacks
leaves the red "MISSING" frame.

For every CAR_<id> frame of a source library that the target lacks, the frame's
wrapper sprite, its shapes and their geometry (textured quads, texture via a
bundle import on the part's texture slot) are copied to the end of the target
blob; the target's character list, export table, geometry table and cars_mc
frame list are re-allocated (old entries + new) at the end. Existing bytes are
not moved.

What the Ultimate Box runtime does with such additions (host tests 2026-11-16,
memory of the loaded library read through /proc):
- new frames work, also with an existing sprite of the target (aliases);
- new sprites work once they are exported (a 'Car<n>' name like the target's);
- new shape characters are loaded, linked to their geometry and instantiated,
  but are never drawn, whatever their geometry, texture, Image index or place
  in the blob. Renumbering the target's characters (Images/shapes first) and
  an inline geometry list did not change that and leave some of the target's
  own tiles empty, so neither is done. The cause is not known.
Cars that need a picture of their own therefore get a library of their own
(remaster_cars.cop_icon_library) or an alias to the tile of the same model."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import bnd2, apt as aptmod

def al(n, a=16): return (n + a - 1) // a * a

class Writer:
    def __init__(self, blob, base):
        self.b = bytearray(blob); self.base = base
    def tail(self, data, a=16):
        while len(self.b) % a: self.b += b'\0'
        off = len(self.b); self.b += data; return off          # absolute offset
    def rel(self, absoff): return absoff - self.base
    def w32(self, absoff, v): struct.pack_into('<I', self.b, absoff, v)

def cars_mc(a):
    mo = a.movie_off(); n, p = a.lst(mo + 28)
    best = None
    for i in range(n):
        co = a.u32(p + 4 * i)
        if a.character(co) == 5:
            fn, _ = a.lst(co + 16)
            if best is None or fn > best[1]: best = (i, fn, co)
    return best

def frame_items(a, fp, f):
    k, ip = a.lst(fp + 8 * f)
    return [a.u32(ip + 4 * j) for j in range(k)]

def labels(a, co):
    fn, fp = a.lst(co + 16); out = {}
    for f in range(fn):
        for io in frame_items(a, fp, f):
            if a.u32(io) == 2: out[a.cstr(a.u32(io + 4))] = f
    return out

def merge(target_path, sources, out_path, tex_convert, want=None, repack=None, aliases=None):
    dt, ht, et = bnd2.read(target_path)
    te = [x for x in et if x['type'] == 0x1E][0]
    tbody, timps = bnd2.chunk(dt, ht, te, 0)[:te['imp_off']], []
    tc = bnd2.chunk(dt, ht, te, 0)
    timps = [struct.unpack_from('<QI', tc, te['imp_off'] + 16 * i) for i in range(te['imp_cnt'])]
    T = aptmod.Apt(tbody); W = Writer(tbody, T.base)
    mo = T.movie_off(); nchar, pchar = T.lst(mo + 28)
    chars = [T.u32(pchar + 4 * i) for i in range(nchar)]           # rel offsets
    g0 = T.h[4]; gcount, gunk, gptr = struct.unpack_from('<3I', tbody, g0)
    geoms = list(struct.unpack_from(f'<{gcount}I', tbody, gptr))    # abs offsets
    # A textured geometry part names an Image character by its character index
    # (part field [2], also the shape header's bits 16..): in every Ultimate Box
    # library (286 libraries, 2698 textured parts) character[w[2]] is an Image
    # whose value (+16) is that same index (the loader puts the part's texture
    # there). New textured parts therefore get a new Image character appended
    # to the character list. Parts name a texture by number 1..n (n = second word
    # of the geometry table) and get it through a bundle import on that slot.
    state = {'texnum': gunk}
    texnum = {}
    repack_parts = []
    cm_idx, cm_frames, cm_co = cars_mc(T)
    fn, fp = T.lst(cm_co + 16)
    frames = [(T.u32(fp + 8 * f), T.u32(fp + 8 * f + 4)) for f in range(fn)]   # (count, rel ptr)
    have = labels(T, cm_co)
    new_imports = list(timps); new_textures = {}
    added = []
    # Exports (movie +56: count, pointer; entries (name, character)): every
    # library exports each Image under its texture file name and each sprite
    # under a symbol name; only exported sprites are drawn when placed (a new,
    # unexported sprite stays empty even with an Ultimate Box shape inside).
    # New sprites and Images get the source library's names; a name the target
    # already exports gets a suffix.
    ecount, eptr = T.lst(mo + 56)
    exports = [(T.u32(eptr + 8 * k), T.u32(eptr + 8 * k + 4)) for k in range(ecount)]   # (rel name, char)
    taken = {T.cstr(n) for n, _ in exports}
    renamed = {}
    # Sprites are exported as 'Car<n>' like the target's own; the remaster names
    # a car's sprite like its Image without '.tif' ('Art_Cop_X' / 'Art_Cop_X.tif'),
    # and such a sprite stays empty in the Ultimate Box (a copy of an Ultimate
    # Box sprite under a 'Car' name draws).
    import re as _re
    car_no = [1 + max([int(m.group(1)) for m in (_re.fullmatch(r'Car(\d+)', x) for x in taken) if m] or [0])]
    def sprite_name():
        while f'Car{car_no[0]}' in taken: car_no[0] += 1
        name = f'Car{car_no[0]}'; taken.add(name); car_no[0] += 1
        return name
    def export(name, index):
        if name not in renamed:
            final = name; k = 1
            while final in taken: final = f'{name}_R{k}'; k += 1
            taken.add(final); renamed[name] = final
        exports.append((W.rel(W.tail(renamed[name].encode() + b'\0', 4)), index))
    for spath in sources:
        ds, hs, es = bnd2.read(spath)
        se = [x for x in es if x['type'] == 0x1E][0]
        sc = bnd2.chunk(ds, hs, se, 0)
        simps = {o: i for i, o in (struct.unpack_from('<QI', sc, se['imp_off'] + 16 * k) for k in range(se['imp_cnt']))}
        S = aptmod.Apt(sc[:se['imp_off']])
        smo = S.movie_off(); sn, sp = S.lst(smo + 28)
        schars = [S.u32(sp + 4 * i) for i in range(sn)]
        sg0 = S.h[4]; sgc, _, sgp = struct.unpack_from('<3I', S.b, sg0)
        sgeom = {}
        for q in struct.unpack_from(f'<{sgc}I', S.b, sgp):
            sgeom[struct.unpack_from('<I', S.b, q)[0]] = q
        s_idx, _, s_co = cars_mc(S)
        sfn, sfp = S.lst(s_co + 16)
        sec, sep = S.lst(smo + 56)
        snames = {}                                                   # source char -> exported names
        for k in range(sec):
            snames.setdefault(S.u32(sep + 8 * k + 4), []).append(S.cstr(S.u32(sep + 8 * k)))
        renamed.clear()
        cmap = {}                                                     # source char -> target char
        offs = {}
        # Placements carry no offset in the target (Ultimate Box: frames and
        # wrapper sprites place at 0,0, the art spans 0..160 x 0..100); the
        # remaster centres its art and places it at about (78, 51), and such
        # tiles stay empty in the Ultimate Box. The translation of the
        # placements down to a shape is added to its vertices and bounds.
        def take_offset(raw, off):
            a, b, c, d, tx, ty = struct.unpack_from('<6f', raw, 16)
            if (a, b, c, d) != (1.0, 0.0, 0.0, 1.0): raise SystemExit('placement with rotation/scale')
            struct.pack_into('<2f', raw, 32, 0.0, 0.0)
            return (off[0] + tx, off[1] + ty)
        def copy_char(ci, off=(0.0, 0.0)):
            if ci in cmap:
                if offs[ci] != off: raise SystemExit('character placed with two offsets')
                return cmap[ci]
            co = schars[ci]; t = S.character(co)
            new_index = len(chars); cmap[ci] = new_index; offs[ci] = off; chars.append(None)
            # one export entry per new character (the source libraries list each
            # twice); the target's 389 entries plus two per new car exceed 512
            if t == 5 and snames.get(ci):
                exports.append((W.rel(W.tail(sprite_name().encode() + b'\0', 4)), new_index))
            elif snames.get(ci):
                export(snames[ci][0], new_index)
            if t == 1:                                                # shape: 36 bytes, geometry id = own index
                raw = bytearray(S.b[S.base + co:S.base + co + 36])
                gid = struct.unpack_from('<I', raw, 32)[0]
                struct.pack_into('<I', raw, 32, new_index)
                x0, y0, x1, y1 = struct.unpack_from('<4f', raw, 16)
                struct.pack_into('<4f', raw, 16, x0 + off[0], y0 + off[1], x1 + off[0], y1 + off[1])
                sidx = copy_geometry(gid, new_index, off)
                struct.pack_into('<I', raw, 8, (sidx << 16) | (struct.unpack_from('<I', raw, 8)[0] & 0xFFFF))
                chars[new_index] = W.rel(W.tail(bytes(raw)))
            elif t == 5:                                              # sprite: frames of PlaceObjects
                k_frames, kfp = S.lst(co + 16)
                new_frames = []
                for f in range(k_frames):
                    items = []
                    for io in frame_items(S, kfp, f):
                        it = S.u32(io)
                        raw = bytearray(S.b[S.base + io:S.base + io + 64])
                        if it == 3:
                            fl = struct.unpack_from('<I', raw, 4)[0]
                            assert fl & 0xA0 == 0, 'named/clip-action placement'
                            child = take_offset(raw, off) if fl & 4 else off
                            if fl & 2: struct.pack_into('<I', raw, 12, copy_char(struct.unpack_from('<I', raw, 12)[0], child))
                        elif it not in (4,):
                            raise SystemExit(f'sprite item type {it} not handled')
                        items.append(W.rel(W.tail(bytes(raw))))
                    arr = W.tail(b''.join(struct.pack('<I', x) for x in items))
                    new_frames.append((len(items), W.rel(arr)))
                farr = W.tail(b''.join(struct.pack('<II', *x) for x in new_frames))
                raw = bytearray(S.b[S.base + co:S.base + co + 28])
                struct.pack_into('<II', raw, 16, len(new_frames), W.rel(farr))
                chars[new_index] = W.rel(W.tail(bytes(raw)))
            elif t == 7:
                chars[new_index] = W.rel(W.tail(S.b[S.base + co:S.base + co + 20]))
            else:
                raise SystemExit(f'character type {t} not handled')
            return new_index
        def new_image(src):
            m = len(chars)
            chars.append(W.rel(W.tail(struct.pack('<5I', 7, aptmod.SIG, 0, 0, m))))
            if snames.get(src): export(snames[src][0], m)
            return m
        def copy_geometry(gid, new_id, off):
            """copy one geometry; returns the shape index (first textured part's Image, else 0)"""
            q = sgeom[gid]
            _, nparts, pl = struct.unpack_from('<3I', S.b, q)
            parts = []; sidx = 0
            for pt in struct.unpack_from(f'<{nparts}I', S.b, pl):
                w = list(struct.unpack_from('<8I', S.b, pt))
                verts = struct.unpack_from(f'<{w[4]}I', S.b, w[5])
                vabs = []
                for v in verts:
                    vx = bytearray(S.b[v:v + 20]); x, y = struct.unpack_from('<2f', vx, 0)
                    struct.pack_into('<2f', vx, 0, x + off[0], y + off[1])
                    vabs.append(W.tail(bytes(vx), 4))
                vl = W.tail(b''.join(struct.pack('<I', v) for v in vabs))
                w[5] = vl
                tex = simps.get(pt + 12)
                if w[1] == 1:                                     # textured part: its own Image character
                    w[2] = new_image(w[2])
                    if not sidx: sidx = w[2]
                if tex is not None and repack is not None:
                    w[3] = gunk + 1                               # one shared atlas, texture number n+1
                    pabs = W.tail(struct.pack('<8I', *w))
                    repack_parts.append((pabs, vabs, spath, tex))
                    parts.append(pabs); continue
                if tex is not None:
                    tid = tex_convert(spath, tex)
                    if tid not in texnum:
                        state['texnum'] += 1; texnum[tid] = state['texnum']
                    w[3] = texnum[tid]
                pabs = W.tail(struct.pack('<8I', *w))
                if tex is not None:
                    new_imports.append((tid, pabs + 12))
                parts.append(pabs)
            plabs = W.tail(b''.join(struct.pack('<I', x) for x in parts))
            geoms.append(W.tail(struct.pack('<3I', new_id, len(parts), plabs)))
            return sidx
        for f in range(sfn):
            items = frame_items(S, sfp, f)
            lab = [S.cstr(S.u32(io + 4)) for io in items if S.u32(io) == 2]
            if not lab or lab[0] in have or lab[0] == 'MISSING': continue
            if want is not None and lab[0] not in want: continue
            label = lab[0]
            new_items = []
            frame_no = len(frames)
            for io in items:
                it = S.u32(io); raw = bytearray(S.b[S.base + io:S.base + io + 64])
                if it == 1:                                           # stop(); bytecode followed by the label text
                    code = W.tail(b'\x07\x00\x00\x00' + label.encode() + b'\0', 4)
                    struct.pack_into('<I', raw, 4, W.rel(code))
                    raw = raw[:16]
                elif it == 2:
                    sabs = W.tail(label.encode() + b'\0', 4)
                    struct.pack_into('<I', raw, 4, W.rel(sabs)); struct.pack_into('<I', raw, 12, frame_no)
                    raw = raw[:16]
                elif it == 3:
                    fl = struct.unpack_from('<I', raw, 4)[0]
                    assert fl & 0xA0 == 0
                    child = take_offset(raw, (0.0, 0.0)) if fl & 4 else (0.0, 0.0)
                    if fl & 2: struct.pack_into('<I', raw, 12, copy_char(struct.unpack_from('<I', raw, 12)[0], child))
                    # the placement's ratio is its frame number / 65536 in every
                    # library (Ultimate Box and remaster); the source values
                    # would repeat the target's frames 0..n and the seek then
                    # takes the new placement for an existing one (tile stays empty)
                    struct.pack_into('<I', raw, 4, fl | 0x10)
                    struct.pack_into('<f', raw, 48, frame_no / 65536.0)
                elif it == 4:
                    raw = raw[:16]
                else:
                    raise SystemExit(f'cars_mc item type {it}')
                new_items.append(W.rel(W.tail(bytes(raw))))
            if not any(S.u32(io) == 4 for io in items):               # first source frame: clear depth 1 too
                new_items.insert(0, W.rel(W.tail(struct.pack('<4I', 4, 1, 0, 0))))
            arr = W.tail(b''.join(struct.pack('<I', x) for x in new_items))
            frames.append((len(new_items), W.rel(arr)))
            have[label] = frame_no; added.append(label)
    # Aliases: a new label whose car's model already has a tile in the target
    # gets a frame that places that tile's sprite (a new frame with an
    # existing sprite draws; new shapes do not, see the module notes).
    for label, base_label in (aliases or {}).items():
        if label in have or base_label not in have: continue
        frame_no = len(frames)
        cnt, ptr = frames[have[base_label]]
        place = [T.u32(ptr + 4 * j) if ptr < T.h[3] - T.base else struct.unpack_from('<I', W.b, T.base + ptr + 4 * j)[0] for j in range(cnt)]
        place = [io for io in place if struct.unpack_from('<I', W.b, T.base + io)[0] == 3][0]
        raw = bytearray(W.b[T.base + place:T.base + place + 64])
        struct.pack_into('<I', raw, 4, struct.unpack_from('<I', raw, 4)[0] | 0x10)
        struct.pack_into('<f', raw, 48, frame_no / 65536.0)
        code = W.tail(b'\x07\x00\x00\x00' + label.encode() + b'\0', 4)
        name = W.tail(label.encode() + b'\0', 4)
        items = [W.rel(W.tail(struct.pack('<4I', 4, 1, 0, 0))), W.rel(W.tail(struct.pack('<4I', 1, W.rel(code), 0, 0))),
                 W.rel(W.tail(struct.pack('<4I', 2, W.rel(name), 0x70000, frame_no))), W.rel(W.tail(bytes(raw)))]
        arr = W.tail(b''.join(struct.pack('<I', x) for x in items))
        frames.append((len(items), W.rel(arr)))
        have[label] = frame_no; added.append(label)
    if repack is not None and repack_parts:
        atlas_id = repack(W, repack_parts)                        # rewrites the parts' UVs in W.b
        state['texnum'] = gunk + 1
        for pabs, _, _, _ in repack_parts: new_imports.append((atlas_id, pabs + 12))
    # re-allocated lists
    carr = W.tail(b''.join(struct.pack('<I', x) for x in chars))
    W.w32(T.base + mo + 28, len(chars)); W.w32(T.base + mo + 32, W.rel(carr))
    farr = W.tail(b''.join(struct.pack('<II', *x) for x in frames))
    W.w32(T.base + cm_co + 16, len(frames)); W.w32(T.base + cm_co + 20, W.rel(farr))
    earr = W.tail(b''.join(struct.pack('<II', *x) for x in exports))
    W.w32(T.base + mo + 56, len(exports)); W.w32(T.base + mo + 60, W.rel(earr))
    garr = W.tail(b''.join(struct.pack('<I', x) for x in geoms))
    struct.pack_into('<3I', W.b, g0, len(geoms), state['texnum'], garr)
    while len(W.b) % 16: W.b += b'\0'
    struct.pack_into('<I', W.b, 20, len(W.b))                       # header: end of the movie data
    return bytes(W.b), new_imports, added, te, ht, et, dt


def make_repacker(cell_w, cell_h, atlas, read_texture, new_id, store):
    """Pack the icon regions of the copied quads into one atlas texture.
    read_texture(spath, tid) -> (w, h, RGBA bytes); store(id, w, h, rgba)."""
    import numpy as np
    from PIL import Image
    cache = {}
    def run(W, parts):
        cols, rows = atlas // cell_w, atlas // cell_h
        regions = {}
        for pabs, vabs, spath, tid in parts:
            uv = [struct.unpack_from('<2f', W.b, v + 12) for v in vabs]
            key = (spath, tid, min(u for u, _ in uv), min(v for _, v in uv), max(u for u, _ in uv), max(v for _, v in uv))
            regions.setdefault(key, []).append((pabs, vabs))
        if len(regions) > cols * rows:
            raise SystemExit(f'{len(regions)} icons do not fit {cols}x{rows}')
        canvas = Image.new('RGBA', (atlas, atlas), (0, 0, 0, 0))
        for n, (key, users) in enumerate(regions.items()):
            spath, tid, u0, v0, u1, v1 = key
            if (spath, tid) not in cache:
                w, h, rgba = read_texture(spath, tid)
                cache[(spath, tid)] = Image.frombytes('RGBA', (w, h), rgba)
            src = cache[(spath, tid)]
            box = (round(u0 * src.width), round(v0 * src.height), round(u1 * src.width), round(v1 * src.height))
            tile = src.crop(box).resize((cell_w - 2, cell_h - 2), Image.LANCZOS)
            cx, cy = (n % cols) * cell_w + 1, (n // cols) * cell_h + 1
            canvas.paste(tile, (cx, cy))
            nu0, nv0, nu1, nv1 = cx / atlas, cy / atlas, (cx + cell_w - 2) / atlas, (cy + cell_h - 2) / atlas
            for pabs, vabs in users:
                for v in vabs:
                    u, vv = struct.unpack_from('<2f', W.b, v + 12)
                    fu = (u - u0) / (u1 - u0) if u1 > u0 else 0.0
                    fv = (vv - v0) / (v1 - v0) if v1 > v0 else 0.0
                    struct.pack_into('<2f', W.b, v + 12, nu0 + fu * (nu1 - nu0), nv0 + fv * (nv1 - nv0))
        aid = new_id
        store(aid, atlas, atlas, canvas.tobytes())
        return aid
    return run
