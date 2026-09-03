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
        # The first three below run in the coverage leg itself
        # (MORPH_BUILD_EXAMPLES defaults ON) and none reaches llvm-cov; the
        # rest are the same defect in suites only a local configure builds. They
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
        # Suites the CI coverage leg does not build, but a local coverage
        # configure can. Without these entries, adding any of their options to
        # `cmake --preset clang-coverage` turns coverage.sh from "produces a
        # report" into "exits 1 and produces nothing" -- a gate against silent
        # omission should not itself make a legitimate configure unusable. They
        # are the same defect as the three above and get the same one-line fix;
        # they are only separated because nothing in CI has ever measured them.
        bank_tests|bank_gui_tests|bank_gui_qml_tests)
            echo "GAP: built by -DMORPH_BUILD_BANK_EXAMPLE=ON, which the coverage leg does not set, and never instrumented (examples/bank/CMakeLists.txt). Fix: an if(AF_COVERAGE) apply_coverage(<target>) block there; the names already end in _tests"
            ;;
        morph_vetted_hmac_libsodium_tests|morph_vetted_hmac_openssl_tests)
            echo "GAP: built by -DMORPH_BUILD_HMAC_EXAMPLES=ON, which the coverage leg does not set, and never instrumented (examples/vetted_hmac/CMakeLists.txt). Fix: an if(AF_COVERAGE) apply_coverage(<target>) block there"
            ;;
        morph_forms_qml_tests|morph_forms_controller_core_tests)
            echo "GAP: built by -DMORPH_BUILD_FORMS_QML=ON, which the coverage leg does not set, and never instrumented (src/qt/forms/CMakeLists.txt). These drive include/morph/forms through the QML renderer, so they are the suites most likely to move the forms number when they are instrumented"
            ;;
        *)
            return 1
            ;;
    esac
}

ctest_profile_scratch="$(mktemp -d)"
trap 'rm -rf "$ctest_profile_scratch"' EXIT

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

# A manifest holding only blank lines passes the `-s` test above but names
# nothing, which would make every check below trivially true.
if [ "$manifest_count" -eq 0 ]; then
    echo "error: ${manifest} names no binaries." >&2
    echo "A coverage configure that registered nothing cannot produce a report over" >&2
    echo "anything; see cmake/compiler_options.cmake's apply_coverage()." >&2
    exit 1
fi

# ── What ctest actually runs ─────────────────────────────────────────────────
#
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
# What this cannot see, stated rather than left to be discovered: a binary a
# test *spawns* appears in no ctest command at all. tests/qt's qt_test_server
# and qt_test_client are exactly that -- the far end of the process-separation
# tests -- and they are covered here only because tests/qt/CMakeLists.txt opts
# them in explicitly with apply_coverage(... TEST). Any future test that forks
# a helper has to do the same; this gate will not ask for it.
#
# The JSON is streamed straight into python rather than captured into a shell
# variable first: `ctest --show-only=json-v1` over this repository's ~2,400
# tests is about 2 MB, and there is no reason for it to live in the shell's
# memory twice on the way past.
#
# Compared as realpath()s: the manifest holds generator-resolved absolute
# paths, while a test may be registered with either form.
ctest_binaries="$(
    {
        if [ -n "$ctest_json_file" ]; then
            cat "$ctest_json_file"
        else
            # LLVM_PROFILE_FILE into a scratch directory: --show-only still
            # *runs* every PRE_TEST discovery script, which executes the
            # instrumented test binaries with --list-tests. Left alone they
            # would each drop a default.profraw into the build tree, and
            # scripts/check_coverage_profiles.sh's `find ... -name
            # '*.profraw'` (invoked from scripts/coverage.sh) would sweep
            # those listing runs into the next merge.
            LLVM_PROFILE_FILE="${ctest_profile_scratch}/discovery-%p.profraw" \
                ctest --test-dir "$build_dir" --show-only=json-v1
        fi
    } | BUILD_DIR="$build_dir" python3 -c '
