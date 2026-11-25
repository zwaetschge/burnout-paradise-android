#!/usr/bin/env python3
"""Build the port-mods overlay that adds the Remastered-only vehicles to the
Ultimate Box (private; reads the user's own two installations, writes only the
overlay directory, originals untouched).

  remaster_cars.py <out-mod-root> [--only ID,ID] [--max-dim 1024]

Writes VEHICLES/VEH_<id>_{AT,CD,GR}.BIN, WHEELS/WHE_*_GR.BNDL for wheels the
Ultimate Box lacks, VEHICLES/VEHICLELIST.BUNDLE and WHEELS/WHEELLIST.BUNDLE
(UB entries unchanged and in order, new entries appended)."""
import sys, os, struct, argparse, zlib
sys.path.insert(0, os.path.dirname(__file__))
import bnd2, remaster_convert as rc

RDIR = os.environ.get('REMASTERED_DIR', '')
UDIR = os.environ.get('UB_DIR', '')
UB_VAULT_VERSION = bytes.fromhex('2f61bf595b7925b4')   # every UB vehicle AT carries this
# The UB executable knows six junkyard categories (bits 0..5; table 0xB2F0F8);
# tabs 3..5 (toy/iconic/boost) only appear for owned DLC packs (0x4EBDA0).
# The added cars are owned from the start (port patch grant-added-vehicles), so:
#  - cop cars (Remastered "Cop Cars") -> Community tab (bit 2), whose label the
#    overlay's language files set to the game's own "Cop Cars" text; plus the
#    Boost bit, whose icon library (B5BoostCarIcons) the overlay replaces with
#    the remaster's cop icons;
#  - Big Surf Island cars -> Paradise Cars + Iconic bit (island icon library);
#  - the remaster's legend/toy redesigns -> Paradise Cars + Toy bit (toy icons).
# Port patch added-vehicles-no-pack makes 0x6FC030 report "no pack" (0x26) for
# these ids, so the junkyard selects them instead of offering a store pack.
CATEGORY = {0x40: 0x24, 0x41: 0x24, 0x80: 0x11, 0x88: 0x09}
# remaster icon library -> Ultimate Box icon bundle it replaces (APT id kept)
ICONS = {'B5COPCARICONS': 'B5BOOSTCARICONS', 'B5ISLANDCARICONS': 'B5ICONICCARICONS',
         'B5TOYCARICONS': 'B5TOYCARICONS'}
# language: the Community tab label shows the game's own "Cop Cars" text
LABELS = {0x327A2D6C: 0xD75B60D9}
# icon library the junkyard loads (PORT_TRACE_FILES: GUIAPT/B5PARADISECARICONS.BUNDLE)
ICON_TARGET = 'B5PARADISECARICONS'

def entries(res_bytes, size):
    n, p = struct.unpack_from('<II', res_bytes, 0)
    return [res_bytes[p + size * i:p + size * (i + 1)] for i in range(n)], p

def rebuild_list(src_u, src_r, rtype, size, add_ids, patch=None):
    du, hu, eu = bnd2.read(src_u); dr, hr, er = bnd2.read(src_r)
    res = []
    for e in eu:
        body = bnd2.chunk(du, hu, e, 0)
        if e['type'] == rtype:
            ue, p = entries(body, size)
            re_, _ = entries(bnd2.chunk(dr, hr, [x for x in er if x['type'] == rtype][0], 0), size)
            have = {x[:8] for x in ue}
            extra = [x for x in re_ if struct.unpack_from('<Q', x, 0)[0] in add_ids and x[:8] not in have]
            if rtype == 0x10005:   # island/legend cars and their secondary finishes stay out for now
                extra = [x for x in extra if struct.unpack_from('<I', x, 0xF8)[0] not in SKIP_CATEGORIES]
            if patch: extra = [patch(x) for x in extra]
            allv = ue + extra
            body = struct.pack('<II', len(allv), p) + body[8:p] + b''.join(allv)
            print(f'  {os.path.basename(src_u)}: {len(ue)} + {len(extra)} entries')
        res.append([e, body, [bnd2.chunk(du, hu, e, 1), bnd2.chunk(du, hu, e, 2)], [], None])
    return hu, res

# Categories left out of the list (none since the converter gives resources
# whose ids the Ultimate Box already uses their own ids, remaster_convert.remap_ids;
# the shared ids made bundles free each other's resources).
SKIP_CATEGORIES = set()

