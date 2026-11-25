#!/usr/bin/env bash
# Runs inside the builder container (docker/Dockerfile.builder), without network.
# Mounts: /repo (ro, this repository), /game (ro, your Ultimate Box folder),
#         /work (build cache), /out (results).
# Stages (UB_STAGE): lift | capture | lib | apk | all
#   lift     BurnoutParadise.exe .text -> C (pcrecomp + seeds), trampolines, port patches
#   capture  headless host run (lavapipe + DXVK): collects code the game loads at run
#            time (sound bundles) and compiles the game's shaders with its own
#            d3dx9_37.dll under Wine -> dynamic-code pack + shader cache
#   lib      ARM64 libburnout_static.so
#   apk      signed APK + device files (shader cache, NLS tables)
#   topup    (not part of all) compile shader requests the device wrote (/work/topup/*.dxcq)
#   codetopup (not part of all) translate code windows the device saved
#            (/work/topup-code/dyn-window-*.bin) into the dynamic-code pack; then run lib + apk
set -euo pipefail

STAGE="${UB_STAGE:-all}"
JOBS="${UB_JOBS:-$(nproc)}"
CAPTURE_MIN="${UB_CAPTURE_MINUTES:-45}"
REPO=/repo; GAME=/game; WORK=/work; OUT=/out
PY="$(command -v python3)"
EXE="$GAME/BurnoutParadise.exe"
SEEDS="$REPO/seeds/ub-steam-1.1.0.0-functions.txt"
GEN="$WORK/gen"; HOT="$WORK/hot"; DYN="$WORK/dyn/static_dyn.c"; CACHE="$WORK/shader-cache"
DXVK_INC=/opt/upstream/dxvk/include

log() { printf '\n==> %s\n' "$*"; }
want() { { [ "$STAGE" = all ] && [ "$1" != topup ]; } || [ "$STAGE" = "$1" ]; }
fix_owner() { [ -n "${UB_HOST_UID:-}" ] && chown -R "$UB_HOST_UID:${UB_HOST_GID:-$UB_HOST_UID}" "$WORK" "$OUT" 2>/dev/null || true; }
trap fix_owner EXIT

