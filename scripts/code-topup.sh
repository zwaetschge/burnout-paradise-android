#!/usr/bin/env bash
# The game loads some code at run time from its sound bundles. If the device runs
# into such code that the capture run did not see, the port stops and saves the
# surrounding memory window (dyn-window-<address>-<base>.bin next to the shader
# cache). This script pulls those windows, translates them in the builder
# container, rebuilds libburnout_static.so and the APK, and installs it.
#
# Usage: ./scripts/code-topup.sh --game-dir DIR [--serial SERIAL] [--work DIR] [--out DIR] [--image NAME]
set -euo pipefail

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
GAME_DIR=""; SERIAL=""; WORK_DIR="$REPO/work"; OUT_DIR="$REPO/out"; IMAGE="burnout-paradise-builder:latest"
FILES=/sdcard/Android/data/dev.plum.burnoutport/files
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; shift 2 ;;
    --serial)   SERIAL="$2"; shift 2 ;;
    --work)     WORK_DIR="$2"; shift 2 ;;
    --out)      OUT_DIR="$2"; shift 2 ;;
    --image)    IMAGE="$2"; shift 2 ;;
    -h|--help)  sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$GAME_DIR" ] || { echo "ERROR: --game-dir is required." >&2; exit 2; }
[ -d "$WORK_DIR/gen" ] || { echo "ERROR: $WORK_DIR/gen missing - run scripts/build-apk.sh first." >&2; exit 2; }
ADB=(adb); [ -n "$SERIAL" ] && ADB+=(-s "$SERIAL")
mkdir -p "$WORK_DIR/topup-code"
windows=$("${ADB[@]}" shell "cd $FILES && ls dyn-window-*.bin 2>/dev/null" | tr -d '\r')
[ -n "$windows" ] || { echo "No saved code windows on the device."; exit 0; }
for w in $windows; do "${ADB[@]}" pull "$FILES/$w" "$WORK_DIR/topup-code/$w" >/dev/null; done
echo "==> $(echo "$windows" | wc -w) code windows pulled"

GAME_DIR="$(CDPATH= cd -- "$GAME_DIR" && pwd)"; WORK_DIR="$(CDPATH= cd -- "$WORK_DIR" && pwd)"
mkdir -p "$OUT_DIR"; OUT_DIR="$(CDPATH= cd -- "$OUT_DIR" && pwd)"
for stage in codetopup lib apk; do
  docker run --rm --network none -v "$REPO:/repo:ro" -v "$GAME_DIR:/game:ro" -v "$WORK_DIR:/work" -v "$OUT_DIR:/out" \
    -e "UB_STAGE=$stage" -e "UB_HOST_UID=$(id -u)" -e "UB_HOST_GID=$(id -g)" "$IMAGE" /repo/scripts/build-inside-docker.sh
done
APK="$(ls -1t "$OUT_DIR"/BurnoutParadise-Android-*.apk | head -1)"
"${ADB[@]}" install -r "$APK"
for w in $windows; do "${ADB[@]}" shell rm -f "$FILES/$w"; done
echo "==> Installed the rebuilt APK. Start the game again."