def patch_vehicle(x):
    b = bytearray(x)
    cat = struct.unpack_from('<I', b, 0xF8)[0]
    struct.pack_into('<I', b, 0xF8, CATEGORY.get(cat, cat))
    return bytes(b)

def copy_bundle(src, dst, fix=None):
    d, h, ents = bnd2.read(src)
    res = []
    for e in ents:
        body, imps = rc.body_imports(d, h, e)
        if fix: body = fix(e, body)
        res.append([e, body, [bnd2.chunk(d, h, e, 1), bnd2.chunk(d, h, e, 2)], imps, None])
    rc.write(dst, h, res)

def fix_vault(e, body):
    if e['type'] == 0x1C and body[0x10:0x14] == b'sreV':
        b = bytearray(body); b[0x18:0x20] = UB_VAULT_VERSION; return bytes(b)
    return body

def vehicle_ids(path):
    d, h, ents = bnd2.read(path)
    e = [x for x in ents if x['type'] == 0x10005][0]
    ve, _ = entries(bnd2.chunk(d, h, e, 0), 264)
    return {struct.unpack_from('<Q', x, 0)[0]: x for x in ve}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('--only', default='')
    ap.add_argument('--max-dim', type=int, default=1024)
    ap.add_argument('--lists-only', action='store_true')
    a = ap.parse_args()
    os.makedirs(f'{a.out}/VEHICLES', exist_ok=True); os.makedirs(f'{a.out}/WHEELS', exist_ok=True)
    uv = vehicle_ids(f'{UDIR}/VEHICLES/VEHICLELIST.BUNDLE')
    rv = vehicle_ids(f'{RDIR}/VEHICLES/VEHICLELIST.BUNDLE')
    ufiles = {n.upper() for n in os.listdir(f'{UDIR}/VEHICLES')}
    new = {}
    for vid, x in rv.items():
        if vid in uv: continue
        new[vid] = x
    # file names: VEH_<name>_*.BIN where name is the Remastered file present for it
    rfiles = sorted(n for n in os.listdir(f'{RDIR}/VEHICLES') if n.endswith('_GR.BIN') and n.upper() not in ufiles)
    names = [n[4:-7] for n in rfiles]
    if a.only: names = [n for n in names if n in a.only.split(',')]
    if a.lists_only: names = []
    for name in names:
        for kind in ('GR', 'AT', 'CD'):
            src = f'{RDIR}/VEHICLES/VEH_{name}_{kind}.BIN'; dst = f'{a.out}/VEHICLES/VEH_{name}_{kind}.BIN'
            if kind == 'GR': rc.convert(src, dst, a.max_dim, 1)
            elif kind == 'AT': copy_bundle(src, dst, fix_vault)
            else: copy_bundle(src, dst)
        print('vehicle', name, os.path.getsize(f'{a.out}/VEHICLES/VEH_{name}_GR.BIN'))
    # wheels the UB lacks
    uw = {n.upper() for n in os.listdir(f'{UDIR}/WHEELS')}
    for n in sorted(os.listdir(f'{RDIR}/WHEELS')):
        if n.upper() in uw or not n.upper().endswith('_GR.BNDL') or a.lists_only: continue
        rc.convert(f'{RDIR}/WHEELS/{n}', f'{a.out}/WHEELS/{n}', a.max_dim, 1)
        print('wheel', n)
    hu, res = rebuild_list(f'{UDIR}/VEHICLES/VEHICLELIST.BUNDLE', f'{RDIR}/VEHICLES/VEHICLELIST.BUNDLE', 0x10005, 264, set(new), patch_vehicle)
    rc.write(f'{a.out}/VEHICLES/VEHICLELIST.BUNDLE', hu, res)
    # ids the port hands to the game's own "give vehicle" routine (port patch
    # grant-added-vehicles): race vehicles added by this overlay, nothing else
    with open(f'{a.out}/VEHICLES/PORT_ADDED_VEHICLES.TXT', 'w') as f:
        f.write('# vehicles added by the port-mods overlay (CgsID, hex); owned from the start\n')
        for vid, x in sorted(new.items()):
            # only junkyard cars (race vehicle, primary finish, special category);
            # secondary finishes stay with the game's own unlock logic
            if struct.unpack_from('<I', x, 0xF8)[0] in SKIP_CATEGORIES: continue
            cat = CATEGORY.get(struct.unpack_from('<I', x, 0xF8)[0], struct.unpack_from('<I', x, 0xF8)[0])
            if struct.unpack_from('<I', x, 0x94)[0] & 1 and x[0xFD] in (0, 2) and cat & 0x38:
                f.write(f'{vid:016x}\n')
    dw = {struct.unpack_from('<Q', x, 0)[0] for x in entries(bnd2.chunk(*bnd2.read(f'{RDIR}/WHEELS/WHEELLIST.BUNDLE')[:2], [e for e in bnd2.read(f'{RDIR}/WHEELS/WHEELLIST.BUNDLE')[2] if e['type'] == 0x10009][0], 0), 72)[0]}
    hu, res = rebuild_list(f'{UDIR}/WHEELS/WHEELLIST.BUNDLE', f'{RDIR}/WHEELS/WHEELLIST.BUNDLE', 0x10009, 72, dw)
    rc.write(f'{a.out}/WHEELS/WHEELLIST.BUNDLE', hu, res)
    CS = ' -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_'
    def dec(v):
        t = ''
        while v: t = CS[v % 40] + t; v //= 40
        return t.strip()
    merge_language(a.out, [dec(v) for v in new])
    icon_bundles(a.out, a.max_dim)
    cop_icon_library(a.out)


