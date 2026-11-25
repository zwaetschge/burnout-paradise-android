#!/usr/bin/env bash
# Build the Burnout Paradise Android APK (A17).
#
# No Gradle, no network, no game assets. Everything comes from the local NDK +
# SDK and from locally built .so files:
#   NDK  $ANDROID_NDK_ROOT (r27)
#   SDK  $ANDROID_SDK_ROOT (build-tools 36.0.0, android-36)
#
# Pipeline: clang -> ELF checks (aarch64 + 16 KB LOAD alignment + NEEDED
# closure) -> aapt2 -> zip (STORED) -> zipalign -P 16 4 -> APK Signature
# Scheme v2 (pure Python: this host has no JDK) -> verification -> JSON report.
#
# Packed into lib/arm64-v8a/:
#   libport_main.so            Android front end (android/jni/port_main.c)
#   libburnout_static.so       newest locally built static image, renamed
#   libdxvk_d3d11.so           port/build/dxvk-android/lib/*.so
#   libdxvk_dxgi.so
#   libguest_d3d11_thunk.so    port/build/d3d11-thunk/ (if present)
#   libc++_shared.so           NDK aarch64 (DXVK needs it)
#
# Nothing else is packaged: no game code beyond the locally built .so, no
# assets, no license data.
#
# Usage: tools/build_apk.sh [--clean] [--no-static] [--static-so PATH]
#                           [--no-dxvk] [-j N]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:?set ANDROID_SDK_ROOT}"
NDK="${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}"
BT="$SDK/build-tools/36.0.0"
PLATFORM="$SDK/platforms/android-36/android.jar"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
CC="$TOOLCHAIN/bin/aarch64-linux-android29-clang"
STRIP="$TOOLCHAIN/bin/llvm-strip"
READELF="$TOOLCHAIN/bin/llvm-readelf"
NM="$TOOLCHAIN/bin/llvm-nm"
GLUE="$NDK/sources/android/native_app_glue"
LIBCXX="$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"

APP_DIR="$ROOT/android"
OUT_DIR="${APK_OUT_DIR:-$ROOT/out}"
WORK="$OUT_DIR/work"
LIBDIR="$WORK/lib/arm64-v8a"
PYTHON="${PYTHON:-$(command -v python3)}"

API=29
MIN_SDK=29
TARGET_SDK=36
PKG=dev.plum.burnoutport
VERSION_CODE=2
VERSION_NAME=0.2-a17
JOBS=6
CLEAN=0
WITH_STATIC=1
WITH_DXVK=1
STATIC_SO=""

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        --no-static) WITH_STATIC=0 ;;
        --no-dxvk) WITH_DXVK=0 ;;
        -j) shift; JOBS="${1:-6}" ;;
        --static-so) shift; STATIC_SO="${1:?--static-so needs a path}" ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

log() { printf '[build_apk] %s\n' "$*"; }
die() { printf '[build_apk] ERROR: %s\n' "$*" >&2; exit 1; }

for f in "$CC" "$READELF" "$NM" "$BT/aapt2" "$BT/zipalign" "$PLATFORM" \
         "$GLUE/android_native_app_glue.c" "$LIBCXX"; do
    [ -e "$f" ] || die "missing toolchain component: $f"
done
[ -x "$PYTHON" ] || die "python not found: $PYTHON"

if [ "$CLEAN" = 1 ]; then
    log "clean: removing $WORK"
    rm -rf "$WORK"
fi
# The lib staging dir is always rebuilt from scratch: a plain (non --clean)
# build must never repackage a .so from an earlier run.
rm -rf "$LIBDIR"
mkdir -p "$WORK/obj" "$WORK/res" "$LIBDIR" "$OUT_DIR"

# ---------------------------------------------------------------------------
# ELF helper (pure Python: LOAD alignment, machine, DT_NEEDED; no binutils)
# ---------------------------------------------------------------------------
cat > "$WORK/elfinfo.py" <<'PY'
#!/usr/bin/env python3
"""Minimal ELF64 reader: machine, PT_LOAD alignment, DT_NEEDED. No deps."""
import json
import struct
import sys

EM_AARCH64 = 0xB7
PT_LOAD, PT_DYNAMIC = 1, 2
DT_NULL, DT_NEEDED, DT_STRTAB = 0, 1, 5


