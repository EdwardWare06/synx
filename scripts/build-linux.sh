#!/usr/bin/env bash
# Build and test the Synx compiler on Linux or macOS.
# Usage: scripts/build-linux.sh [Debug|Release]
set -euo pipefail
CONFIG="${1:-Debug}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-linux"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
"$BUILD/compiler/synx" version
