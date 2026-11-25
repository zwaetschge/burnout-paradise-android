#!/usr/bin/env bash
# Optional: convert the vehicles that only Burnout Paradise Remastered has (cop cars,
# Big Surf Island cars, toy legends) into an overlay for the Ultimate Box port.
# Needs BOTH games: your Ultimate Box folder and your Remastered folder. The overlay
# (./mods) is derived from your game files - never redistribute it.
#
# Usage: ./scripts/build-remastered-cars.sh --game-dir UB_DIR --remastered-dir REMASTERED_DIR [--out DIR]
set -euo pipefail

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
UB=""; RM=""; OUT_DIR="$REPO/mods"; WORK_DIR="$REPO/work"; IMAGE="burnout-paradise-builder:latest"
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir)       UB="$2"; shift 2 ;;
    --remastered-dir) RM="$2"; shift 2 ;;
    --out)            OUT_DIR="$2"; shift 2 ;;
    --image)          IMAGE="$2"; shift 2 ;;
    -h|--help)        sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$UB" ] && [ -n "$RM" ] || { echo "ERROR: --game-dir and --remastered-dir are required." >&2; exit 2; }
mkdir -p "$OUT_DIR" "$WORK_DIR"
UB="$(CDPATH= cd -- "$UB" && pwd)"; RM="$(CDPATH= cd -- "$RM" && pwd)"
OUT_DIR="$(CDPATH= cd -- "$OUT_DIR" && pwd)"; WORK_DIR="$(CDPATH= cd -- "$WORK_DIR" && pwd)"
docker run --rm --network none -v "$REPO:/repo:ro" -v "$UB:/ub:ro" -v "$RM:/rm:ro" \
  -v "$WORK_DIR:/work" -v "$OUT_DIR:/mods" -e UB_DIR=/ub -e REMASTERED_DIR=/rm \
  -e REMASTER_LEARNED=/work/learned-maps.json "$IMAGE" -c '
    set -e
    python3 /repo/tools/remastered_cars/remaster_learn.py
    python3 /repo/tools/remastered_cars/remaster_cars.py /mods
    chown -R '"$(id -u):$(id -g)"' /mods /work'
echo "==> Overlay in $OUT_DIR - copy it with: ./scripts/prepare-game-files.sh ... --mods $OUT_DIR"
