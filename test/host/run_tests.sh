#!/usr/bin/env bash
# Host-side unit tests for the pure CAT frame decoders (no ESP-IDF, no hardware).
# Run from anywhere: test/host/run_tests.sh
set -euo pipefail

# Repo root = two levels up from this script.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CAT_INC="$ROOT/components/cat_parser/include"
ANT_INC="$ROOT/components/antenna_switch/include"
OUT="$(mktemp -d)"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -Wall -Wextra -Wpedantic"

rc=0

# Pure CAT frame decoders (share the cat_parser include dir).
for t in kenwood yaesu; do
    src="$ROOT/test/host/test_${t}_frames.cpp"
    bin="$OUT/test_${t}_frames"
    echo "== building $t =="
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS -I "$CAT_INC" "$src" -o "$bin"
    "$bin" || rc=1
done

# Pure PTT antenna-swap decision logic (antenna_switch include dir).
echo "== building antenna_swap_logic =="
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -I "$ANT_INC" "$ROOT/test/host/test_antenna_swap_logic.cpp" -o "$OUT/test_antenna_swap_logic"
"$OUT/test_antenna_swap_logic" || rc=1

rm -rf "$OUT"
exit "$rc"
