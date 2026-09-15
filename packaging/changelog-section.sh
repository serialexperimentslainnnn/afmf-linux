#!/bin/bash
# Prints the CHANGELOG.md section of one version (release notes for the GitHub release).
set -Eeuo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 <version>" >&2; exit 2; }
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
awk -v ver="$1" '
  /^## \[/ { inside = index($0, "## [" ver "]") == 1; next }
  inside && !/^\[/ { print }
' "${SCRIPT_DIR}/../CHANGELOG.md" | sed -e '1{/^$/d}' -e '${/^$/d}'
