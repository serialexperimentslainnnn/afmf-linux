#!/usr/bin/env bash
# Smoke test for VK_LAYER_AFMF_frame_generation against a real window: runs vkcube under the layer
# from a build tree, checks it loads, sees a swapchain through to teardown, and that DISABLE_AFMF=1
# turns it off. Khronos validation is not enabled here because vkcube itself is not validation-clean
# (VUID-vkAcquireNextImageKHR-surface-07783); the validated, sanitizer-friendly test is `ctest`.
set -Eeuo pipefail
shopt -s inherit_errexit
IFS=$'\n\t'
SCRIPT_NAME="${0##*/}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_NAME SCRIPT_DIR
: "${BUILD_DIR:=${SCRIPT_DIR}/../build}"
: "${FRAMES:=300}"
WORKDIR=''

log() { printf '%(%FT%TZ)T [%s] %s: %s\n' -1 "$1" "$SCRIPT_NAME" "${*:2}" >&2; }
die() { log error "$*"; exit 1; }
cleanup() { local rc=$?; [[ -d "$WORKDIR" ]] && rm -rf -- "$WORKDIR"; exit "$rc"; }
trap cleanup EXIT
trap 'log error "failure in ${BASH_SOURCE[0]}:${LINENO} -> ${BASH_COMMAND}"' ERR

usage() {
  cat <<'EOF'
Usage: tests/smoke.sh [-h]

Environment:
  BUILD_DIR  CMake build tree holding layer/VkLayer_AFMF.json (default: ../build)
  FRAMES     Frames vkcube renders per run (default: 300)

Exit codes: 0 all checks passed, 1 a check failed, 2 usage error.
Example:    BUILD_DIR=build tests/smoke.sh
EOF
}

# run_vkcube <logfile> <frames> [VAR=value ...]  — the assignments are exported only inside the subshell.
run_vkcube() {
  local out=$1 frames=$2
  shift 2
  (
    local kv
    for kv in "$@"; do export "${kv?}"; done
    # Implicit layers are searched separately from explicit ones: VK_ADD_LAYER_PATH would not do.
    export VK_ADD_IMPLICIT_LAYER_PATH="${BUILD_DIR}/layer"
    vkcube --c "$frames" --suppress_popups
  ) >"$out" 2>&1 || { tail -n 20 -- "$out" >&2; die "vkcube exited with an error (last 20 lines above)"; }
}

main() {
  [[ $# -eq 0 ]] || { [[ $1 == -h ]] && { usage; exit 0; }; usage >&2; exit 2; }
  command -v vkcube >/dev/null || die "vkcube missing (vulkan-tools)"
  [[ -f "${BUILD_DIR}/layer/VkLayer_AFMF.json" ]] || die "no layer manifest in ${BUILD_DIR}/layer"
  WORKDIR="$(mktemp -d)"

  log info "layer enabled"
  run_vkcube "$WORKDIR/on.log" "$FRAMES" AFMF_ENABLE=1 AFMF_LOG=3
  grep -q '\[AFMF info\] layer active' "$WORKDIR/on.log" || die "layer did not load"
  grep -q '\[AFMF info\] swapchain .* created' "$WORKDIR/on.log" || die "no swapchain went through the layer"
  grep -q '\[AFMF info\] swapchain .* destroyed after' "$WORKDIR/on.log" || die "swapchain teardown not seen"

  # A few frames suffice to prove the layer stayed out; an unfocused vkcube window is throttled by
  # the compositor, so a long run here only makes the test look hung.
  log info "layer disabled (negative control)"
  run_vkcube "$WORKDIR/off.log" 30 AFMF_ENABLE=1 DISABLE_AFMF=1 AFMF_LOG=3
  if grep -q '\[AFMF' "$WORKDIR/off.log"; then die "DISABLE_AFMF=1 did not disable the layer"; fi

  log info "all checks passed"
}
main "$@"
