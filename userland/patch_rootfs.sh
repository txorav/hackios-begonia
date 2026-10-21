#!/usr/bin/env bash
#
# patch_rootfs.sh — deprecated alias.
#
# The Phase-5 rootfs patcher is now apply_rootfs_patches.sh (discoverable
# under the same directory). This wrapper keeps older operator muscle memory
# and documentation links working; it performs no logic of its own except
# advisory banner + argument passthrough.
#
# Removal note: delete this file when repo tooling docs stop referring to the
# old name (grep docs/ + root README when that cleanup happens).

set -euo pipefail

# shellcheck source=/dev/null
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REAL="$HERE/apply_rootfs_patches.sh"

cat <<EOF
==============================================================================
 [DEPRECATION] ${0##*/} is the OLD name of the rootfs patcher.
   → invoking ${REAL}
 All arguments are passed through unchanged.
==============================================================================
EOF

if [[ ! -x "$REAL" ]]; then
  printf 'ERROR: %s missing or not executable (repo tree incomplete?)\n' "$REAL" >&2
  exit 2
fi

exec "$REAL" "$@"
