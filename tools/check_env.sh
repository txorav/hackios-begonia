#!/usr/bin/env bash
#
# check_env.sh — thin wrapper around check_env.py (same directory).
#
# Runs the PASS/FAIL environment matrix for HackiOS-begonia and reproduces
# the exit code of check_env.py:
#     0 → all CRITICAL tools present
#     1 → at least one CRITICAL tool missing (see the FAIL yes rows)
#
# The table itself is printed by check_env.py; this wrapper only adds a
# final verdict line and pass-through flags.
#
# Options:
#   -h           this help
#   -q           quiet: no table, exit code only (still prints verdict)
#   -j           also write JSON (to -p PATH, default check_env_results.json)
#   -o LIST      only check these tools (comma-separated), e.g. -o clang,dtc
#   -p PATH      where the JSON goes when -j is given
#
# Examples:
#   ./check_env.sh                      # full matrix
#   ./check_env.sh -o clang,dtc -j      # subset + JSON dump
#   ./check_env.sh -q                   # CI: verdict/exit code only
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROG="$(basename "${BASH_SOURCE[0]}")"
PY="$SCRIPT_DIR/check_env.py"

usage() { sed -n '5,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

[[ -f "$PY" ]] || { echo "[check_env.sh] ERROR: $PY missing" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || {
  echo "[check_env.sh] ERROR: python3 not found — install it first" >&2; exit 2; }

ARGS=""
JSON=0
JSON_PATH="check_env_results.json"
while getopts ":hqjo:p:" opt; do
  case "$opt" in
    h) usage ;;
    q) ARGS="$ARGS --quiet" ;;
    j) JSON=1 ; ARGS="$ARGS --json" ;;
    o) ARGS="$ARGS --only $OPTARG" ;;
    p) JSON_PATH="$OPTARG" ; JSON=1 ;;
    :) echo "[$PROG] ERROR: -$OPTARG needs an argument" >&2; exit 1 ;;
    \?) echo "[$PROG] ERROR: unknown option: -$OPTARG (see -h)" >&2
        exit 1 ;;
  esac
done

if (( JSON )); then
  ARGS="$ARGS --json-out $JSON_PATH"
fi

set +e
python3 "$PY" $ARGS
RC=$?
set -e

if (( RC == 0 )); then
  printf '[check_env.sh] RESULT: PASS — host environment ready\n'
else
  printf '[check_env.sh] RESULT: FAIL (rc=%s) — install missing CRITICAL tools\n' "$RC"
  printf '[check_env.sh]   1) tools/install_hosttoolchain.sh   (apt side: dtc, python3, git, mkfs.*)\n'
  printf '[check_env.sh]   2) tools/install_llvm_toolchain.sh    (clang + cctools ld64 + ldid)\n'
fi

(( JSON )) && [[ -f "$JSON_PATH" ]] &&
  printf '[check_env.sh] json results: %s\n' "$JSON_PATH"
exit "$RC"
