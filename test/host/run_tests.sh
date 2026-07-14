#!/usr/bin/env bash
# Host-side unit tests for the pure CAT frame decoders (no ESP-IDF, no hardware).
# Run from anywhere: test/host/run_tests.sh
set -euo pipefail

# Repo root = two levels up from this script.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INC="$ROOT/components/cat_parser/include"
OUT="$(mktemp -d)"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -Wall -Wextra -Wpedantic"

rc=0
for t in kenwood yaesu; do
    src="$ROOT/test/host/test_${t}_frames.cpp"
    bin="$OUT/test_${t}_frames"
    echo "== building $t =="
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS -I "$INC" "$src" -o "$bin"
    "$bin" || rc=1
done

rm -rf "$OUT"
exit "$rc"
