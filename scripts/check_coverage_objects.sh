#!/usr/bin/env bash
# Usage: bash scripts/check_coverage_objects.sh [BUILD_DIR] [CTEST_JSON]
#
# Fails if ctest runs a binary that llvm-cov is never handed, so that
# everything the binary executed is measured as if it had never run.
#
# Why this gate exists (morph#403). scripts/coverage.sh used to name the
# binaries it profiles by hand. It named three families while the tree built
# nine. morph_net_tests was instrumented, ran, and wrote profile data that
# llvm-profdata dutifully merged -- and then llvm-cov, which resolves counters
# through a *binary*'s coverage mapping, was never given the binary, so the
# whole of include/morph/net (955 lines, 42 of the library's 148 throw sites,
# eight test files driving it) contributed zero files to the uploaded report.
# morph_qt_tests, morph_offline_sqlite_tests and morph_net_qt_interop_tests
# were worse off still: they had no apply_coverage() call at all, so there was
# not even profile data to drop, and both sqlite_offline_queue.hpp's 57.04%
# (the library's worst file) and include/morph/qt's 13 lines were measured with
# their own suites absent.
#
# That was the third occurrence. morph#141 (rungs 2-4 shipped without ever
# being added to coverage.sh, leaving ~15k lines outside the number) and
# morph#179 (the hand-copied rung list had drifted past ledger and lims) were
# the same defect in the same file. Both were fixed by deleting the copy and
# reading an authoritative list instead, and coverage.sh's own comment named
# the failure mode exactly:
#
#     Nothing fails when a rung is forgotten -- the script runs, the report
#     uploads, and the figure is simply computed over a shrinking fraction.
#
# The list is now derived (cmake/compiler_options.cmake's apply_coverage()
# writes coverage_objects.txt as it instruments), which stops the list falling
# behind the *build*. This script stops it falling behind the *test suite*,
# which is the half that derivation cannot fix: a suite that was never
# instrumented is absent from the manifest for a perfectly consistent reason.
#
# The comparison is behavioural rather than textual. It asks ctest which
# binaries it runs and checks those, instead of grepping the CMake files for
# add_executable -- a grep passes just as happily on a target that is defined,
# instrumented, and never registered as a test, which is the same shape of
# blindness as a path filter that matches nothing.
#
# CTEST_JSON (second argument) supplies `ctest --show-only=json-v1` output from
# a file instead of running ctest, which is what scripts/test_check_coverage_objects.sh
# uses to exercise this gate without a build.
set -euo pipefail

build_dir="${1:-build/clang-coverage}"
ctest_json_file="${2:-}"
readonly build_dir ctest_json_file

readonly manifest="${build_dir}/coverage_objects.txt"

# ── Stated non-participants ──────────────────────────────────────────────────
# A binary ctest runs that is not profiled, and why. Every entry is a claim
# someone can argue with, which is the point: the alternative to naming them is
# a silence that reads identically to the defect above.
#
# Two kinds live here and they are not the same thing. The first are decisions
# -- a benchmark is not a test, a soak binary re-drives covered paths at great
# cost. The second, marked GAP, are defects that have not been fixed yet;
# listing one keeps the leg green while making the omission print on every run,
# and each says what the fix is.
#
# Prints the reason and returns 0 for a known exclusion; returns 1 for anything
# else, which is then a failure.
coverage_exclusion_reason() {
    case "$1" in
        morph_bench)
            echo "a benchmark, not a test: it measures dispatch latency against a trivial echo model and asserts nothing about correctness (MORPH_BUILD_LOAD_TESTS=ON only, which the coverage leg does not set)"
            ;;
        morph_soak)
            echo "a long-running opt-in soak binary (MORPH_BUILD_LOAD_TESTS=ON only): it re-drives, over many cycles, paths the ordinary suites already cover, so instrumenting it at -O0 would cost the leg minutes and move no number"
            ;;
        morph_journal_skew_old|morph_journal_skew_new)
            echo "two probe programs registering a model that exists nowhere else in the tree; instrumenting them would add template instantiations unique to the probe and score them against the library, and what the probe asserts is about a journal file on disk rather than about coverage"
            ;;
        fuzz_wire_decode|fuzz_dispatch_execute)
            echo "libFuzzer harnesses (MORPH_BUILD_FUZZERS=ON only, which the coverage leg does not set); apply_fuzzer() builds them at -O1 under -fsanitize=fuzzer,address, a different instrumentation from apply_coverage()'s"
            ;;
        # ── Known gaps, not justified exclusions ────────────────────────────
        # The three below are the same defect morph#403 is about, in files this
        # gate did not exist to guard: all three run in the coverage leg
        # (MORPH_BUILD_EXAMPLES defaults ON) and none reaches llvm-cov. They
        # are listed rather than silently tolerated so the leg prints the gap
        # on every run, which is the whole difference between this and the
        # three occurrences that went unnoticed. Each is a small edit in a file
        # under examples/, and each should delete its entry here when it lands.
        morph_concepts_tests)
            echo "GAP: a real Catch2 suite (22 ctest cases driving include/morph's journal, offline queue, validation, transport-limit, versioning, connection-scope, observability and shutdown paths) whose target in examples/concepts/CMakeLists.txt has no apply_coverage() call at all, so it is not instrumented and contributes nothing. Fix: an if(AF_COVERAGE) apply_coverage(morph_concepts_tests) block there; the name already ends in _tests, so nothing else is needed"
            ;;
        morph_forms_demo)
            echo "GAP: instrumented by examples/forms/CMakeLists.txt and driven by two ctest tests (forms_html_math, forms_repl_roundtrip), so it writes profile data that llvm-profdata merges and llvm-cov then drops -- morph#403's defect exactly. Fix: apply_coverage(morph_forms_demo TEST) there, TEST because a demo binary driven by tests is not named like a test"
            ;;
        morph_qt_tls_example)
            echo "GAP: run by the qt_tls_example_runs ctest test and never instrumented, so the pinned-certificate and insecure-verify paths it exercises through include/morph/qt score nothing. Fix: an if(AF_COVERAGE) apply_coverage(morph_qt_tls_example TEST) block in examples/qt_tls_client/CMakeLists.txt"
            ;;
        *)
            return 1
            ;;
    esac
}

