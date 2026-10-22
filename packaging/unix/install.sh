#!/bin/sh
# Installs this Synx package for the current user.
#   ./install.sh                     copy to ~/.local/share/synx, link ~/.local/bin/synx
#   ./install.sh --prefix DIR        copy to DIR
#   ./install.sh --bindir DIR        create the synx link in DIR
#   ./install.sh --no-path           do not add the link directory to ~/.profile
#   ./install.sh --uninstall         remove the installed copy and its link
set -eu

prefix="${HOME}/.local/share/synx"
bindir="${HOME}/.local/bin"
update_path=1
uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) [ $# -ge 2 ] || { echo "install.sh: --prefix needs a directory" >&2; exit 1; }; prefix="$2"; shift 2 ;;
    --bindir) [ $# -ge 2 ] || { echo "install.sh: --bindir needs a directory" >&2; exit 1; }; bindir="$2"; shift 2 ;;
    --no-path) update_path=0; shift ;;
    --uninstall) uninstall=1; shift ;;
    *) echo "install.sh: unknown option $1" >&2; exit 1 ;;
  esac
done

source_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
mkdir -p -- "$prefix"
prefix="$(cd -- "$prefix" && pwd)"
link="$bindir/synx"

# A directory is replaced or removed only when it holds a Synx installation.
is_install() { [ -x "$1/bin/synx" ] && [ -d "$1/std" ] && [ -f "$1/install.sh" ]; }

if [ "$uninstall" -eq 1 ]; then
  if [ -L "$link" ] && [ "$(readlink -- "$link")" = "$prefix/bin/synx" ]; then rm -f -- "$link"; fi
  if is_install "$prefix"; then
    cd /
    rm -rf -- "$prefix"
    echo "Removed Synx from $prefix."
  else
    rmdir -- "$prefix" 2>/dev/null || true
    echo "install.sh: $prefix does not hold a Synx installation." >&2
  fi
  exit 0
fi

is_install "$source_dir" || { echo "install.sh: $source_dir is not a complete Synx package." >&2; exit 1; }
[ "$source_dir" != "$prefix" ] || { echo "install.sh: the package is already at $prefix." >&2; exit 1; }
if [ -n "$(ls -A -- "$prefix")" ]; then
  is_install "$prefix" || { echo "install.sh: $prefix is not empty and does not hold a Synx installation." >&2; exit 1; }
  rm -rf -- "$prefix"
  mkdir -p -- "$prefix"
fi
for item in bin lib std examples install.sh LICENSE-MIT LICENSE-APACHE; do
  cp -R -- "$source_dir/$item" "$prefix/"
done
if [ -d "$source_dir/libexec" ]; then cp -R -- "$source_dir/libexec" "$prefix/"; fi
mkdir -p -- "$bindir"
if [ -e "$link" ] && [ ! -L "$link" ]; then
  echo "install.sh: $link exists and is not a link; leaving it unchanged." >&2
else
  ln -sfn -- "$prefix/bin/synx" "$link"
fi

case ":${PATH}:" in
  *":$bindir:"*) on_path=1 ;;
  *) on_path=0 ;;
esac
if [ "$on_path" -eq 0 ] && [ "$update_path" -eq 1 ]; then
  profile="${HOME}/.profile"
  line="export PATH=\"$bindir:\$PATH\""
  if ! grep -qxF -- "$line" "$profile" 2>/dev/null; then
    printf '\n# Added by the Synx installer\n%s\n' "$line" >> "$profile"
  fi
  echo "Installed Synx to $prefix. Added $bindir to PATH in $profile; open a new shell, then run: synx version"
elif [ "$on_path" -eq 0 ]; then
  echo "Installed Synx to $prefix. Run: $link version"
else
  echo "Installed Synx to $prefix. Run: synx version"
fi