def read_elf(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise SystemExit(f"{path}: not a little-endian ELF64 file")
    e_machine = struct.unpack_from("<H", data, 18)[0]
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]
    loads, dyn = [], None
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        (p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz,
         p_align) = struct.unpack_from("<IIQQQQQQ", data, off)
        if p_type == PT_LOAD:
            loads.append(dict(offset=p_offset, vaddr=p_vaddr, filesz=p_filesz,
                              memsz=p_memsz, align=p_align, flags=p_flags))
        elif p_type == PT_DYNAMIC:
            dyn = dict(offset=p_offset, filesz=p_filesz)
    needed = []
    if dyn and dyn["filesz"]:
        tags = []
        for i in range(dyn["filesz"] // 16):
            tag, val = struct.unpack_from("<qQ", data, dyn["offset"] + i * 16)
            if tag == DT_NULL:
                break
            tags.append((tag, val))
        strtab = next((v for t, v in tags if t == DT_STRTAB), None)
        if strtab is not None:
            foff = None
            for seg in loads:
                if seg["vaddr"] <= strtab < seg["vaddr"] + seg["filesz"]:
                    foff = seg["offset"] + (strtab - seg["vaddr"])
                    break
            if foff is not None:
                for tag, val in tags:
                    if tag != DT_NEEDED:
                        continue
                    end = data.index(b"\0", foff + val)
                    needed.append(data[foff + val:end].decode("utf-8", "replace"))
    return dict(path=path, machine=e_machine,
                machine_name="aarch64" if e_machine == EM_AARCH64 else hex(e_machine),
                loads=loads, needed=needed,
                min_load_align=min((l["align"] for l in loads), default=0))


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: elfinfo.py <file.so> [...]")
    print(json.dumps([read_elf(p) for p in argv[1:]], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
PY

# ---------------------------------------------------------------------------
# 1. native front end
# ---------------------------------------------------------------------------
CFLAGS=(
    --target=aarch64-linux-android$API
    --sysroot="$TOOLCHAIN/sysroot"
    -O2 -g -fPIC -fno-omit-frame-pointer
    -Wall -Wextra -Werror
    -DANDROID
    -I"$GLUE"
)
GLUE_CFLAGS=(
    --target=aarch64-linux-android$API
    --sysroot="$TOOLCHAIN/sysroot"
    -O2 -fPIC
    -DANDROID
    -I"$GLUE"
)
LDFLAGS=(
    -shared
    -Wl,-z,max-page-size=16384
    -Wl,-z,noexecstack
    -Wl,--no-undefined
    -Wl,--fatal-warnings
    -llog -landroid -lEGL -lGLESv2 -lm -ldl
)

log "compiling native_app_glue.c"
"$CC" "${GLUE_CFLAGS[@]}" -c "$GLUE/android_native_app_glue.c" \
    -o "$WORK/obj/android_native_app_glue.o"

log "compiling port_main.c + port_host_api.h"
"$CC" "${CFLAGS[@]}" -c "$APP_DIR/jni/port_main.c" -o "$WORK/obj/port_main.o"

log "linking libport_main.so (arm64-v8a, 16 KB max-page-size)"
rm -f "$LIBDIR/libport_main.so"
"$CC" "${LDFLAGS[@]}" "$WORK/obj/port_main.o" \
    "$WORK/obj/android_native_app_glue.o" -o "$LIBDIR/libport_main.so"
"$STRIP" --strip-unneeded "$LIBDIR/libport_main.so"

# ---------------------------------------------------------------------------
# 2. static image: newest local build, in the documented priority order
# ---------------------------------------------------------------------------
# grep -c (not -q) so nm is never killed by SIGPIPE under `set -o pipefail`.
exports_port_entry() {
    local out n
    out="$("$NM" -D --defined-only "$1" 2>/dev/null || true)"
    n=$(printf '%s\n' "$out" | grep -cE ' T port_entry$' || true)
    [ "${n:-0}" -gt 0 ]
}

pick_static_image() {
    local cand
    # priority 1: newest port/build/static-full-v2*/…so
    for cand in $(ls -1dt "$ROOT"/port/build/static-full-v2*/ 2>/dev/null); do
        local f
        for f in "$cand"libburnout_static*.so; do
            [ -f "$f" ] || continue
            if exports_port_entry "$f"; then printf '%s\n' "$f"; return 0; fi
            log "ignoring $f (no exported port_entry)" >&2
        done
    done
    # priority 2: static-full-v1-entry2, priority 3: static-full-v1-entry
    for cand in "$ROOT"/port/build/static-full-v1-entry2/libburnout_static_entry.so \
                "$ROOT"/port/build/static-full-v1-entry/libburnout_static_entry.so; do
        [ -f "$cand" ] || continue
        if exports_port_entry "$cand"; then printf '%s\n' "$cand"; return 0; fi
        log "ignoring $cand (no exported port_entry)" >&2
    done
    return 1
}

STATIC_SOURCE=""
if [ "$WITH_STATIC" = 1 ]; then
    if [ -n "$STATIC_SO" ]; then
        [ -f "$STATIC_SO" ] || die "--static-so $STATIC_SO does not exist"
        exports_port_entry "$STATIC_SO" || die "$STATIC_SO does not export port_entry"
        STATIC_SOURCE="$STATIC_SO"
    else
        STATIC_SOURCE="$(pick_static_image || true)"
    fi
fi

if [ -n "$STATIC_SOURCE" ]; then
    log "packaging static image: $STATIC_SOURCE"
    log "  -> lib/arm64-v8a/libburnout_static.so"
    cp -f "$STATIC_SOURCE" "$LIBDIR/libburnout_static.so"
else
    log "no static image found -> shell-only APK (front end logs"
    log "  'static image missing' and keeps the EGL heartbeat alive)"
fi

# ---------------------------------------------------------------------------
# 3. DXVK + thunk + libc++
# ---------------------------------------------------------------------------
if [ "$WITH_DXVK" = 1 ]; then
    shopt -s nullglob
    dxvk_libs=("${DXVK_LIB_DIR:-$ROOT/work/dxvk-android/lib}"/*.so)
    shopt -u nullglob
    if [ ${#dxvk_libs[@]} -eq 0 ]; then
        log "no DXVK libraries in port/build/dxvk-android/lib -> skipping"
    else
        for f in "${dxvk_libs[@]}"; do
            log "packaging DXVK lib: $(basename "$f")"
            cp -f "$f" "$LIBDIR/"
        done
    fi
else
    log "--no-dxvk: DXVK libraries not packaged"
fi

if [ "$WITH_DXVK" = 0 ]; then
    log "--no-dxvk: skipping the D3D11 thunk too (it NEEDs libdxvk_d3d11/dxgi)"
elif [ -f "$ROOT/port/build/d3d11-thunk/libguest_d3d11_thunk.so" ]; then
    log "packaging D3D11 thunk: libguest_d3d11_thunk.so"
    cp -f "$ROOT/port/build/d3d11-thunk/libguest_d3d11_thunk.so" "$LIBDIR/"
else
    log "no port/build/d3d11-thunk/libguest_d3d11_thunk.so -> skipping thunk"
fi

log "packaging NDK libc++_shared.so (aarch64, required by DXVK)"
cp -f "$LIBCXX" "$LIBDIR/libc++_shared.so"

# ---------------------------------------------------------------------------
# 4. ELF checks: aarch64, 16 KB LOAD alignment, NEEDED closure
# ---------------------------------------------------------------------------
log "checking ELF machine / 16 KB LOAD alignment / NEEDED closure"
"$PYTHON" - "$WORK/elfinfo.py" "$LIBDIR" "$OUT_DIR/elf-check.json" <<'PY'
import json
import pathlib
import subprocess
import sys

elfinfo, libdir, out = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3]
libs = sorted(libdir.glob("*.so"))
report = subprocess.run([sys.executable, elfinfo] + [str(p) for p in libs],
                        capture_output=True, text=True, check=True)
infos = json.loads(report.stdout)

# Libraries that every Android device provides; anything else has to be in the
# APK. This is the resolution check the task asks for, not a loader emulation.
SYSTEM = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
    "libEGL.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so", "libz.so",
    "libjnigraphics.so", "libstdc++.so", "libOpenSLES.so", "libmediandk.so",
    "libnativewindow.so", "libsync.so", "libbinder_ndk.so", "libui.so",
    "libutils.so", "libcutils.so", "libhardware.so", "libnativehelper.so",
    "libicuuc.so", "libicui18n.so", "libcrypto.so", "libssl.so",
    "libsqlite.so", "libneuralnetworks.so", "libaaudio.so", "libamidi.so",
    "libcamera2ndk.so", "libhidlbase.so", "libvndksupport.so", "libbase.so",
    "libnetd_client.so", "libprocessgroup.so", "libion.so", "libdmabufheap.so",
    "libbacktrace.so", "libunwindstack.so", "liblzma.so", "libbrotli.so",
}
packaged = {p.name for p in libs}
problems = []
for info in infos:
    name = pathlib.Path(info["path"]).name
    if info["machine_name"] != "aarch64":
        problems.append(f"{name}: machine {info['machine_name']} (expected aarch64)")
    if info["min_load_align"] < 0x4000:
        problems.append(f"{name}: LOAD alignment 0x{info['min_load_align']:x} "
                        f"< 0x4000 (16 KB pages)")
    unresolved = [n for n in info["needed"]
                  if n not in packaged and n not in SYSTEM]
    if unresolved:
        problems.append(f"{name}: unresolved NEEDED {unresolved}")
    info["name"] = name
    info["resolution"] = {n: ("apk" if n in packaged else "system")
                          for n in info["needed"]}

json.dump({"libs": infos, "problems": problems}, open(out, "w"), indent=2)
for info in infos:
    print(f"  {info['name']:<28} align=0x{info['min_load_align']:x} "
          f"needed={','.join(info['needed']) or '-'}")
if problems:
    for p in problems:
        print(f"[build_apk] ERROR: {p}", file=sys.stderr)
    raise SystemExit(1)
PY

# ---------------------------------------------------------------------------
# 5. resources + manifest
# ---------------------------------------------------------------------------
log "aapt2 compile (resources)"
rm -rf "$WORK/res" && mkdir -p "$WORK/res"
"$BT/aapt2" compile --dir "$APP_DIR/res" -o "$WORK/res/" >/dev/null

log "aapt2 link (manifest + resources -> unsigned base.apk)"
UNSIGNED="$WORK/base-unsigned.apk"
rm -f "$UNSIGNED"
"$BT/aapt2" link \
    -o "$UNSIGNED" \
    -I "$PLATFORM" \
    --manifest "$APP_DIR/AndroidManifest.xml" \
    --min-sdk-version "$MIN_SDK" \
    --target-sdk-version "$TARGET_SDK" \
    --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
    --auto-add-overlay \
    "$WORK"/res/*.flat

# ---------------------------------------------------------------------------
# 6. add the native libs (STORED, aligned later)
# ---------------------------------------------------------------------------
log "adding native libs to the APK (uncompressed)"
"$PYTHON" - "$UNSIGNED" "$LIBDIR" <<'PY'
import pathlib
import sys
import zipfile

apk, libdir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
DATE_TIME = (1980, 1, 1, 0, 0, 0)  # fixed: byte-identical rebuilds
total = 0
with zipfile.ZipFile(apk, "a", zipfile.ZIP_STORED) as zf:
    for so in sorted(libdir.glob("*.so")):
        arcname = f"lib/arm64-v8a/{so.name}"
        zi = zipfile.ZipInfo(arcname, date_time=DATE_TIME)
        zi.compress_type = zipfile.ZIP_STORED
        zi.external_attr = 0o644 << 16
        zf.writestr(zi, so.read_bytes())
        size = so.stat().st_size
        total += size
        print(f"[build_apk]   + {arcname} ({size} bytes)")
print(f"[build_apk]   native lib payload: {total} bytes "
      f"({total / (1024 * 1024):.1f} MiB)")
PY

# ---------------------------------------------------------------------------
# 7. align + sign + verify
# ---------------------------------------------------------------------------
log "zipalign (-P 16 4: 16 KB page-aligned uncompressed .so)"
ALIGNED="$WORK/base-aligned.apk"
rm -f "$ALIGNED"
"$BT/zipalign" -P 16 -f 4 "$UNSIGNED" "$ALIGNED"

log "verifying alignment (zipalign -c -P 16 4)"
"$BT/zipalign" -c -P 16 4 "$ALIGNED" || die "zipalign verification failed"

log "signing (APK Signature Scheme v2, self-signed debug cert)"
SIGNED="$OUT_DIR/burnoutport-debug.apk"
rm -f "$SIGNED"
"$PYTHON" "$ROOT/tools/apk_sign_v2.py" sign \
    --in "$ALIGNED" --out "$SIGNED" --keydir "$OUT_DIR" \
    | tee "$OUT_DIR/sign.json"

log "verifying v2 signature"
"$PYTHON" "$ROOT/tools/apk_sign_v2.py" verify --apk "$SIGNED" --keydir "$OUT_DIR" \
    | tee "$OUT_DIR/verify.json"

log "aapt2 dump badging"
"$BT/aapt2" dump badging "$SIGNED" | tee "$OUT_DIR/badging.txt"

log "unzip -l"
unzip -l "$SIGNED" | tee "$OUT_DIR/contents.txt"

# ---------------------------------------------------------------------------
# 8. JSON report
# ---------------------------------------------------------------------------
log "writing $OUT_DIR/apk-report.json"
"$PYTHON" - "$OUT_DIR" "$LIBDIR" "$STATIC_SOURCE" "$OUT_DIR/elf-check.json" \
    "$MIN_SDK" "$TARGET_SDK" "$PKG" "$VERSION_CODE" "$VERSION_NAME" <<'PY'
import hashlib
import json
import pathlib
import sys
import zipfile

(out_dir, libdir, static_source, elfcheck, min_sdk, target_sdk, pkg,
 version_code, version_name) = sys.argv[1:10]
out_dir, libdir = pathlib.Path(out_dir), pathlib.Path(libdir)
apk = out_dir / "burnoutport-debug.apk"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


elf = json.loads(pathlib.Path(elfcheck).read_text())
elf_by_name = {i["name"]: i for i in elf["libs"]}
entries = []
with zipfile.ZipFile(apk) as zf:
    for info in zf.infolist():
        entries.append({
            "name": info.filename,
            "size": info.file_size,
            "compressed_size": info.compress_size,
            "method": "stored" if info.compress_type == zipfile.ZIP_STORED
                      else "deflate",
            "header_offset": info.header_offset,
        })

libs = []
for so in sorted(libdir.glob("*.so")):
    info = elf_by_name.get(so.name, {})
    libs.append({
        "apk_name": f"lib/arm64-v8a/{so.name}",
        "source": static_source if so.name == "libburnout_static.so"
                  else str(so),
        "size": so.stat().st_size,
        "sha256": sha256(so),
        "machine": info.get("machine_name"),
        "min_load_align": info.get("min_load_align"),
        "needed": info.get("needed", []),
        "needed_resolution": info.get("resolution", {}),
    })

report = {
    "apk": str(apk),
    "size": apk.stat().st_size,
    "sha256": sha256(apk),
    "package": pkg,
    "version_code": int(version_code),
    "version_name": version_name,
    "min_sdk": int(min_sdk),
    "target_sdk": int(target_sdk),
    "abi": "arm64-v8a",
    "signature": "APK Signature Scheme v2 (RSA-2048/PKCS#1 v1.5/SHA-256)",
    "zipalign": "-P 16 4 (verified)",
    "static_image": {
        "source": static_source or None,
        "packaged_as": "lib/arm64-v8a/libburnout_static.so"
                       if static_source else None,
    },
    "libs": libs,
    "entries": entries,
    "unresolved_needed": elf["problems"],
    "game_assets_included": False,
}
(out_dir / "apk-report.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"[build_apk] apk={apk}")
print(f"[build_apk] size={report['size']} bytes "
      f"({report['size'] / (1024 * 1024):.1f} MiB)")
print(f"[build_apk] sha256={report['sha256']}")
print(f"[build_apk] libs={len(libs)} entries={len(entries)} "
      f"unresolved_needed={len(elf['problems'])}")
PY

{
    printf 'apk=%s\n' "$SIGNED"
    printf 'size=%s\n' "$(stat -c %s "$SIGNED")"
    printf 'sha256=%s\n' "$(sha256sum "$SIGNED" | cut -d' ' -f1)"
    printf 'static_image=%s\n' "${STATIC_SOURCE:-none}"
    printf 'min_sdk=%s target_sdk=%s abi=arm64-v8a package=%s\n' \
        "$MIN_SDK" "$TARGET_SDK" "$PKG"
    printf 'libs=%s\n' "$(cd "$LIBDIR" && ls -1 *.so | tr '\n' ' ')"
    printf 'libport_main_sha256=%s\n' \
        "$(sha256sum "$LIBDIR/libport_main.so" | cut -d' ' -f1)"
} | tee "$OUT_DIR/build-info.txt"

log "OK -> $SIGNED"
