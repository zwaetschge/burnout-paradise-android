#!/usr/bin/env python3
"""Generate runtime/d3d9_thunk_gen.inc: one thunk per method of every
Direct3D 9 interface, from the DXVK-native d3d9.h (the exact host signatures).

Guest side: 32-bit stdcall COM (every argument is one 4-byte stack slot, `this`
first).  Host side: the 64-bit DXVK object.  A thunk converts the arguments by
type:

  scalars / enums / D3DCOLOR / float   value (float: the 32 bits as they are)
  T* / const T*  (pointer-free POD)    the same bytes in guest memory, in place
  IDirect3D*9*                         guest object -> host object
  IDirect3D*9**                        host object -> guest object (wrapped)
  HANDLE* pSharedHandle                must be NULL (named stop otherwise)

Everything that needs more than that (structures holding handles or pointers,
Lock/Unlock, QueryInterface, Release, CreateDevice, Present ...) is listed in
SPECIAL and implemented by hand in runtime/d3d9_thunk.cpp; a parameter type the
generator does not know aborts generation instead of guessing.

Usage: tools/gen_d3d9_thunks.py [--check]
"""
from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D3D9_H = os.path.join(ROOT, "port", "build", "dxvk-android", "src", "include",
                      "native", "directx", "d3d9.h")
OUT = os.path.join(ROOT, "runtime", "d3d9_thunk_gen.inc")

# Interfaces the guest can see (Direct3DCreate9, not ...Ex).
INTERFACES = [
    "IDirect3D9", "IDirect3DDevice9", "IDirect3DSwapChain9", "IDirect3DSurface9",
    "IDirect3DVolume9", "IDirect3DTexture9", "IDirect3DCubeTexture9",
    "IDirect3DVolumeTexture9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9",
    "IDirect3DVertexDeclaration9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
    "IDirect3DStateBlock9", "IDirect3DQuery9",
]

# Hand-written thunks (runtime/d3d9_thunk.cpp). "*" = every interface.
SPECIAL = {
    ("*", "QueryInterface"): "S_QueryInterface",
    ("*", "Release"): "S_Release",
    ("IDirect3D9", "RegisterSoftwareDevice"): None,          # named stop
    ("IDirect3D9", "GetAdapterIdentifier"): "S_GetAdapterIdentifier",
    ("IDirect3D9", "GetAdapterMonitor"): "S_GetAdapterMonitor",
    ("IDirect3D9", "CreateDevice"): "S_CreateDevice",
    ("IDirect3DDevice9", "GetCreationParameters"): "S_GetCreationParameters",
    ("IDirect3DDevice9", "CreateAdditionalSwapChain"): "S_CreateAdditionalSwapChain",
    ("IDirect3DDevice9", "Reset"): "S_Reset",
    ("IDirect3DDevice9", "Present"): "S_DevicePresent",
    ("IDirect3DDevice9", "GetNPatchMode"): None,              # float in st(0)
    ("IDirect3DSwapChain9", "Present"): "S_SwapChainPresent",
    ("IDirect3DSwapChain9", "GetPresentParameters"): "S_GetPresentParameters",
    ("IDirect3DSurface9", "LockRect"): "S_SurfaceLockRect",
    ("IDirect3DSurface9", "UnlockRect"): "S_SurfaceUnlockRect",
    ("IDirect3DSurface9", "GetDC"): None,
    ("IDirect3DSurface9", "ReleaseDC"): None,
    ("IDirect3DSurface9", "GetContainer"): "S_GetContainer",
    ("IDirect3DVolume9", "LockBox"): "S_VolumeLockBox",
    ("IDirect3DVolume9", "UnlockBox"): "S_VolumeUnlockBox",
    ("IDirect3DVolume9", "GetContainer"): "S_GetContainer",
    ("IDirect3DTexture9", "LockRect"): "S_TextureLockRect",
    ("IDirect3DTexture9", "UnlockRect"): "S_TextureUnlockRect",
    ("IDirect3DCubeTexture9", "LockRect"): "S_CubeLockRect",
    ("IDirect3DCubeTexture9", "UnlockRect"): "S_CubeUnlockRect",
    ("IDirect3DVolumeTexture9", "LockBox"): "S_VolumeTextureLockBox",
    ("IDirect3DVolumeTexture9", "UnlockBox"): "S_VolumeTextureUnlockBox",
    ("IDirect3DVertexBuffer9", "Lock"): "S_VertexBufferLock",
    ("IDirect3DVertexBuffer9", "Unlock"): "S_VertexBufferUnlock",
    ("IDirect3DIndexBuffer9", "Lock"): "S_IndexBufferLock",
    ("IDirect3DIndexBuffer9", "Unlock"): "S_IndexBufferUnlock",
}

