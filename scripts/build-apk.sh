#!/usr/bin/env bash
# Host entrypoint: build a signed, 16 KiB-aligned ARM64 Android APK of
# Burnout Paradise: The Ultimate Box from your own game copy, inside an isolated
# Docker builder container (no network during the build).
#
# Usage:
#   ./scripts/build-apk.sh --game-dir /path/to/BurnoutParadiseUltimateBox [options]
#
# Options:
#   --game-dir DIR          Your Ultimate Box folder (contains BurnoutParadise.exe and DirectX/)
#   --out DIR               Output directory (default: ./out)
#   --work DIR              Persistent build cache (default: ./work)
#   --jobs N                Parallel compiler jobs (default: nproc)
#   --stage STAGE           lift | capture | lib | apk | all (default: all)
#   --capture-minutes N     Length of the headless capture drive (default: 45)
#   --image NAME            Builder image tag (default: burnout-paradise-builder:latest)
#   --no-build-image        Reuse an existing builder image
set -euo pipefail

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
GAME_DIR=""
OUT_DIR="$REPO/out"
WORK_DIR="$REPO/work"
JOBS="$(nproc 2>/dev/null || echo 8)"
STAGE="all"
CAPTURE_MIN=45
IMAGE="burnout-paradise-builder:latest"
BUILD_IMAGE=1

PINNED_EXE_SHA256="fa55ff4d5b0f0f1e7a3dd861ec765d1d81c51554b588ae9872a3f923686f41be"

while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir)        GAME_DIR="$2"; shift 2 ;;
    --out)             OUT_DIR="$2"; shift 2 ;;
    --work)            WORK_DIR="$2"; shift 2 ;;
    --jobs)            JOBS="$2"; shift 2 ;;
    --stage)           STAGE="$2"; shift 2 ;;
    --capture-minutes) CAPTURE_MIN="$2"; shift 2 ;;
    --image)           IMAGE="$2"; shift 2 ;;
    --no-build-image)  BUILD_IMAGE=0; shift ;;
    -h|--help)         sed -n '2,19p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ -n "$GAME_DIR" ] || { echo "ERROR: --game-dir is required (see --help)." >&2; exit 2; }
EXE="$GAME_DIR/BurnoutParadise.exe"
[ -f "$EXE" ] || { echo "ERROR: $EXE not found." >&2; exit 2; }

echo "==> Verifying BurnoutParadise.exe (Steam, 1.1.0.0) SHA-256..."
GOT_SHA="$(sha256sum "$EXE" | awk '{print $1}')"
if [ "$GOT_SHA" != "$PINNED_EXE_SHA256" ]; then
  echo "ERROR: BurnoutParadise.exe SHA-256 mismatch." >&2
  echo "  Expected (Steam 1.1.0.0): $PINNED_EXE_SHA256" >&2
  echo "  Found:                    $GOT_SHA" >&2
  echo "Only this unmodified executable is supported (the seeds and patches are pinned to it)." >&2
  exit 2
fi
echo "    OK"

mkdir -p "$OUT_DIR" "$WORK_DIR"
GAME_DIR="$(CDPATH= cd -- "$GAME_DIR" && pwd)"
OUT_DIR="$(CDPATH= cd -- "$OUT_DIR" && pwd)"
WORK_DIR="$(CDPATH= cd -- "$WORK_DIR" && pwd)"

if [ "$BUILD_IMAGE" -eq 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "==> Building the builder image ($IMAGE)..."
  docker build -f "$REPO/docker/Dockerfile.builder" -t "$IMAGE" "$REPO"
fi

echo "==> Building offline in the container (stage=$STAGE, jobs=$JOBS)..."
docker run --rm --network none \
  -v "$REPO:/repo:ro" \
  -v "$GAME_DIR:/game:ro" \
  -v "$WORK_DIR:/work" \
  -v "$OUT_DIR:/out" \
  -e "UB_STAGE=$STAGE" \
  -e "UB_JOBS=$JOBS" \
  -e "UB_CAPTURE_MINUTES=$CAPTURE_MIN" \
  -e "UB_HOST_UID=$(id -u)" \
  -e "UB_HOST_GID=$(id -g)" \
  "$IMAGE" /repo/scripts/build-inside-docker.sh

echo "==> Finished. Results in: $OUT_DIR"
ls -lh "$OUT_DIR"
