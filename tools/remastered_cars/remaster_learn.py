#!/usr/bin/env python3
"""Learn Remastered->Ultimate Box resource correspondences from cars present in
both editions (private tool; reads the user's own game files, writes only to
REMASTER_LEARNED). Pairs VertexDescriptor/TextureState/MaterialState
by import position of common Renderables/Materials."""
import sys, os, struct, json, glob
from collections import defaultdict
sys.path.insert(0, os.path.dirname(__file__))
import bnd2

RDIR = os.environ.get('REMASTERED_DIR', '')
UDIR = os.environ.get('UB_DIR', '')

def body_and_imports(d, h, e):
    c = bnd2.chunk(d, h, e, 0)
    imps = [struct.unpack_from('<QI', c, e['imp_off'] + 16 * i) for i in range(e['imp_cnt'])]
    return c[:e['imp_off']] if e['imp_cnt'] else c, imps

def strip_imports(body, imps):
    """Body with import slots zeroed (they hold runtime pointers/garbage)."""
    b = bytearray(body)
    for _, off in imps:
        if off + 4 <= len(b): b[off:off + 4] = b'\0\0\0\0'
    return bytes(b)

def pairs():
    out = []
    for sub in ('VEHICLES', 'WHEELS'):
        for rp in sorted(glob.glob(f'{RDIR}/{sub}/*')):
            name = os.path.basename(rp)
            if not (name.endswith('_GR.BIN') or name.endswith('_GR.BNDL')): continue
            up = f'{UDIR}/{sub}/{name}'
            if os.path.exists(up): out.append((rp, up))
    return out

def main():
    maps = {0xA: {}, 0xE: {}, 0xF: {}}       # R key(hex) -> set of U key(hex)
    conflicts = defaultdict(set)
    n = 0
    for rp, up in pairs():
        dr, hr, er = bnd2.read(rp); du, hu, eu = bnd2.read(up)
        mr = {e['id']: e for e in er}; mu = {e['id']: e for e in eu}
        for rid, e in mr.items():
            if e['type'] not in (0x1, 0xC) or rid not in mu: continue
            f = mu[rid]
            br, ir = body_and_imports(dr, hr, e); bu, iu = body_and_imports(du, hu, f)
            if len(ir) != len(iu): print('import count differs', rp, hex(rid)); continue
            for (ra, _), (ua, _) in zip(ir, iu):
                if ra == ua: continue
                er_ = mr.get(ra); eu_ = mu.get(ua)
                if not er_ or not eu_: continue
                t = er_['type']
                if t not in maps or eu_['type'] != t: continue
                rb, rimp = body_and_imports(dr, hr, er_); ub, uimp = body_and_imports(du, hu, eu_)
                rk = strip_imports(rb, rimp).hex() + '|' + ','.join(f'{a:x}' for a, _ in rimp)
                uk = strip_imports(ub, uimp).hex() + '|' + ','.join(f'{a:x}' for a, _ in uimp)
                maps[t].setdefault(rk, {})
                maps[t][rk][uk] = maps[t][rk].get(uk, 0) + 1
        n += 1
    print('bundles', n)
    for t, m in maps.items():
        multi = sum(1 for v in m.values() if len(v) > 1)
        print(f'type 0x{t:X}: {len(m)} R keys, {multi} with >1 U value')
    json.dump({f'{t:x}': {k: v for k, v in m.items()} for t, m in maps.items()},
              open(os.environ.get('REMASTER_LEARNED', 'learned-maps.json'), 'w'))

if __name__ == '__main__':
    main()
