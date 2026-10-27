#!/usr/bin/env bash
# Builds an installable Synx package on Linux or macOS.
# Usage: scripts/package-linux.sh
# Installs the compiler built by the seed release (stage 2) and checks it.
# Output: dist/packages/synx-<version>-<os>-x86_64.tar.gz
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
case "$(uname -s)" in Darwin) OS=macos ;; *) OS=linux ;; esac
BUILD="$ROOT/build-package-$OS"
VERSION="$(sed -n 's/^project(synx VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
NAME="synx-$VERSION-$OS-x86_64"
STAGE="$BUILD/stage/$NAME"
OUT="$ROOT/dist/packages"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target synx_compiler synx_rt synx_rt_shared synx_rt_host
rm -rf -- "$BUILD/stage"
cmake --install "$BUILD" --prefix "$STAGE"

# The package holds only the compiler, runtime, standard library sources, the
# workbench example, the installer, and the license files.
unexpected="$(cd -- "$STAGE" && find . -type f | sed 's|^\./||' | grep -Ev \
  '^(bin/synx|lib/(libsynx_rt\.(a|so|dylib)|synx-host)|install\.sh|LICENSE-(MIT|APACHE)|std/.+\.snx|examples/workbench/[a-z_]+\.snx)$' || true)"
if [[ -n "$unexpected" ]]; then echo "unexpected package files:" >&2; echo "$unexpected" >&2; exit 1; fi

# Smoke test without environment overrides: the compiler finds std/ and lib/
# from its own path. Then install, run, and uninstall through install.sh.
CHECK="$BUILD/check"
rm -rf -- "$CHECK"; mkdir -p -- "$CHECK"
run() { env -u SYNX_STD_PATH -u SYNX_RUNTIME_LIB -u SYNX_RUNTIME_SHARED -u SYNX_LINKER -u SYNX_LINKER_DRIVER -u SYNX_RUNTIME_HOST -u SYNX_CACHE_DIR "$@"; }
native() { run /usr/bin/env PATH=/nonexistent "$@"; }
native "$STAGE/bin/synx" version
native "$STAGE/bin/synx" build "$STAGE/examples/workbench/main.snx" -o "$CHECK/workbench" --linker=builtin
native "$CHECK/workbench" < /dev/null > "$CHECK/workbench.out"
printf 'main(){}' > "$CHECK/format.snx"
native "$STAGE/bin/synx" fmt "$CHECK/format.snx"
native "$STAGE/bin/synx" fmt --check "$CHECK/format.snx"
if [[ "$(cat "$CHECK/format.snx")" != 'main() {}' ]]; then echo "packaged formatter output mismatch" >&2; exit 1; fi
run "$STAGE/install.sh" --prefix "$CHECK/home/synx" --bindir "$CHECK/home/bin" --no-path
# The package links with the built-in linker without a flag.
native "$CHECK/home/bin/synx" build "$STAGE/examples/workbench/main.snx" -o "$CHECK/workbench-installed"
native "$CHECK/workbench-installed" < /dev/null > /dev/null
run "$CHECK/home/synx/install.sh" --prefix "$CHECK/home/synx" --bindir "$CHECK/home/bin" --uninstall
if [[ -e "$CHECK/home/synx" || -L "$CHECK/home/bin/synx" ]]; then echo "uninstall left files behind" >&2; exit 1; fi

mkdir -p -- "$OUT"
rm -f -- "$OUT/$NAME.tar.gz"
(cd -- "$BUILD/stage" && tar -czf "$OUT/$NAME.tar.gz" "$NAME")
echo "Package: $OUT/$NAME.tar.gz"
