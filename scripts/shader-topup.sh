#!/usr/bin/env bash
# The device has no shader compiler. When the game needs a shader that is not in
# the cache, the port writes the request (<key>.dxcq) next to the cache and stops
# with "shader not in cache". This script pulls those requests, compiles them in
# the builder container with the game's own d3dx9_37.dll, and pushes the results.
#
# Usage: ./scripts/shader-topup.sh --game-dir DIR [--serial SERIAL] [--work DIR] [--image NAME]
set -euo pipefail

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
GAME_DIR=""; SERIAL=""; WORK_DIR="$REPO/work"; IMAGE="burnout-paradise-builder:latest"
CACHE=/sdcard/Android/data/dev.plum.burnoutport/files/shader-cache
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; shift 2 ;;
    --serial)   SERIAL="$2"; shift 2 ;;
    --work)     WORK_DIR="$2"; shift 2 ;;
    --image)    IMAGE="$2"; shift 2 ;;
    -h|--help)  sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$GAME_DIR" ] || { echo "ERROR: --game-dir is required." >&2; exit 2; }
ADB=(adb); [ -n "$SERIAL" ] && ADB+=(-s "$SERIAL")
mkdir -p "$WORK_DIR/topup"; rm -f "$WORK_DIR"/topup/*.dxcq "$WORK_DIR"/topup/*.dxcr

missing=$("${ADB[@]}" shell "cd $CACHE && for q in *.dxcq; do [ -f \"\${q%.dxcq}.dxcr\" ] || echo \"\$q\"; done" | tr -d '\r')
[ -n "$missing" ] || { echo "No open shader requests on the device."; exit 0; }
for q in $missing; do "${ADB[@]}" pull "$CACHE/$q" "$WORK_DIR/topup/$q" >/dev/null; done
echo "==> $(echo "$missing" | wc -w) requests pulled; compiling"

GAME_DIR="$(CDPATH= cd -- "$GAME_DIR" && pwd)"; WORK_DIR="$(CDPATH= cd -- "$WORK_DIR" && pwd)"
docker run --rm --network none -v "$REPO:/repo:ro" -v "$GAME_DIR:/game:ro" -v "$WORK_DIR:/work" \
  -v "$WORK_DIR/topup-out:/out" -e UB_STAGE=topup -e "UB_HOST_UID=$(id -u)" -e "UB_HOST_GID=$(id -g)" \
  "$IMAGE" /repo/scripts/build-inside-docker.sh

for r in "$WORK_DIR"/topup/*.dxcr; do [ -e "$r" ] && "${ADB[@]}" push "$r" "$CACHE/" >/dev/null; done
echo "==> Pushed $(ls "$WORK_DIR"/topup/*.dxcr 2>/dev/null | wc -l) compiled shaders. Restart the game."
