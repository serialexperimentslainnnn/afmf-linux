#!/bin/bash
# (Direct interpreter path on purpose: some security tooling flags `env bash` shebangs.)
# Builds one distribution package of afmf-linux from the checked-out sources into packaging/out.
# Meant for CI containers (as root it installs the build dependencies with the distribution's
# package manager) but runs on a matching host too, where the dependencies are yours to install.
#   packaging/build-package.sh tarball | rpm | deb | arch
set -Eeuo pipefail
shopt -s inherit_errexit
IFS=$'\n\t'
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
OUT_DIR="${SCRIPT_DIR}/out"
readonly SCRIPT_DIR REPO_DIR OUT_DIR

log() { printf '[%s] %s\n' "$1" "${*:2}" >&2; }
die() { log error "$*"; exit 1; }

version() {
  sed -n 's/^project(afmf-linux VERSION \([0-9.]*\).*/\1/p' "${REPO_DIR}/CMakeLists.txt"
}

# A source tarball named the way every packager expects: afmf-linux-<version>/ inside. From git
# when there is a repository; from the working tree when actions/checkout fetched a tarball
# because the container had no git (then there is no .git and nothing to exclude but our output).
source_tarball() {
  local ver=$1 out=$2
  if [[ -d ${REPO_DIR}/.git ]] && command -v git >/dev/null; then
    git -C "$REPO_DIR" config --global --add safe.directory "$REPO_DIR" 2>/dev/null || true
    git -C "$REPO_DIR" archive --format=tar.gz --prefix="afmf-linux-${ver}/" -o "$out" HEAD
  else
    tar -C "$REPO_DIR" --exclude=./packaging/out --exclude='./build*' --exclude='./cmake-build-*' \
      --exclude=./.claudetools --exclude=./.idea --transform "s,^\./,afmf-linux-${ver}/," \
      -czf "$out" .
  fi
}

build_tarball() {
  local ver=$1
  local stage="${OUT_DIR}/afmf-linux-${ver}"
  [[ -f ${REPO_DIR}/build/layer/libafmf-linux.so ]] || die "build the layer first (cmake --build build)"
  rm -rf -- "$stage"
  mkdir -p -- "$stage"
  cp -- "${REPO_DIR}/build/layer/libafmf-linux.so" "$stage/"
  # The manifest names the library by file name; install.sh rewrites it to an absolute path.
  sed 's|"library_path": *"[^"]*"|"library_path": "libafmf-linux.so"|' \
    "${REPO_DIR}/build/layer/afmf-linux.json" >"$stage/afmf-linux.json"
  cp -- "${SCRIPT_DIR}/install.sh" "${REPO_DIR}/LICENSE" "${REPO_DIR}/README.md" "${REPO_DIR}/CHANGELOG.md" "$stage/"
  cp -- "${REPO_DIR}/shaders/fidelityfx/LICENSE.txt" "$stage/LICENSE.fidelityfx.txt"
  cp -- "${REPO_DIR}/shaders/fidelityfx/NOTICE.md" "$stage/NOTICE.fidelityfx.md"
  cp -- "${SCRIPT_DIR}/afmf-linux-release-key.asc" "$stage/"
  tar -C "$OUT_DIR" -cJf "${OUT_DIR}/afmf-linux-${ver}-x86_64.tar.xz" "afmf-linux-${ver}"
  rm -rf -- "$stage"
  log info "wrote ${OUT_DIR}/afmf-linux-${ver}-x86_64.tar.xz"
}

build_rpm() {
  local ver=$1 top="${OUT_DIR}/rpmbuild"
  if command -v dnf >/dev/null && [[ $(id -u) -eq 0 ]]; then
    dnf install -y --setopt=install_weak_deps=False rpm-build cmake ninja-build gcc \
      vulkan-headers vulkan-loader-devel glslang spirv-tools git tar gzip
  fi
  mkdir -p -- "$top"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}
  source_tarball "$ver" "$top/SOURCES/afmf-linux-${ver}.tar.gz"
  cp -- "${SCRIPT_DIR}/rpm/afmf-linux.spec" "$top/SPECS/"
  rpmbuild --define "_topdir $top" -bb "$top/SPECS/afmf-linux.spec"
  cp -- "$top"/RPMS/x86_64/afmf-linux-*.rpm "$OUT_DIR/"
  rm -rf -- "$top"
  log info "wrote $(ls "$OUT_DIR"/*.rpm)"
}

build_deb() {
  local ver=$1 work="${OUT_DIR}/debbuild"
  if command -v apt-get >/dev/null && [[ $(id -u) -eq 0 ]]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends build-essential debhelper cmake ninja-build \
      libvulkan-dev glslang-tools spirv-tools git
  fi
  rm -rf -- "$work"
  mkdir -p -- "$work"
  source_tarball "$ver" "$work/src.tar.gz"
  tar -C "$work" -xzf "$work/src.tar.gz"
  cp -r -- "${SCRIPT_DIR}/debian" "$work/afmf-linux-${ver}/debian"
  (cd "$work/afmf-linux-${ver}" && dpkg-buildpackage -b -us -uc)
  # Name the .deb after the distribution it was built on: the two are not interchangeable.
  local codename
  codename="$(. /etc/os-release && printf '%s' "${VERSION_CODENAME:-${ID}}")"
  local deb
  for deb in "$work"/afmf-linux_*.deb; do
    cp -- "$deb" "$OUT_DIR/$(basename "${deb%.deb}")~${codename}.deb"
  done
  rm -rf -- "$work"
  log info "wrote $(ls "$OUT_DIR"/*.deb)"
}

build_arch() {
  local ver=$1 work="${OUT_DIR}/archbuild"
  if command -v pacman >/dev/null && [[ $(id -u) -eq 0 ]]; then
    pacman -Syu --noconfirm --needed cmake ninja gcc vulkan-headers vulkan-icd-loader glslang spirv-tools git
  fi
  rm -rf -- "$work"
  mkdir -p -- "$work"
  source_tarball "$ver" "$work/afmf-linux-${ver}.tar.gz"
  # Build from the local source tarball instead of the GitHub URL the AUR PKGBUILD points at.
  sed -e "s|^source=.*|source=(\"afmf-linux-${ver}.tar.gz\")|" -e "s|^sha256sums=.*|sha256sums=('SKIP')|" \
    "${SCRIPT_DIR}/arch/PKGBUILD" >"$work/PKGBUILD"
  # makepkg refuses to run as root; CI containers start as root.
  if [[ $(id -u) -eq 0 ]]; then
    id builder >/dev/null 2>&1 || useradd -m builder
    chown -R builder "$work"
    su builder -c "cd '$work' && makepkg -f"
  else
    (cd "$work" && makepkg -f)
  fi
  cp -- "$work"/afmf-linux-*.pkg.tar.zst "$OUT_DIR/"
  rm -rf -- "$work"
  log info "wrote $(ls "$OUT_DIR"/*.pkg.tar.zst)"
}

main() {
  [[ $# -eq 1 ]] || { echo "usage: $0 tarball|rpm|deb|arch" >&2; exit 2; }
  local ver
  ver="$(version)"
  [[ -n $ver ]] || die "could not read the version from CMakeLists.txt"
  mkdir -p -- "$OUT_DIR"
  case "$1" in
    tarball) build_tarball "$ver" ;;
    rpm) build_rpm "$ver" ;;
    deb) build_deb "$ver" ;;
    arch) build_arch "$ver" ;;
    *) echo "usage: $0 tarball|rpm|deb|arch" >&2; exit 2 ;;
  esac
}
main "$@"
