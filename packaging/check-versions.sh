#!/bin/bash
# Fails when the version in CMakeLists.txt, the RPM spec, the Debian changelog, the PKGBUILD and
# CHANGELOG.md disagree, or (with an argument) when they differ from the tag being released.
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
readonly SCRIPT_DIR REPO_DIR

cmake_ver="$(sed -n 's/^project(afmf-linux VERSION \([0-9.]*\).*/\1/p' "${REPO_DIR}/CMakeLists.txt")"
spec_ver="$(sed -n 's/^Version: *\([0-9.]*\).*/\1/p' "${SCRIPT_DIR}/rpm/afmf-linux.spec")"
deb_ver="$(sed -n '1s/^afmf-linux (\([0-9.]*\)-[0-9]*).*/\1/p' "${SCRIPT_DIR}/debian/changelog")"
arch_ver="$(sed -n 's/^pkgver=\([0-9.]*\).*/\1/p' "${SCRIPT_DIR}/arch/PKGBUILD")"
log_ver="$(sed -n 's/^## \[\([0-9.]*\)\].*/\1/p' "${REPO_DIR}/CHANGELOG.md" | head -n1)"

status=0
for pair in "rpm spec:${spec_ver}" "debian changelog:${deb_ver}" "PKGBUILD:${arch_ver}" "CHANGELOG.md:${log_ver}"; do
  if [[ ${pair#*:} != "$cmake_ver" ]]; then
    echo "version mismatch: ${pair%%:*} says '${pair#*:}', CMakeLists.txt says '${cmake_ver}'" >&2
    status=1
  fi
done
if [[ $# -eq 1 && $1 != "$cmake_ver" ]]; then
  echo "tag says '$1', CMakeLists.txt says '${cmake_ver}'" >&2
  status=1
fi
[[ $status -eq 0 ]] && echo "versions agree: ${cmake_ver}"
exit "$status"