# Scalar parameter / return types: passed as the 32-bit value.
SCALARS = {
    "UINT", "DWORD", "INT", "int", "WINBOOL", "BOOL", "ULONG", "LONG", "HRESULT",
    "D3DCOLOR", "D3DFORMAT", "D3DDEVTYPE", "D3DRESOURCETYPE", "D3DMULTISAMPLE_TYPE",
    "D3DBACKBUFFER_TYPE", "D3DPOOL", "D3DTEXTUREFILTERTYPE", "D3DTRANSFORMSTATETYPE",
    "D3DRENDERSTATETYPE", "D3DSTATEBLOCKTYPE", "D3DTEXTURESTAGESTATETYPE",
    "D3DSAMPLERSTATETYPE", "D3DPRIMITIVETYPE", "D3DQUERYTYPE", "D3DCUBEMAP_FACES",
}
# Pointer-free structures with the same layout for the 32-bit guest and the
# 64-bit host; value = size in bytes (checked with static_assert in the output).
POD = {
    "RECT": 16, "POINT": 8, "D3DRECT": 16, "D3DMATRIX": 64, "D3DVIEWPORT9": 24,
    "D3DMATERIAL9": 68, "D3DLIGHT9": 104, "D3DCLIPSTATUS9": 8, "D3DCAPS9": 304,
    "D3DDISPLAYMODE": 16, "D3DRASTER_STATUS": 8, "D3DGAMMARAMP": 1536,
    "D3DVERTEXELEMENT9": 8, "PALETTEENTRY": 4, "D3DSURFACE_DESC": 32,
    "D3DVOLUME_DESC": 28, "D3DVERTEXBUFFER_DESC": 24, "D3DINDEXBUFFER_DESC": 20,
    "D3DBOX": 24, "D3DRECTPATCH_INFO": 28, "D3DTRIPATCH_INFO": 16,
}
# Plain memory: no layout to check.
RAW = {"void", "DWORD", "UINT", "float", "int", "WINBOOL", "BOOL", "RGNDATA", "INT"}


def parse_header(path: str):
    src = open(path).read()
    interfaces = {}
    for match in re.finditer(r"DECLARE_INTERFACE_\((\w+),\s*(\w+)\)\s*\{(.*?)\n\};", src, re.S):
        name, body = match.group(1), match.group(3)
        methods = []
        for m in re.finditer(r"STDMETHOD(_?)\(([^)]*)\)\s*\((.*?)\)\s*PURE;", body, re.S):
            if m.group(1):
                ret, mname = [t.strip() for t in m.group(2).split(",")]
            else:
                ret, mname = "HRESULT", m.group(2).strip()
            params = " ".join(m.group(3).split())
            if params == "THIS":
                plist = []
            else:
                assert params.startswith("THIS_"), (name, mname, params)
                plist = [p.strip() for p in params[5:].split(",")]
            methods.append((ret, mname, plist))
        interfaces[name] = methods
    iids = {}
    for m in re.finditer(r"__CRT_UUID_DECL\((\w+),\s*([^)]*)\)", src):
        iids[m.group(1)] = [int(x, 16) for x in m.group(2).split(",")]
    return interfaces, iids


def split_param(decl: str):
    """'const RECT *src_rect' -> (const, 'RECT', stars, name)."""
    text = decl.replace("struct ", "")
    stars = text.count("*")
    text = text.replace("*", " ")
    tokens = text.split()
    const = "const" in tokens
    tokens = [t for t in tokens if t != "const"]
    base = tokens[0]
    return const, base, stars


