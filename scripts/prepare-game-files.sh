#!/usr/bin/env bash
# Copy your Ultimate Box folder and the build's device files to an Android device
# over adb. Install the APK first (adb install -r out/BurnoutParadise-Android-<version>.apk).
#
# Usage:
#   ./scripts/prepare-game-files.sh --game-dir /path/to/BurnoutParadiseUltimateBox [options]
#
# Options:
#   --game-dir DIR   Your Ultimate Box folder
#   --device DIR     Device files from the build (default: ./out/device: nls/, shader-cache/)
#   --mods DIR       Optional Remastered-vehicles overlay (scripts/build-remastered-cars.sh)
#   --serial SERIAL  adb device serial (default: the only connected device)
set -euo pipefail

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
GAME_DIR=""; DEVICE_DIR="$REPO/out/device"; MODS_DIR=""; SERIAL=""
PINNED_EXE_SHA256="fa55ff4d5b0f0f1e7a3dd861ec765d1d81c51554b588ae9872a3f923686f41be"
PKG_DIR=/sdcard/Android/data/dev.plum.burnoutport/files

while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; shift 2 ;;
    --device)   DEVICE_DIR="$2"; shift 2 ;;
    --mods)     MODS_DIR="$2"; shift 2 ;;
    --serial)   SERIAL="$2"; shift 2 ;;
    -h|--help)  sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$GAME_DIR" ] || { echo "ERROR: --game-dir is required." >&2; exit 2; }
EXE="$GAME_DIR/BurnoutParadise.exe"
[ "$(sha256sum "$EXE" | awk '{print $1}')" = "$PINNED_EXE_SHA256" ] || { echo "ERROR: $EXE is not the supported Steam 1.1.0.0 executable." >&2; exit 2; }
[ -d "$DEVICE_DIR/nls" ] || { echo "ERROR: $DEVICE_DIR/nls missing - run scripts/build-apk.sh first." >&2; exit 2; }

ADB=(adb); [ -n "$SERIAL" ] && ADB+=(-s "$SERIAL")
"${ADB[@]}" shell mkdir -p "$PKG_DIR/BurnoutPR" "$PKG_DIR/nls" "$PKG_DIR/shader-cache"

echo "==> Game files -> $PKG_DIR/BurnoutPR (several GB)"
for f in "$GAME_DIR"/*; do
  name="$(basename "$f")"
  case "$name" in DirectX|*.vdf) continue ;; esac
  "${ADB[@]}" push "$f" "$PKG_DIR/BurnoutPR/"
done
# the runtime maps its main image from <game root>/BurnoutPR.exe
"${ADB[@]}" push "$EXE" "$PKG_DIR/BurnoutPR/BurnoutPR.exe"

echo "==> Code-page tables and shader cache"
"${ADB[@]}" push "$DEVICE_DIR/nls/." "$PKG_DIR/nls/"
[ -d "$DEVICE_DIR/shader-cache" ] && "${ADB[@]}" push "$DEVICE_DIR/shader-cache/." "$PKG_DIR/shader-cache/"

# port.env: start-up settings the runtime needs on Android (an app has no command line).
# PORT_OEP: start at the game's own entry point in .text (the Steam wrapper is skipped,
# not run); PORT_NLS/PORT_SHADER_CACHE: the files pushed above; PORT_OFFLINE: no network.
# Other lines already in the file (your own settings, see README) are kept.
echo "==> port.env"
ENV_TMP="$(mktemp)"; trap 'rm -f "$ENV_TMP"' EXIT
{
  echo "PORT_OEP=0x9AA04B"
  echo "PORT_NLS=$PKG_DIR/nls/c_1252.nls"
  echo "PORT_SHADER_CACHE=$PKG_DIR/shader-cache"
  echo "PORT_OFFLINE=1"
  "${ADB[@]}" shell "cat $PKG_DIR/BurnoutPR/port.env 2>/dev/null" | tr -d '\r' \
    | grep -vE '^(PORT_OEP|PORT_NLS|PORT_SHADER_CACHE|PORT_OFFLINE)=' || true
} > "$ENV_TMP"
"${ADB[@]}" push "$ENV_TMP" "$PKG_DIR/BurnoutPR/port.env" >/dev/null
sed 's/^/    /' "$ENV_TMP"

if [ -n "$MODS_DIR" ]; then
  echo "==> Remastered-vehicles overlay -> $PKG_DIR/BurnoutPR/port-mods"
  "${ADB[@]}" shell mkdir -p "$PKG_DIR/BurnoutPR/port-mods"
  "${ADB[@]}" push "$MODS_DIR/." "$PKG_DIR/BurnoutPR/port-mods/"
fi
echo "==> Done. Start 'Burnout Paradise' on the device."