def icon_bundles(out, max_dim):
    """Junkyard tiles: the carousel shows gotoAndStop("CAR_<id>") on cars_mc of
    the ICON_TARGET library. The remaster's cop/island/toy icon frames are merged
    into a copy of the Ultimate Box's library (tools/local/apt_merge.py);
    their atlas textures are converted and added to that bundle."""
    import apt_merge
    os.makedirs(f'{out}/GUIAPT', exist_ok=True)
    for stale in list(ICONS.values()) + ['B5CARSICON']:            # earlier approaches
        if stale not in (ICON_TARGET, COP_SLOT) and os.path.exists(f'{out}/GUIAPT/{stale}.BUNDLE'): os.remove(f'{out}/GUIAPT/{stale}.BUNDLE')
    added_tex = {}
    def tex_convert(spath, tid):
        key = (spath, tid)
        if key in added_tex: return added_tex[key][0]['id']
        d, h, ents = bnd2.read(spath)
        e = [x for x in ents if x['id'] == tid][0]
        body, sec = rc.texture(bnd2.chunk(d, h, e, 0), bnd2.chunk(d, h, e, 1), max_dim, 0, gui=True)
        ne = dict(e); ne['id'] = rc.new_id(tid) if tid in rc.ub_ids() else tid
        added_tex[key] = (ne, body, sec)
        return ne['id']
    srcs = [f'{RDIR}/GUIAPT/{n}.BUNDLE' for n in ICONS]
    # only the added cars' tiles, repacked into one atlas: the GUI memory of the
    # Ultimate Box does not take the remaster's 11 full atlases (junkyard UI
    # stays away at 2.8 MB of extra textures, works at 0.7 MB)
    CS = ' -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_'
    def dec(v):
        t = ''
        while v: t = CS[v % 40] + t; v //= 40
        return t.strip()
    ids = [int(l, 16) for l in open(f'{out}/VEHICLES/PORT_ADDED_VEHICLES.TXT') if l.strip() and not l.startswith('#')]
    # cars whose model already has a tile here show that tile (apt_merge aliases)
    aliases = {'CAR_PDDK01XS': 'CAR_PDDK01', 'CAR_PBTSVK02': 'CAR_PBTSVK01'}
    want = {f'CAR_{dec(v)}' for v in ids if not dec(v).startswith('PCC')} - set(aliases)   # cop cars: own library (cop_icon_library)
    import texture2ddecoder, etcpak, numpy as np
    def read_texture(spath, tid):
        d, h, ents = bnd2.read(spath)
        e = [x for x in ents if x['id'] == tid][0]
        hd = bnd2.chunk(d, h, e, 0); px = bnd2.chunk(d, h, e, 1)
        fmt = struct.unpack_from('<I', hd, 0x1C)[0]; w, hh = struct.unpack_from('<HH', hd, 0x24)
        fn = texture2ddecoder.decode_bc3 if fmt in (0x4D, 0x4E) else texture2ddecoder.decode_bc1
        bs = 16 if fmt in (0x4D, 0x4E) else 8
        bgra = fn(px[:max(1, w // 4) * max(1, hh // 4) * bs], w, hh)
        return w, hh, np.frombuffer(bgra, np.uint8).reshape(hh, w, 4)[..., [2, 1, 0, 3]].tobytes()
    atlas_store = {}
    def store(aid, w, hh, rgba):
        data = etcpak.compress_bc3(rgba, w, hh)
        hdr = struct.pack('<IIIHBB4sHHBBBBI', 0, 0, 0, 1, 0, 0, b'DXT5', w, hh, 1, 1, 0, 0, 0)
        atlas_store[aid] = (hdr, data)
    # texture numbers 1..n of the geometry parts (w[3]) may follow the bundle's
    # (id-sorted) texture order rather than the import list: keep the atlas last
    _, _, tents = bnd2.read(f'{UDIR}/GUIAPT/{ICON_TARGET}.BUNDLE')
    last = max(x['id'] for x in tents if x['type'] == 0)
    atlas_id = rc.new_id(0xA7105C0F)
    while atlas_id <= last or atlas_id in rc.ub_ids():
        atlas_id = zlib.crc32(struct.pack('<I', atlas_id) + b'remaster')
    repack = apt_merge.make_repacker(170, 113, 1024, read_texture, atlas_id, store)
    blob, imports, added, te, ht, et, dt = apt_merge.merge(f'{UDIR}/GUIAPT/{ICON_TARGET}.BUNDLE', srcs, None, None, want, repack, aliases)
    res = []
    for e in et:
        if e['type'] == 0x1E:
            res.append([e, blob, [b'', b''], imports, None])
        else:
            body, imps = rc.body_imports(dt, ht, e)
            res.append([e, body, [bnd2.chunk(dt, ht, e, 1), bnd2.chunk(dt, ht, e, 2)], imps, None])
    tmpl = [x for x in et if x['type'] == 0][0]
    for aid, (hdr, data) in atlas_store.items():
        ne = dict(tmpl); ne['id'] = aid
        res.append([ne, hdr, [data, b''], [], (2, 4, 0)])
    rc.write(f'{out}/GUIAPT/{ICON_TARGET}.BUNDLE', ht, res)
    print(f'  GUIAPT/{ICON_TARGET}.BUNDLE: +{len(added)} car frames, +{len(atlas_store)} atlas')

COP_SLOT = 'B5BOOSTCARICONS'        # icon library slot 141 (tools/port_patches.py grant-added-vehicles)

def cop_icon_library(out):
    """The remaster's cop icon library, complete and unchanged in structure, in
    library slot 141 (the Community tab - Cop Cars - is pointed there by the
    port patch). Cars added to an Ultimate Box library do not draw (new shape
    characters stay empty, see apt_merge), a library of its own does. Only
    the textures change: every icon is copied into one 1024 atlas (the GUI
    memory does not take the seven remaster atlases) and the parts' UVs and
    texture imports follow; the movie's second name and the resource id
    become those of the slot."""
    import apt_merge, apt as aptmod, texture2ddecoder, etcpak, numpy as np
    src = f'{RDIR}/GUIAPT/B5COPCARICONS.BUNDLE'
    d, h, e = bnd2.read(src)
    te = [x for x in e if x['type'] == 0x1E][0]
    c0 = bnd2.chunk(d, h, te, 0)
    blob = bytearray(c0[:te['imp_off']])
    imps = [list(struct.unpack_from('<QI', c0, te['imp_off'] + 16 * i)) for i in range(te['imp_cnt'])]
    du, hu, eu = bnd2.read(f'{UDIR}/GUIAPT/{COP_SLOT}.BUNDLE')
    ute = [x for x in eu if x['type'] == 0x1E][0]
    S = aptmod.Apt(bytes(blob))
    if S.base != 0x40 or blob[0x30:0x40].rstrip(b'\0') != b'B5CopCarIcons':
        raise SystemExit('unexpected cop library header')
    blob[0x30:0x40] = b'B5BoostCarIcons\0'
    g0 = S.h[4]; gc, gu, gp = struct.unpack_from('<3I', blob, g0)
    texids = {x['id'] for x in e if x['type'] == 0}
    byoff = {o: i for i, (tid, o) in enumerate(imps)}
    parts = []
    for q in struct.unpack_from(f'<{gc}I', blob, gp):
        _, nparts, pl = struct.unpack_from('<3I', blob, q)
        for pt in struct.unpack_from(f'<{nparts}I', blob, pl):
            w = struct.unpack_from('<8I', blob, pt)
            k = byoff.get(pt + 12)
            if k is None: continue
            if imps[k][0] not in texids: raise SystemExit('import is not a texture of the bundle')
            vabs = list(struct.unpack_from(f'<{w[4]}I', blob, w[5]))
            parts.append((pt, vabs, src, imps[k][0], k))
    def read_texture(spath, tid):
        x = [y for y in e if y['id'] == tid][0]
        hd = bnd2.chunk(d, h, x, 0); px = bnd2.chunk(d, h, x, 1)
        fmt = struct.unpack_from('<I', hd, 0x1C)[0]; w, hh = struct.unpack_from('<HH', hd, 0x24)
        fn = texture2ddecoder.decode_bc3 if fmt in (0x4D, 0x4E) else texture2ddecoder.decode_bc1
        bs = 16 if fmt in (0x4D, 0x4E) else 8
        bgra = fn(px[:max(1, w // 4) * max(1, hh // 4) * bs], w, hh)
        return w, hh, np.frombuffer(bgra, np.uint8).reshape(hh, w, 4)[..., [2, 1, 0, 3]].tobytes()
    store = {}
    def keep(aid, w, hh, rgba):
        store[aid] = (struct.pack('<IIIHBB4sHHBBBBI', 0, 0, 0, 1, 0, 0, b'DXT5', w, hh, 1, 1, 0, 0, 0),
                      etcpak.compress_bc3(rgba, w, hh))
    atlas_id = rc.new_id(te['id'] ^ 0x5A5A5A5A)
    class Buf: pass
    W = Buf(); W.b = blob
    repack = apt_merge.make_repacker(170, 113, 1024, read_texture, atlas_id, keep)
    repack(W, [(pt, vabs, sp, tid) for pt, vabs, sp, tid, _ in parts])
    for pt, _, _, _, k in parts:
        struct.pack_into('<I', blob, pt + 12, 1)
        imps[k][0] = atlas_id
    struct.pack_into('<I', blob, g0 + 4, 1)
    ne = dict(te); ne['id'] = ute['id']
    tmpl = [x for x in eu if x['type'] == 0][0]
    res = [[ne, bytes(blob), [b'', b''], [tuple(i) for i in imps], None]]
    for aid, (hdr, data) in store.items():
        ta = dict(tmpl); ta['id'] = aid
        res.append([ta, hdr, [data, b''], [], (2, 4, 0)])
    os.makedirs(f'{out}/GUIAPT', exist_ok=True)
    rc.write(f'{out}/GUIAPT/{COP_SLOT}.BUNDLE', hu, res)
    print(f'  GUIAPT/{COP_SLOT}.BUNDLE: remaster cop icon library, {len(parts)} textured parts in one atlas')

def lang_key(key):
    """Language key hash: CRC-32 without the final inversion (checked:
    CAR_CATEGORY_COMMUNITY_CAR -> 0x327A2D6C)."""
    return zlib.crc32(key.encode()) ^ 0xFFFFFFFF

def blurb_bases(car_ids):
    """Base model of each added car for its CAR_BLURB text: the remaster has no
    blurb for its 33 cop cars (the Ultimate Box's junkyard ticker then shows the
    raw key). A cop car 'PCC<model>' gets the blurb of the Ultimate Box's car
    with the same model suffix (regular cars before the toy 'PSD' ones, then a
    substring match); None means no base (empty text)."""
    import re
    CS = ' -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_'
    def dec(v):
        t = ''
        while v: t = CS[v % 40] + t; v //= 40
        return t.strip()
    uids = sorted(dec(v) for v in vehicle_ids(f'{UDIR}/VEHICLES/VEHICLELIST.BUNDLE'))
    def norm(x): return re.sub(r'0(\d)$', r'\1', x)
    out = {}
    for i in car_ids:
        if not i.startswith('PCC'): continue
        suf = i[3:]
        regular = [u for u in uids if u[:1] == 'P' and not u.startswith(('PCC', 'PSD'))]
        cand = [u for u in regular if norm(u[3:]) == norm(suf)] or [u for u in regular if suf in u[3:]]
        out[i] = cand
    return out

def merge_language(out, car_ids=()):
    """Language tables (type 0x27: id, count, 12, then count x (key hash, string
    offset), then the strings). The Ultimate Box lacks the remaster's strings for
    the added cars (CAR_CAPS_<id>, CAR_BLURB_<id>, ...). Its tables are a fixed
    0x8C000 bytes: the last entry is a filler string that pads the buffer, and
    the game breaks (raw keys everywhere) when the resource grows. Entries whose
    key hash the UB table lacks are inserted before the filler, which shrinks by
    the same amount; size and all existing strings stay as they are. Blurbs the
    remaster lacks (cop cars) become entries pointing at the base model's
    existing string (or at an empty string); they add no text."""
    os.makedirs(f'{out}/LANGUAGE', exist_ok=True)
    bases = blurb_bases(car_ids)
    for name in sorted(os.listdir(f'{UDIR}/LANGUAGE')):
        if not name.endswith('.BUNDLE') or not os.path.exists(f'{RDIR}/LANGUAGE/{name}'): continue
        du, hu, eu = bnd2.read(f'{UDIR}/LANGUAGE/{name}'); dr, hr, er = bnd2.read(f'{RDIR}/LANGUAGE/{name}')
        cu = bnd2.chunk(du, hu, eu[0], 0); cr = bnd2.chunk(dr, hr, er[0], 0)
        lid, n, off = struct.unpack_from('<3I', cu, 0); rlid, rn, roff = struct.unpack_from('<3I', cr, 0)
        if lid != rlid or off != 12: continue
        ue = [struct.unpack_from('<II', cu, off + 8 * i) for i in range(n)]
        re_ = [struct.unpack_from('<II', cr, roff + 8 * i) for i in range(rn)]
        have = {h for h, _ in ue}; uoff = dict(ue)
        extra_all = [(h, cr[o:cr.index(b'\0', o) + 1]) for h, o in re_ if h not in have]
        given = have | {h for h, _ in extra_all}
        text = {h: cu[o:cu.index(b'\0', o) + 1] for h, o in ue}
        relabel = {k: text[v].rstrip(b' \0') + b'\0' for k, v in LABELS.items() if k in text and v in text}
        first = min(o for _, o in ue); empty = cu.index(b'\0', first)          # offset of an empty string
        aliases = []                                              # (key hash, UB string offset)
        for car, cand in bases.items():
            k = lang_key(f'CAR_BLURB_{car}')
            if k in given: continue
            src = next((uoff[lang_key(f'CAR_BLURB_{b}')] for b in cand if lang_key(f'CAR_BLURB_{b}') in uoff), empty)
            aliases.append((k, src))
        filler = max(range(n), key=lambda i: ue[i][1])            # the padding entry (last string)
        fo = ue[filler][1]
        strings = cu[off + 8 * n:fo]                              # every real string, in place
        total = len(cu)
        def build(extra):
            shift = 8 * (len(extra) + len(aliases))
            table = [(h, o + shift) for h, o in ue]
            pos = off + 8 * (n + len(extra) + len(aliases)) + len(strings)
            blob = bytearray(strings); add = []
            for h, t in extra:
                add.append((h, pos)); blob += t; pos += len(t)
            for k, t in relabel.items():                          # existing key, new text at the end
                i = next(j for j, (h, _) in enumerate(table) if h == k)
                table[i] = (k, pos); blob += t; pos += len(t)
            add += [(h, o + shift) for h, o in aliases]
            return table, add, blob, pos
        extra = extra_all
        table, add, blob, pos = build(extra)
        if total - pos - 1 < 64 and car_ids:
            # too little filler for every remaster string: only the added cars' texts
            want = {lang_key(f'CAR_{k}_{i}') for i in car_ids for k in ('CAPS', 'BLURB')}
            extra = [(h, t) for h, t in extra_all if h in want]
            table, add, blob, pos = build(extra)
        pad = total - pos - 1                                     # filler keeps its terminator
        if pad < 64:
            raise SystemExit(f'{name}: no room for {len(extra)} strings')
        table[filler] = (ue[filler][0], pos)
        blob += cu[fo:fo + 1] * pad + b'\0'
        body = struct.pack('<3I', lid, n + len(add), 12) + b''.join(struct.pack('<II', h, o) for h, o in table + add) + bytes(blob)
        assert len(body) == total, (len(body), total)
        rc.write(f'{out}/LANGUAGE/{name}', hu, [[eu[0], body, [b'', b''], [], None]])
        print(f'  LANGUAGE/{name}: {n} + {len(extra)} strings + {len(aliases)} blurb aliases, filler -> {pad + 1} bytes')

if __name__ == '__main__':
    main()