def emit_method(iface: str, slot: int, ret: str, name: str, params, out, stats):
    fn = f"T_{iface}_{name}"
    special = SPECIAL.get((iface, name), SPECIAL.get(("*", name), "-"))
    argc = 1 + len(params)
    if special != "-":
        stats["special" if special else "stop"] += 1
        return (special or "nullptr", argc)
    pre, args, post = [], [], []
    for index, decl in enumerate(params, start=1):
        const, base, stars = split_param(decl)
        a = f"a[{index}]"
        ctype = ("const " if const else "") + base
        if stars == 0:
            if base == "float":
                args.append(f"f32({a})")
            elif base == "HWND":
                args.append(f"hwnd_of({a})")
            elif base in ("REFIID", "REFGUID"):
                pre.append(f"    if (!{a}) return stop(\"null-guid\");")
                args.append(f"*(const GUID *)P({a})")
            elif base in SCALARS:
                args.append(f"({base}){a}")
            else:
                raise SystemExit(f"unknown scalar type {base!r} in {iface}::{name}")
        elif stars == 1:
            if base.startswith("IDirect3D"):
                pre.append(f"    {base} *h{index} = nullptr;")
                pre.append(f"    if ({a} && !(h{index} = ({base} *)host_of({a}))) return stop_object({a});")
                args.append(f"h{index}")
            elif base == "HANDLE":
                pre.append(f"    if ({a}) return stop(\"shared-handle\");")
                args.append("nullptr")
            elif base in POD or base in RAW:
                args.append(f"({ctype} *)P({a})")
            else:
                raise SystemExit(f"unknown pointer type {base!r} in {iface}::{name}")
        elif stars == 2:
            if base.startswith("IDirect3D"):
                pre.append(f"    {base} *o{index} = nullptr;")
                args.append(f"{a} ? &o{index} : nullptr")
                post.append(f"    if ({a}) W32({a}, wrap_typed(o{index}, IF_{base}));")
            else:
                raise SystemExit(f"unknown pointer-to-pointer {base!r} in {iface}::{name}")
        else:
            raise SystemExit(f"unsupported indirection in {iface}::{name}: {decl}")
    out.append(f"static int {fn}(const uint32_t *a, uint32_t *r) {{")
    out.append(f"    {iface} *self = ({iface} *)host_of(a[0]);")
    out.append("    if (!self) return stop(\"unknown-this\");")
    out.extend(pre)
    call = f"self->{name}({', '.join(args)})"
    if ret == "void":
        out.append(f"    {call}; (void)r;")
    elif ret in SCALARS or ret in ("HRESULT", "ULONG"):
        out.append(f"    *r = (uint32_t){call};")
    else:
        raise SystemExit(f"unknown return type {ret!r} in {iface}::{name}")
    out.extend(post)
    out.append("    return 0;")
    out.append("}")
    stats["generated"] += 1
    return (fn, argc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    interfaces, iids = parse_header(D3D9_H)
    out = ["/* Generated by tools/gen_d3d9_thunks.py from the DXVK-native d3d9.h. Do not edit. */", ""]
    for base, size in sorted(POD.items()):
        out.append(f"static_assert(sizeof({base}) == {size}, \"{base}: host layout differs from the 32-bit guest layout\");")
    out.append("")
    out.append("enum {")
    for index, iface in enumerate(INTERFACES):
        out.append(f"    IF_{iface} = {index},")
    # Declared-only bases an out parameter can name; resolved at run time.
    out.append(f"    IF_IDirect3DBaseTexture9 = {len(INTERFACES)},")
    out.append(f"    IF_IDirect3DResource9 = {len(INTERFACES) + 1},")
    out.append(f"    IF_COUNT = {len(INTERFACES)}")
    out.append("};")
    out.append("")
    stats = {"generated": 0, "special": 0, "stop": 0}
    tables = []
    for iface in INTERFACES:
        if iface not in interfaces:
            raise SystemExit(f"{iface} not found in {D3D9_H}")
        rows = []
        for slot, (ret, name, params) in enumerate(interfaces[iface]):
            fn, argc = emit_method(iface, slot, ret, name, params, out, stats)
            rows.append((name, fn, argc))
        tables.append((iface, rows))
        out.append("")
    max_slots = max(len(rows) for _, rows in tables)
    for iface, rows in tables:
        out.append(f"static const Method M_{iface}[] = {{")
        for name, fn, argc in rows:
            out.append(f"    {{\"{name}\", {fn}, {argc}}},")
        out.append("};")
    out.append("")
    out.append("static const Interface g_interfaces[IF_COUNT] = {")
    for iface, rows in tables:
        iid = iids.get(iface)
        if not iid:
            raise SystemExit(f"no IID for {iface}")
        guid = "{0x%08X, 0x%04X, 0x%04X, {%s}}" % (iid[0], iid[1], iid[2], ", ".join("0x%02X" % b for b in iid[3:]))
        out.append(f"    {{\"{iface}\", {guid}, M_{iface}, {len(rows)}}},")
    out.append("};")
    out.append(f"#define D3D9T_MAX_SLOTS {max_slots}")
    for extra in ("IDirect3DBaseTexture9", "IDirect3DResource9"):
        iid = iids[extra]
        out.append("static const GUID g_iid_%s = {0x%08X, 0x%04X, 0x%04X, {%s}};" % (
            extra, iid[0], iid[1], iid[2], ", ".join("0x%02X" % b for b in iid[3:])))
    text = "\n".join(out) + "\n"
    total = sum(len(rows) for _, rows in tables)
    print(f"interfaces={len(tables)} methods={total} generated={stats['generated']} "
          f"special={stats['special']} named-stops={stats['stop']} max-slots={max_slots}")
    if not options.check:
        with open(OUT, "w") as handle:
            handle.write(text)
        print("wrote", os.path.relpath(OUT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
