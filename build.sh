#!/bin/sh
# Builds the Android APK from your own Burnout Paradise: The Ultimate Box (Steam) folder, in Docker.
#   ./build.sh "/path/to/Burnout Paradise The Ultimate Box"   (the folder with BurnoutParadise.exe)
# Result: out/BurnoutParadise-Android-<version>.apk and out/device/   (see README.md)
# More options (--capture-minutes N, --stage …) are passed on to scripts/build-apk.sh;
# JOBS=4 ./build.sh … limits the parallel compilers.
set -e
cd "$(dirname "$0")"
GAME=${1:?usage: ./build.sh <game folder> [options]}
shift
exec ./scripts/build-apk.sh --game-dir "$GAME" ${JOBS:+--jobs "$JOBS"} "$@"
