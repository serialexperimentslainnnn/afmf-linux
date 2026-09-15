#!/bin/bash
# (Direct interpreter path on purpose: some security tooling flags `env bash` shebangs.)
# Installs the afmf-linux Vulkan layer from the release tarball into the user's home, without
# root: the library under ~/.local/lib/afmf-linux and the manifest, with an absolute
# library_path, under ~/.local/share/vulkan/implicit_layer.d, where the Vulkan loader looks.
#   ./install.sh              install or update
#   ./install.sh --uninstall  remove both files
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
LIB_DIR="$HOME/.local/lib/afmf-linux"
MANIFEST_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"
readonly SCRIPT_DIR LIB_DIR MANIFEST_DIR

if [[ ${1:-} == --uninstall ]]; then
  rm -f -- "$LIB_DIR/libafmf-linux.so" "$MANIFEST_DIR/afmf-linux.json"
  rmdir -- "$LIB_DIR" 2>/dev/null || true
  echo "afmf-linux removed from $LIB_DIR and $MANIFEST_DIR"
  exit 0
fi

[[ -f $SCRIPT_DIR/libafmf-linux.so && -f $SCRIPT_DIR/afmf-linux.json ]] ||
  { echo "run this from the unpacked release directory" >&2; exit 1; }
mkdir -p -- "$MANIFEST_DIR" "$LIB_DIR"
install -m 0644 -- "$SCRIPT_DIR/libafmf-linux.so" "$LIB_DIR/libafmf-linux.so"
# The tarball's manifest names the library by file name; the loader wants a path it can open.
sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$LIB_DIR/libafmf-linux.so\"|" \
  "$SCRIPT_DIR/afmf-linux.json" >"$MANIFEST_DIR/afmf-linux.json"
echo "afmf-linux installed: $LIB_DIR/libafmf-linux.so, $MANIFEST_DIR/afmf-linux.json"
echo "enable it per game with AFMF_ENABLE=1 (Steam: AFMF_ENABLE=1 %command%)"