# ── The manifest ─────────────────────────────────────────────────────────────
if [ ! -s "$manifest" ]; then
    echo "error: ${manifest} is missing or empty." >&2
    echo "It is written by cmake/compiler_options.cmake's apply_coverage() during an" >&2
    echo "AF_COVERAGE configure. Configure with 'cmake --preset clang-coverage' and build" >&2
    echo "before running this." >&2
    exit 1
fi

manifest_realpaths=""
manifest_count=0
while IFS= read -r object; do
    [ -n "$object" ] || continue
    # A manifest entry that is not on disk means the configure registered a
    # target the build did not produce. Reporting coverage over the survivors
    # would be the silently-shrinking figure this gate exists to stop.
    if [ ! -x "$object" ]; then
        echo "error: ${manifest} names '${object}', which is not an executable file." >&2
        echo "The build did not produce every target the configure registered." >&2
        exit 1
    fi
    manifest_realpaths+="$(realpath "$object")"$'\n'
    manifest_count=$((manifest_count + 1))
done < "$manifest"

# ── What ctest actually runs ─────────────────────────────────────────────────
if [ -n "$ctest_json_file" ]; then
    ctest_json="$(cat "$ctest_json_file")"
else
    ctest_json="$(ctest --test-dir "$build_dir" --show-only=json-v1)"
fi

# Every element of a test's command that resolves to an executable file inside
# the build tree -- not just command[0]. Two of this repository's tests are
# driven by a wrapper that takes the binary under test as an argument
# (`node examples/forms/test_html_math.mjs <morph_forms_demo>` and
# `examples/forms/test_repl.sh <morph_forms_demo>`), so command[0] there is
# /usr/bin/node or a shell script in the source tree and the thing whose
# coverage is at stake is an argument. Scoping to the build tree is what makes
# that safe to do bluntly: interpreters, source-tree drivers and plain
# data-file arguments all fall outside it, and the binaries this gate is about
# are all inside it by construction.
#
# Compared as realpath()s: the manifest holds generator-resolved absolute
# paths, while a test may be registered with either form.
ctest_binaries="$(printf '%s' "$ctest_json" | BUILD_DIR="$build_dir" python3 -c '
import json, os, sys

build_root = os.path.realpath(os.environ["BUILD_DIR"]) + os.sep
document = json.load(sys.stdin)
seen = []
for test in document.get("tests", []):
    for argument in test.get("command") or []:
        candidate = os.path.realpath(argument)
        if not candidate.startswith(build_root):
            continue
        if not os.path.isfile(candidate) or not os.access(candidate, os.X_OK):
            continue
        if candidate not in seen:
            seen.append(candidate)
print("\n".join(seen))
')"

unexplained=()
checked=0
while IFS= read -r binary; do
    [ -n "$binary" ] || continue
    checked=$((checked + 1))
    case $'\n'"$manifest_realpaths" in
        *$'\n'"$binary"$'\n'*) continue ;;
    esac
    name="$(basename "$binary")"
    if reason="$(coverage_exclusion_reason "$name")"; then
        printf 'note: %s is run by ctest and deliberately not profiled -- %s\n' "$name" "$reason"
    else
        unexplained+=("$name")
    fi
done <<< "$ctest_binaries"

if [ "${#unexplained[@]}" -gt 0 ]; then
    echo "error: ctest runs the following binaries, but llvm-cov is not given them, so" >&2
    echo "everything they execute is measured as if it had never run:" >&2
    printf '  %s\n' "${unexplained[@]}" >&2
    echo >&2
    echo "Either instrument the target -- if(AF_COVERAGE) apply_coverage(<target>) in its" >&2
    echo "CMakeLists.txt, with a name ending in _tests or with the TEST option -- or add it" >&2
    echo "to coverage_exclusion_reason() in this script together with the reason it stays" >&2
    echo "out. See the header comment: the three previous occurrences of this defect" >&2
    echo "(morph#141, morph#179, morph#403) were all silent." >&2
    exit 1
fi

printf 'ok: %d test binary/binaries profiled; %d ctest binary/binaries checked\n' \
    "$manifest_count" "$checked"