# Shader compiler: the game's own d3dx9_37.dll (DirectX redistributable in the game
# folder) driven by tools/d3dxcc under Wine.
setup_d3dxcc() {
  # shader compiler: the game's own d3dx9_37.dll from its DirectX redistributable, under Wine
  mkdir -p "$WORK/d3dxcc"
  if [ ! -f "$WORK/d3dxcc/d3dx9_37.dll" ]; then
    CAB="$(find "$GAME" -iname 'Mar2008_d3dx9_37_x86.cab' | head -1)"
    [ -n "$CAB" ] || { echo "ERROR: DirectX/Mar2008_d3dx9_37_x86.cab not found in the game folder" >&2; exit 2; }
    cabextract -q -F d3dx9_37.dll -d "$WORK/d3dxcc" "$CAB"
  fi
  zig-cc -target x86-windows-gnu -O2 -o "$WORK/d3dxcc/d3dxcc.exe" "$SRC/tools/d3dxcc/d3dxcc.c"
  export WINEPREFIX=/tmp/ub-wineprefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;d3dx9_37=n"
  [ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1 || true
  cat > "$WORK/d3dxcc/run.sh" <<EOF
#!/bin/sh
export WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;d3dx9_37=n"
cd "$WORK/d3dxcc" && exec wine d3dxcc.exe "Z:\$1" "Z:\$2"
EOF
  chmod +x "$WORK/d3dxcc/run.sh"
}

# The repository is mounted read-only; tools that write next to themselves run from a copy.
rm -rf "$WORK/src" && mkdir -p "$WORK/src"
tar -C "$REPO" --exclude=./.git --exclude=./work --exclude=./out --exclude=./mods -cf - . | tar -C "$WORK/src" -xf -
SRC="$WORK/src"

if want lift; then
  log "Lift: $EXE (.text) -> $GEN"
  rm -rf "$GEN"
  $PY "$SRC/tools/static_full_lift.py" --v2 --gap-entries --sections .text --exe "$EXE" \
      --seed-file "$SEEDS" --out "$GEN" --jobs "$JOBS" --no-syntax-check
  log "Trampolines"
  $PY "$SRC/tools/static_trampoline_pass.py" "$GEN" --exe "$EXE" --jobs "$JOBS"
  log "Port patches"
  $PY "$SRC/tools/port_patches.py" "$GEN"
fi

if want capture; then
  log "Capture: host build"
  $PY "$SRC/scripts/lib/build-host.py" --gen "$GEN" --dxvk-include "$DXVK_INC" --out "$WORK/host" -j "$JOBS"
  setup_d3dxcc
  # game folder as the runtime expects it: <root>/BurnoutPR.exe
  rm -rf "$WORK/hostgame" && mkdir -p "$WORK/hostgame" "$WORK/hostuser" "$HOT" "$CACHE"
  for f in "$GAME"/*; do ln -s "$f" "$WORK/hostgame/$(basename "$f")"; done
  ln -sf "$EXE" "$WORK/hostgame/BurnoutPR.exe"
  # title -> save notice -> main menu -> Enter Paradise City (A every 7 s), then drive
  # around in a 20 s cycle (throttle + left, straight, throttle + right, reverse) so the
  # car does not stay against a wall and new districts, sounds and shaders come up
  SCRIPT="$(seq 20 7 419 | sed 's/$/:pad:A:0.3/' | paste -sd, -)"
  for t in $(seq 430 20 $((CAPTURE_MIN * 60))); do
    SCRIPT="$SCRIPT,$t:pad:RT+LX-:3,$((t + 3)):pad:RT:8,$((t + 11)):pad:RT+LX+:3,$((t + 14)):pad:LT:4"
  done
  log "Capture: headless run, ${CAPTURE_MIN} min"
  set +e
  PORT_OEP=0x9AA04B PORT_OFFLINE=1 PORT_HOT=1 PORT_HOT_EXE="$EXE" PORT_HOT_GEN="$GEN" PORT_HOT_OUT="$HOT" \
  PORT_HOT_SECTIONS=.text PORT_HOT_HELPER="$PY -u $SRC/tools/static_hot_helper.py" UB_HOST_CC=zig-cc \
  PORT_SHADER_CACHE="$CACHE" PORT_D3DX_HELPER="$WORK/d3dxcc/run.sh" \
  DXVK_WSI_DRIVER=Android PORT_DXVK_D3D9=/opt/dxvk-host/lib/libdxvk_d3d9.so PORT_INPUT_SCRIPT="$SCRIPT" \
  timeout $((CAPTURE_MIN * 60 + 60)) "$WORK/host/ub_host" --game "$WORK/hostgame" --user "$WORK/hostuser" \
      --nls /opt/nls/c_1252.nls > "$WORK/capture.log" 2>&1
  echo "capture run ended (status $?)"
  set -e
  mkdir -p "$(dirname "$DYN")"
  $PY "$SRC/tools/static_dyn_pack.py" "$HOT" "$(dirname "$DYN")"
  echo "shader cache: $(ls "$CACHE" | grep -c '\.dxcr$') compiled shaders"
fi

if want lib; then
  log "ARM64 static image"
  DYNARG=(); [ -f "$DYN" ] && DYNARG=(--dyn "$DYN")
  $PY "$SRC/scripts/lib/build-android-lib.py" --gen "$GEN" --dxvk-include "$DXVK_INC" "${DYNARG[@]}" \
      --out "$WORK/android-lib" -j "$JOBS"
fi

if want apk; then
  log "APK"
  KEYDIR="$WORK/apk-keys"; mkdir -p "$KEYDIR"
  APK_OUT_DIR="$WORK/apk" DXVK_LIB_DIR=/opt/dxvk-android/lib PYTHON="$PY" \
      bash "$SRC/tools/build_apk.sh" --static-so "$WORK/android-lib/libburnout_static.so"
  APK="$(ls -1t "$WORK"/apk/*.apk | head -1)"
  VERSION="$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$SRC/android/AndroidManifest.xml")"
  NAME="BurnoutParadise-Android-${VERSION:-dev}.apk"
  cp -f "$APK" "$OUT/$NAME"
  rm -rf "$OUT/device" && mkdir -p "$OUT/device/nls"
  cp -f /opt/nls/c_1252.nls /opt/nls/sortdefault.nls "$OUT/device/nls/"
  [ -d "$CACHE" ] && cp -a "$CACHE" "$OUT/device/shader-cache"
  (cd "$OUT" && sha256sum "$NAME" > SHA256SUMS.txt)
  log "Done: $OUT/$NAME"
fi

if want topup; then
  log "Shader top-up: compiling device requests in $WORK/topup"
  setup_d3dxcc
  n=0
  for q in "$WORK"/topup/*.dxcq; do
    [ -e "$q" ] || continue
    r="${q%.dxcq}.dxcr"; [ -f "$r" ] && continue
    "$WORK/d3dxcc/run.sh" "$q" "$r" && n=$((n + 1))
  done
  echo "compiled $n shader requests"
fi

if [ "$STAGE" = codetopup ]; then
  log "Code top-up: device windows in $WORK/topup-code"
  mkdir -p "$HOT" "$(dirname "$DYN")"
  for w in "$WORK"/topup-code/dyn-window-*.bin; do
    [ -e "$w" ] || continue
    b="$(basename "$w" .bin)"; va="${b#dyn-window-}"; va="${va%%-*}"; base="${b##*-}"
    echo "DYN 0x$va 0x$base $w"
  done | PORT_HOT_EXE="$EXE" PORT_HOT_GEN="$GEN" PORT_HOT_OUT="$HOT" UB_HOST_CC=zig-cc \
         $PY "$SRC/tools/static_hot_helper.py" | grep -v '^READY'
  $PY "$SRC/tools/static_dyn_pack.py" "$HOT" "$(dirname "$DYN")"
fi
