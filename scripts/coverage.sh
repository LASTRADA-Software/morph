#!/usr/bin/env bash
# Usage: bash scripts/coverage.sh
# Run from repo root after: cmake --build --preset clang-coverage && ctest --preset clang-coverage
set -euo pipefail

# Match the toolchain to the Clang the coverage build used. CI exports
# CLANG_VERSION (e.g. 22) so we pick llvm-profdata-22 / llvm-cov-22; locally,
# with CLANG_VERSION unset, fall back to the unversioned tools on PATH.
SUFFIX="${CLANG_VERSION:+-${CLANG_VERSION}}"
LLVM_PROFDATA="llvm-profdata${SUFFIX}"
LLVM_COV="llvm-cov${SUFFIX}"

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

${LLVM_PROFDATA} merge -sparse $PROFILES -o "$MERGED"

mkdir -p "$REPORT_DIR"
${LLVM_COV} show "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -format=html \
    -output-dir="$REPORT_DIR" \
    "$SOURCES"

echo "Coverage report: $REPORT_DIR/index.html"

${LLVM_COV} report "$TEST_EXE" \
    -instr-profile="$MERGED" \
    "$SOURCES"

${LLVM_COV} export "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -format=lcov \
    "$SOURCES" \
    > "$OUT/coverage.lcov.raw"

# llvm-cov emits branch (BRDA) records once per template instantiation, so a
# branch that is covered in aggregate is still scored "partial" by Codecov for
# every instantiation that did not take one arm — dozens of spurious partials on
# header-only templated code. Collapse them to one record per source branch,
# matching the aggregate that `llvm-cov report` already prints above. Branch
# coverage is preserved (not skipped); only the per-instantiation noise is removed.
${LLVM_COV} export "$TEST_EXE" \
    -instr-profile="$MERGED" \
    "$SOURCES" \
    > "$OUT/coverage.json"

python3 scripts/aggregate_lcov_branches.py \
    "$OUT/coverage.lcov.raw" "$OUT/coverage.json" "$OUT/coverage.lcov"
