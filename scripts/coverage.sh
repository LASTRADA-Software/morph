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

# Second binary, only present when this configure also built the ladder
# (MORPH_BUILD_LADDER=ON — see the "coverage leg only" Qt install step in
# ci.yml). llvm-cov takes one binary positionally and every additional one
# via -object; OBJECT_ARGS stays empty (and every ${OBJECT_ARGS[@]}
# expansion below a no-op) when the ladder wasn't built, so this script
# still works unchanged for a plain `cmake --preset clang-coverage` with no
# -DMORPH_BUILD_LADDER=ON.
LADDER_TEST_EXE="$OUT/examples/common/ladder_common_tests"
OBJECT_ARGS=()
if [ -x "$LADDER_TEST_EXE" ]; then
    OBJECT_ARGS+=(-object "$LADDER_TEST_EXE")
fi

# Per-rung test binaries, added on exactly the same "only if it was built"
# terms. Each rung's models are what examples/IMPLEMENTATION.md rule 5's
# 100% bar actually names, so a rung that ships models must contribute its
# profile data or the gate below measures nothing.
#
# Driven by a loop rather than one hand-written block per rung, because the
# hand-written form silently rotted: rungs 2, 3 and 4 shipped without ever
# being added here, leaving ~15k lines of models, presenters and QML adapters
# outside the coverage number entirely while the percentage still looked
# healthy (morph#141). Nothing fails when a rung is forgotten -- the script
# runs, the report uploads, and the figure is simply computed over a
# shrinking fraction of the ladder. A loop over the known rungs cannot be
# forgotten in the same way; a new rung needs its name added here and
# nowhere else.
#
# `_MORPH_LADDER_RUNGS` deliberately mirrors examples/CMakeLists.txt's own
# `_morph_known_rungs` list. A rung that was not configured in this build
# contributes nothing, exactly as before, via the `-x` guard.
# Drift is exactly what this list is for, and it had drifted: ledger (rung 5)
# and lims (rung 6) were both missing, so neither contributed a single line to
# any uploaded report -- and codecov.yml's `ledger` component, added with a
# carefully measured 87% target, was scoring a set of files the report did not
# contain. A component that matches nothing does not fail; it silently reports
# nothing, which is the same shape of defect this list's own comment above
# warns about. Added here at rung 6's close; see docs/findings/015.
_MORPH_LADDER_RUNGS=(pastebin bookmarks polls kanban ledger lims)
RUNG_TEST_EXES=()
for _rung in "${_MORPH_LADDER_RUNGS[@]}"; do
    _exe="$OUT/examples/${_rung}/ladder_${_rung}_tests"
    if [ -x "$_exe" ]; then
        OBJECT_ARGS+=(-object "$_exe")
        RUNG_TEST_EXES+=("$_rung")
    fi
done

# Positional source-path filters to llvm-cov: include/morph is the library
# proper; examples/common is the ladder's hand-written GUI/testkit code
# (examples/IMPLEMENTATION.md rule 5 — presenter/BackendRig/etc. logic is
# real coverage of morph's own client stack, per examples/TESTING.md's
# "round-7 T4 reframe"). examples/pastebin (rung 1) adds the first real rung
# models — the sole subject of rule 5's own 100% bar — plus its hand-written
# presenter/QML-adapter layer, held to the same bar as examples/common's for
# the same reason. AUTOMOC's generated
# mocs_compilation.cpp lives under $OUT (the build tree), never under a
# source-tree path named here, so moc output is excluded automatically —
# no separate exclusion mechanism needed. Demo src/, system headers and
# fetched dependencies are excluded the same way.
SOURCES=(include/morph)
if [ -x "$LADDER_TEST_EXE" ]; then
    SOURCES+=(examples/common)
fi
# include/ + src/ are each rung's DTOs and models (rule 5's own 100% bar);
# gui_lib/ is its hand-written presenter/adapter code, held to the same bar
# for the same reason examples/common/gui is — it is real coverage of
# morph's own client stack, not app-specific domain logic. gui/ and
# gui_wasm/ are deliberately absent: those are `main()` shells (engine
# setup, argv parsing, setInitialProperties) with no unit-testable seam,
# exercised only by the offscreen QML smoke test and by hand.
#
# Only rungs whose test binary was actually built (RUNG_TEST_EXES, above)
# contribute sources, so a partial `-DMORPH_LADDER_RUNGS=` configure still
# produces a coherent report rather than naming paths with no profile data.
for _rung in "${RUNG_TEST_EXES[@]}"; do
    for _sub in include src gui_lib; do
        if [ -d "examples/${_rung}/${_sub}" ]; then
            SOURCES+=("examples/${_rung}/${_sub}")
        fi
    done
done

# examples/common/testkit/ mixes real, reusable test-support headers/.cpp
# (backend_rig.hpp, db_fixture.hpp, strand_interleaver.hpp, ...) with actual
# Catch2 test files (test_event_poller.cpp, test_presenter.cpp, ...) in the
# same directory — unlike include/morph and examples/pastebin's SOURCES
# entries above, which contain no test files at all. `SOURCES+=(examples/
# common)` swept both in indiscriminately: a TEST_CASE body's own untaken
# assertion/lambda branches (a REQUIRE's fail arm, a "must not run" callback
# proving itself unreachable) were being measured as if they were product
# code, manufacturing the exact "phantom uncovered branch" noise this
# project has repeatedly had to hand-verify file by file. Test files
# genuinely are not part of what examples/IMPLEMENTATION.md rule 5's 100%
# bar means to hold to that standard — only the real testkit/GUI code is.
IGNORE_REGEX='.*/testkit/test_[^/]+\.cpp$'

PROFILES=$(find "$OUT" -name "*.profraw" 2>/dev/null | tr '\n' ' ')
if [ -z "$PROFILES" ]; then
    echo "ERROR: No .profraw files found in $OUT." >&2
    echo "Did you set LLVM_PROFILE_FILE and run ctest --preset clang-coverage?" >&2
    exit 1
fi

${LLVM_PROFDATA} merge -sparse $PROFILES -o "$MERGED"

mkdir -p "$REPORT_DIR"
${LLVM_COV} show "$TEST_EXE" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    -format=html \
    -output-dir="$REPORT_DIR" \
    "${SOURCES[@]}"

echo "Coverage report: $REPORT_DIR/index.html"

${LLVM_COV} report "$TEST_EXE" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    "${SOURCES[@]}"

${LLVM_COV} export "$TEST_EXE" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    -format=lcov \
    "${SOURCES[@]}" \
    > "$OUT/coverage.lcov.raw"

# llvm-cov emits branch (BRDA) records once per template instantiation, so a
# branch that is covered in aggregate is still scored "partial" by Codecov for
# every instantiation that did not take one arm — dozens of spurious partials on
# header-only templated code. Collapse them to one record per source branch,
# matching the aggregate that `llvm-cov report` already prints above. Branch
# coverage is preserved (not skipped); only the per-instantiation noise is removed.
${LLVM_COV} export "$TEST_EXE" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    "${SOURCES[@]}" \
    > "$OUT/coverage.json"

python3 scripts/aggregate_lcov_branches.py \
    "$OUT/coverage.lcov.raw" "$OUT/coverage.json" "$OUT/coverage.lcov"
