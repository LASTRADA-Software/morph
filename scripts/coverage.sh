#!/usr/bin/env bash
# Usage: bash scripts/coverage.sh
# Run from repo root after: cmake --build --preset clang-coverage && ctest --preset clang-coverage
set -euo pipefail

OUT="build/clang-coverage"
TEST_EXE="$OUT/tests/morph_tests"
MERGED="$OUT/merged.profdata"
REPORT_DIR="$OUT/html"

# Restrict coverage to the library headers. Test files, demo src/, system
# headers and fetched dependencies are excluded by passing this as the
# positional source filter to llvm-cov.
SOURCES="include/morph"

PROFILES=$(find "$OUT" -name "*.profraw" 2>/dev/null | tr '\n' ' ')
if [ -z "$PROFILES" ]; then
    echo "ERROR: No .profraw files found in $OUT." >&2
    echo "Did you set LLVM_PROFILE_FILE and run ctest --preset clang-coverage?" >&2
    exit 1
fi

llvm-profdata-20 merge -sparse $PROFILES -o "$MERGED"

mkdir -p "$REPORT_DIR"
llvm-cov-20 show "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -format=html \
    -output-dir="$REPORT_DIR" \
    "$SOURCES"

echo "Coverage report: $REPORT_DIR/index.html"

llvm-cov-20 report "$TEST_EXE" \
    -instr-profile="$MERGED" \
    "$SOURCES"

# --skip-branches: llvm-cov's lcov branch export is emitted per template
# instantiation, so a branch that is fully covered in aggregate is still marked
# "partial" on Codecov whenever any single instantiation leaves one arm untaken
# (pervasive for header-only templated code like completion.hpp / bridge.hpp).
# That noise buries the meaningful signal, so we upload line coverage only.
# Aggregate branch coverage remains visible in the `llvm-cov report` output above
# and in the HTML report.
llvm-cov-20 export "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -format=lcov \
    --skip-branches \
    "$SOURCES" \
    > "$OUT/coverage.lcov"
