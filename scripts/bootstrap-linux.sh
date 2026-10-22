#!/usr/bin/env bash
# Usage: bootstrap-linux.sh [stage0] [output-dir] [--release] [--no-regalloc]
set -euo pipefail
SYNX_BOOT_ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
SYNX_BOOT_STAGE0="${1:-$SYNX_BOOT_ROOT/build-linux/compiler/synx}"
SYNX_BOOT_OUTPUT="${2:-$SYNX_BOOT_ROOT/build-linux-bootstrap}"
if (($#)); then shift; fi
if (($#)); then shift; fi
mkdir -p -- "$SYNX_BOOT_OUTPUT"
export SYNX_STD_PATH="$SYNX_BOOT_ROOT/std"
export SYNX_RUNTIME_LIB="${SYNX_RUNTIME_LIB:-$SYNX_BOOT_ROOT/build-linux/runtime/libsynx_rt.a}"
"$SYNX_BOOT_STAGE0" build "$SYNX_BOOT_ROOT/selfhost/bootstrap.snx" -o "$SYNX_BOOT_OUTPUT/bootstrap-runner"
"$SYNX_BOOT_OUTPUT/bootstrap-runner" "$SYNX_BOOT_STAGE0" "$SYNX_BOOT_ROOT" "$SYNX_BOOT_OUTPUT" "$@"
