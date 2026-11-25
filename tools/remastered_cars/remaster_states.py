#!/usr/bin/env python3
"""Remastered (D3D11 layout) -> Ultimate Box (D3D9 layout) translators for
VertexDescriptor (0xA), TextureState (0xE) and MaterialState (0xF) bodies
(import slots zeroed). Private tool; field meanings from burnout.wiki
(Vertex Descriptor/Texture State/Material State, PC and Remastered pages) and
checked against every resource pair of cars present in both editions."""
import struct

# Remastered semantic-name index -> UB "indexed usage"
SEM = {1: 1, 2: 2, 3: 3, 4: 4, 5: 6, 6: 7, 7: 8, 8: 9, 9: 10, 10: 11, 11: 12, 12: 13,
       13: 14, 14: 15, 15: 21, 16: 22, 17: 5, 18: 25}
# DXGI_FORMAT -> D3DDECLTYPE
FMT = {41: 0, 16: 1, 6: 2, 2: 3, 87: 4, 30: 5, 37: 6, 14: 7, 28: 8, 38: 9, 13: 10,
       35: 11, 11: 12, 34: 15, 10: 16}

def vd(r):
    one, tflags, _iface, w3 = struct.unpack_from('<4I', r, 0)
    n = w3 & 0xFF
    out = bytearray(struct.pack('<4I', 1, 0, 0, n))
    flags = 0
    for i in range(n):
        sem, semidx, slot, cls, fmt, off, step, stride = struct.unpack_from('<4B4I', r, 16 + 20 * i)
        if sem not in SEM or fmt not in FMT:
            raise ValueError(f'vertex element sem {sem} fmt {fmt} unsupported')
        iu = SEM[sem]
        if sem == 4 and semidx: iu = {1: 5, 2: 23}[semidx]
        flags |= 1 << iu
        out += struct.pack('<BBHB3xBBBBI', slot, stride, off, FMT[fmt], 0, 0xFF, 0xFF, iu, 2 if cls else 1)
    struct.pack_into('<I', out, 8, flags)
    return bytes(out)

# D3D11_FILTER_TYPE (0 point, 1 linear, 2 anisotropic) -> D3DTEXTUREFILTERTYPE
FILT = {0: 1, 1: 2, 2: 3}

def ts(r):
    """64-byte TextureState; texture import slot at 0x3C (UB) / 0x38 (R)."""
    au, av, aw, fmag, fmin, fmip, minlod, maxlod, aniso, bias, cmp_, white = struct.unpack_from('<6I2fIfIB', r, 0)
    mag, mn, mip = FILT[fmag], FILT[fmin], FILT[fmip]
    return struct.pack('<6IIIfI5II', au, av, aw, mag, mn, mip, 0, max(aniso, 1), bias,
                       0xFFFFFFFF if white else 0, 1, 1, 0, 0, 1, 0)

def _blend9(rt):
    """D3D11 RenderTargetBlendState bits -> UB packed D3DBLENDSTATE word."""
    src, dst, op = (rt >> 1) & 31, (rt >> 6) & 31, (rt >> 11) & 7
    srca, dsta, opa = (rt >> 14) & 31, (rt >> 19) & 31, (rt >> 24) & 7
    # D3D11 and D3D9 share the numeric values of blend factors/ops used here.
    return (src | op << 5 | dst << 8 | srca << 16 | opa << 21 | dsta << 24), (src, dst, op) != (srca, dsta, opa)

def ms(r):
    """192-byte Remastered MaterialState -> 240-byte UB MaterialState."""
    R = struct.unpack_from('<48I', r, 0)
    rt0 = R[3]
    blend, separate = _blend9(rt0)
    be, wmask = rt0 & 1, (rt0 >> 27) & 15
    a2c = r[0x3C]
    factor = struct.unpack_from('<4f', r, 0x2C)
    W = [0] * 60
    W[0:3] = [0x0C, 0x54, 0x9C]
    # BlendState @0x0C
    W[3] = blend if be else 0x01220122       # disabled blending: UB stores ONE/ZERO
    W[4:8] = [wmask, 0xF, 0xF, 0xF]
    W[8:12] = [struct.unpack('<I', struct.pack('<f', f))[0] for f in factor]
    W[12] = be; W[13] = 1 if (be and separate) else 0
    # D3D9 has fixed-function alpha test where D3D11 shaders clip; the UB
    # assets enable it exactly where the remaster uses alpha-to-coverage.
    W[14] = 1 if a2c else 0
    W[15] = 5 if (a2c or be) else 8          # D3DCMP_GREATER / ALWAYS
    W[16] = 64 if a2c else 0                 # most common UB reference value
    W[17] = 0xF; W[18] = 0; W[19] = 0; W[20] = 1
    # DepthStencilState @0x54 (comparison/stencil-op values are identical)
    zfunc = R[18]; front = R[19:23]; back = R[23:27]
    sref, smask, swmask = R[27], R[28], R[29]
    zen, zw, sen = r[0x48 + 0x30], r[0x48 + 0x31], r[0x48 + 0x32]
    W[21] = zfunc
    W[22:26] = [front[0], front[1], front[2], front[3]]
    W[26:30] = [back[0], back[1], back[2], back[3]]
    W[30] = sref; W[31] = smask; W[32] = swmask
    W[33] = 1 if zen else 0; W[34] = 1 if zw else 0; W[35] = 1 if sen else 0
    W[36] = 0; W[37] = 0; W[38] = 1
    # RasterizerState @0x9C
    fill, cull, fccw, dbias = R[34], R[35], R[36], struct.unpack_from('<i', r, 0x88 + 0xC)[0]
    slope = struct.unpack_from('<f', r, 0x88 + 0x14)[0]
    if cull == 1: cull9 = 1
    elif cull == 3: cull9 = 2 if fccw else 3      # cull back faces
    else: cull9 = 3 if fccw else 2                # cull front faces
    W[39] = fill; W[40] = cull9
    W[41] = struct.unpack('<I', struct.pack('<f', dbias / float(1 << 24)))[0]
    W[42] = struct.unpack('<I', struct.pack('<f', slope))[0]
    W[43] = 0xFFFFFFFF; W[44] = 0; W[45] = 1; W[46] = 0; W[47] = 0
    W[52] = 1
    return struct.pack('<60I', *W)