import json, os, sys

try:
    document = json.load(sys.stdin)
    tests = document["tests"]
except Exception as error:
    # Without this the caller sees a bare traceback and, because the enclosing
    # command substitution runs under `set -e`, coverage.sh dies with no
    # indication of which script failed or why. `ctest --show-only=json-v1`
    # prints anything a TEST_INCLUDE_FILES discovery script writes to stdout
    # *before* the document, and emits nothing parseable at all when it fails.
    sys.stderr.write(
        "error: could not read the test list for {}: {}\n"
        "`ctest --test-dir {} --show-only=json-v1` did not produce a JSON document "
        "with a \"tests\" array. Run it by hand: a configure error, or a test-discovery "
        "script writing to stdout, both land here.\n".format(
            os.environ["BUILD_DIR"], error, os.environ["BUILD_DIR"]))
    raise SystemExit(1)

build_root = os.path.realpath(os.environ["BUILD_DIR"]) + os.sep

# Catch2 registers one ctest case per assertion-level TEST_CASE, so the same
# command[0] string recurs thousands of times for a handful of binaries.
# realpath() walks and readlink()s every component, so resolving per occurrence
# is ~40k syscalls to learn fifteen answers; resolve per distinct string
# instead.
resolved = {}
seen = {}
for test in tests:
    for argument in test.get("command") or []:
        candidate = resolved.get(argument)
        if candidate is None:
            candidate = resolved[argument] = os.path.realpath(argument)
        if not candidate.startswith(build_root):
            continue
        if not os.path.isfile(candidate) or not os.access(candidate, os.X_OK):
            continue
        seen[candidate] = None
print("\n".join(seen))
'
)"

unexplained=()
checked=0
gaps=0
while IFS= read -r binary; do
    [ -n "$binary" ] || continue
    checked=$((checked + 1))
    case $'\n'"$manifest_realpaths" in
        *$'\n'"$binary"$'\n'*) continue ;;
    esac
    name="$(basename "$binary")"
    if reason="$(coverage_exclusion_reason "$name")"; then
        # A decision and an unfixed defect must not print the same way. Saying
        # "deliberately not profiled" about a GAP is precisely the sentence
        # that lets one stop being read as a defect.
        case "$reason" in
            GAP:*)
                gaps=$((gaps + 1))
                printf 'warning: %s is run by ctest and is NOT profiled -- %s\n' \
                    "$name" "${reason#GAP: }"
                ;;
            *)
                printf 'note: %s is run by ctest and deliberately not profiled -- %s\n' \
                    "$name" "$reason"
                ;;
        esac
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

# A gate that examined nothing must not report success. `ctest --show-only`
# against a build directory that was never configured for tests, or whose test
# registration silently produced an empty list, returns a valid JSON document
# with zero tests -- and every check below it then passes by having nothing to
# check. That is the same vacuous green scripts/check_rung_filters.sh guards
# against at its own end, and it is worth guarding here twice over, because a
# coverage report over a suite ctest does not know about is precisely the
# shape of failure this gate was written for.
if [ "$checked" -eq 0 ]; then
    echo "error: ctest reported no test binaries at all under ${build_dir}." >&2
    echo "This gate then verified nothing, which is not the same as finding nothing" >&2
    echo "wrong. Check that the build directory is configured with MORPH_BUILD_TESTS=ON" >&2
    echo "and that 'ctest --test-dir ${build_dir} --show-only' lists cases." >&2
    exit 1
fi

if [ "$gaps" -gt 0 ]; then
    printf 'ok: %d test binary/binaries profiled; %d ctest binary/binaries checked; %d unfixed gap(s)\n' \
        "$manifest_count" "$checked" "$gaps"
else
    printf 'ok: %d test binary/binaries profiled; %d ctest binary/binaries checked\n' \
        "$manifest_count" "$checked"
fi
