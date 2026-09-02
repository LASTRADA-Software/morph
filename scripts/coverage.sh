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
MERGED="$OUT/merged.profdata"
REPORT_DIR="$OUT/html"
MANIFEST="$OUT/coverage_objects.txt"

# -- The binaries llvm-cov maps the profile data through ---------------------
#
# llvm-cov resolves a .profraw's counters through a *binary*'s coverage
# mapping: one binary positionally, every additional one via -object. A test
# executable that is instrumented, runs, and writes profile data therefore
# still contributes nothing unless it appears here.
#
# This list used to be written out by hand, and it was wrong in the way a
# hand-written list is always eventually wrong. It named three binary families
# while the tree built nine; morph_net_tests wrote profile data that
# llvm-profdata merged and llvm-cov then dropped, so include/morph/net -- 955
# lines, 42 of the library's 148 throw sites, eight test files driving it --
# contributed zero files to the uploaded report, and both
# sqlite_offline_queue.hpp's 57.04% and include/morph/qt's 13 lines were
# measured with their own suites absent (morph#403). It was the third time:
# morph#141 (rungs 2-4 never added) and morph#179 (the rung list had drifted
# past ledger and lims) were the same defect in the same file, and both were
# fixed by deleting the copy and reading an authoritative list instead.
#
# So the list is no longer here. cmake/compiler_options.cmake's
# apply_coverage() registers every instrumented test binary as it instruments
# it, and writes the resolved paths to coverage_objects.txt at generate time.
# A binary cannot be missing from that file without also being missing from the
# build.
#
# The manifest is validated, and cross-checked against the binaries ctest
# actually runs, by a gate of its own -- so that "a suite was added and never
# instrumented" fails the coverage leg instead of quietly shrinking the
# denominator. It is a separate script because a gate nobody tests reports
# green whether or not it still detects anything, and
# scripts/test_check_coverage_objects.sh can exercise it without a build.
#
# Checked before the gate runs, cheap first: without profile data there is
# nothing to report either way, and the gate's own ctest query is the slowest
# thing in this script. Running it only to die twenty lines later on an empty
# profraw set is the wrong order for anyone driving this by hand.
PROFILES=$(find "$OUT" -name "*.profraw" 2>/dev/null | tr '\n' ' ')
if [ -z "$PROFILES" ]; then
    echo "ERROR: No .profraw files found in $OUT." >&2
    echo "Did you set LLVM_PROFILE_FILE and run ctest --preset clang-coverage?" >&2
    exit 1
fi

bash "$(dirname "${BASH_SOURCE[0]}")/check_coverage_objects.sh" "$OUT"

# Re-read rather than re-derived: the gate above has already rejected a missing
# manifest, an empty one, and any entry with no executable behind it, so this
# loop is a read of something already known good.
COVERAGE_OBJECTS=()
while IFS= read -r _object; do
    [ -n "$_object" ] || continue
    COVERAGE_OBJECTS+=("$_object")
done < "$MANIFEST"

# One binary goes positionally and the rest via -object. Which one is first is
# arbitrary -- llvm-cov treats them alike -- so it is simply the first line.
PRIMARY_OBJECT="${COVERAGE_OBJECTS[0]}"
OBJECT_ARGS=()
for _object in "${COVERAGE_OBJECTS[@]:1}"; do
    OBJECT_ARGS+=(-object "$_object")
done

# "Was this test binary built?", answered by looking it up in the manifest
# rather than by rebuilding the path CMake would have used. Both of the
# questions below -- was the ladder built, was this rung built -- used to be
# `[ -x "$OUT/examples/<...>/<name>" ]`, which is the same guessing at CMake's
# output layout that the -object list has just stopped doing. Leaving it here
# would have kept half the mechanism derived and half hand-composed: an output
# name or directory change would go on supplying objects while silently
# dropping that rung's *sources*, which is the shrinking figure again, only
# harder to see because the other half now looks safe. The manifest already
# holds the exact resolved paths, so a basename lookup in it is the whole test.
coverage_object_built() {
    local _name="$1" _candidate
    for _candidate in "${COVERAGE_OBJECTS[@]}"; do
        if [ "${_candidate##*/}" = "$_name" ]; then
            return 0
        fi
    done
    return 1
}

# Per-rung *source paths*, named on exactly the same "only if it was built"
# terms. Each rung's models are what examples/IMPLEMENTATION.md rule 5's
# 100% bar actually names, so a rung that ships models must have its sources
# named here or the gate below measures nothing. (The rung's test *binary*
# reaches llvm-cov through coverage_objects.txt above; this loop decides only
# which source trees the report is computed over.)
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
# The rung names are read from examples/rungs.txt, the ladder's single
# authoritative list, rather than restated here. This used to be a hand-copy
# of the list examples/CMakeLists.txt used to hold inline, and it had drifted:
# ledger (rung 5) and lims (rung 6) were both missing, so neither contributed
# a single line to any uploaded report -- and codecov.yml's `ledger`
# component, added with a carefully measured 87% target, was scoring a set of
# files the report did not contain. A component that matches nothing does not
# fail; it silently reports nothing, which is the same shape of defect this
# list's own comment above warns about, and the same one that left CI's ladder
# path filter a rung behind (morph#179). Reading the list removes the copy.
#
# A rung that was not configured in this build contributes nothing, exactly as
# before, via the coverage_object_built() guard.
#
# Substituted before the loop, not piped into it: under `set -e` a failing
# reader inside a process substitution would not abort this script, and a
# coverage run over an empty rung list is exactly the silently-shrinking
# figure described above.
_MORPH_LADDER_RUNGS="$(bash "$(dirname "${BASH_SOURCE[0]}")/ladder_rungs.sh" list)"
RUNG_TEST_EXES=()
while IFS= read -r _rung; do
    [ -n "$_rung" ] || continue
    if coverage_object_built "ladder_${_rung}_tests"; then
        RUNG_TEST_EXES+=("$_rung")
    fi
done <<< "$_MORPH_LADDER_RUNGS"

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
# examples/common only when this configure also built the ladder
# (MORPH_BUILD_LADDER=ON -- see the "coverage leg only" Qt install step in
# ci.yml), so this script still works unchanged for a plain
# `cmake --preset clang-coverage` with no -DMORPH_BUILD_LADDER=ON.
if coverage_object_built ladder_common_tests; then
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

${LLVM_PROFDATA} merge -sparse $PROFILES -o "$MERGED"

mkdir -p "$REPORT_DIR"
${LLVM_COV} show "$PRIMARY_OBJECT" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    -format=html \
    -output-dir="$REPORT_DIR" \
    "${SOURCES[@]}"

echo "Coverage report: $REPORT_DIR/index.html"

${LLVM_COV} report "$PRIMARY_OBJECT" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    "${SOURCES[@]}"

${LLVM_COV} export "$PRIMARY_OBJECT" \
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
${LLVM_COV} export "$PRIMARY_OBJECT" \
    "${OBJECT_ARGS[@]}" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="$IGNORE_REGEX" \
    "${SOURCES[@]}" \
    > "$OUT/coverage.json"

python3 scripts/aggregate_lcov_branches.py \
    "$OUT/coverage.lcov.raw" "$OUT/coverage.json" "$OUT/coverage.lcov"
