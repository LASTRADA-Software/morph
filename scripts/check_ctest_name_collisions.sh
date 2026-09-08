#!/usr/bin/env bash
# Usage: bash scripts/check_ctest_name_collisions.sh BUILD_DIR [CTEST_JSON]
#
# Fails if two ctest tests registered in BUILD_DIR share a name.
#
# A ctest test name is global to the build tree, not scoped to the target or
# the directory it was registered from. `add_test()` (and therefore Catch2's
# catch_discover_tests) refuses a duplicate name only within one directory;
# across two directories CMake accepts it, and every ctest operation that
# resolves a test *by name* then addresses both at once. That is not a
# theoretical hazard:
#
#   * `set_tests_properties(<name> PROPERTIES LABELS ...)` matches every test
#     of that name in the tree and CTest *appends* the labels, so the first
#     registration of a duplicated name accumulates the second's labels too.
#     In this repository that made `ctest -L ladder-<rung>` over-select: it ran
#     another rung's binary alongside its own (morph#464 -- crm reported on 180
#     cases while owning 168).
#   * a failure line, `--output-junit` and CDash all identify a test by name,
#     so a duplicated name does not say which binary failed.
#
# Neither symptom is loud. An over-selecting label filter is green in exactly
# the same way a correct one is; it simply measures something other than what
# it names, which is the failure this repository has shipped repeatedly (a
# coverage script measuring a quarter of the tree, a sanitizer job green with
# no instrumentation, a clang-tidy gate whose exit status was `tee`'s). So the
# uniqueness is asserted here rather than assumed.
#
# The fix for the ladder is `TEST_PREFIX "<rung>."` in
# cmake/morph_add_rung.cmake, which makes the names unique by construction --
# this gate is what keeps them that way the next time a rung is template-copied
# or a suite is registered by hand.
#
# CTEST_JSON (second argument) supplies `ctest --show-only=json-v1` output from
# a file instead of running ctest, which is what
# scripts/test_check_ctest_name_collisions.sh drives the gate with.
set -euo pipefail

build_dir="${1:-}"
ctest_json_file="${2:-}"
readonly build_dir ctest_json_file

if [ -z "$build_dir" ]; then
    echo "usage: bash scripts/check_ctest_name_collisions.sh BUILD_DIR [CTEST_JSON]" >&2
    exit 2
fi

if [ -z "$ctest_json_file" ] && [ ! -d "$build_dir" ]; then
    echo "error: ${build_dir} is not a directory, so there is no test list to check." >&2
    exit 1
fi

# LLVM_PROFILE_FILE into a scratch directory, for the same reason
# scripts/check_coverage_objects.sh does it: `--show-only` still runs any
# PRE_TEST discovery script, which executes instrumented binaries with
# --list-tests, and their default.profraw files would otherwise be swept into
# the next coverage merge.
profile_scratch="$(mktemp -d)"
trap 'rm -rf "$profile_scratch"' EXIT

{
    if [ -n "$ctest_json_file" ]; then
        cat "$ctest_json_file"
    else
        LLVM_PROFILE_FILE="${profile_scratch}/discovery-%p.profraw" \
            ctest --test-dir "$build_dir" --show-only=json-v1
    fi
} | BUILD_DIR="$build_dir" python3 -c '
import collections, json, os, sys

build_dir = os.environ["BUILD_DIR"]

try:
    document = json.load(sys.stdin)
    tests = document["tests"]
except Exception as error:
    sys.stderr.write(
        "error: could not read the test list for {}: {}\n"
        "`ctest --test-dir {} --show-only=json-v1` did not produce a JSON document "
        "with a \"tests\" array. Run it by hand: a configure error, or a test-discovery "
        "script writing to stdout, both land here.\n".format(build_dir, error, build_dir))
    raise SystemExit(1)

by_name = collections.OrderedDict()
for test in tests:
    command = test.get("command") or []
    binary = command[0] if command else "<no command>"
    by_name.setdefault(test.get("name", "<unnamed>"), []).append(binary)

# A gate that examined nothing must not report success -- the same vacuous
# green scripts/check_coverage_objects.sh guards against at its own end. A
# build directory configured without tests, or whose registration silently
# produced nothing, yields a valid document with an empty array, and every
# check below it then passes by having nothing to check.
if not by_name:
    sys.stderr.write(
        "error: ctest reported no tests at all under {}.\n"
        "This gate then verified nothing, which is not the same as finding nothing\n"
        "wrong. Check that the build directory is configured with MORPH_BUILD_TESTS=ON\n"
        "and that `ctest --test-dir {} --show-only` lists cases.\n".format(build_dir, build_dir))
    raise SystemExit(1)

duplicates = [(name, binaries) for name, binaries in by_name.items() if len(binaries) > 1]
if duplicates:
    sys.stderr.write(
        "error: {} ctest test name(s) under {} are registered more than once. A ctest\n"
        "name is global to the build tree, so every by-name operation -- LABELS via\n"
        "set_tests_properties (which CTest *appends*, so one entry accumulates the\n"
        "other entry\x27s labels and `ctest -L` over-selects), --output-junit, CDash, and\n"
        "the failure line itself -- addresses all of them at once:\n\n".format(
            len(duplicates), build_dir))
    for name, binaries in duplicates:
        sys.stderr.write("  {!r} x{}\n".format(name, len(binaries)))
        for binary in binaries:
            sys.stderr.write("      {}\n".format(binary))
    sys.stderr.write(
        "\nGive the registrations distinct names: TEST_PREFIX/TEST_SUFFIX on the\n"
        "catch_discover_tests call (cmake/morph_add_rung.cmake does this per rung),\n"
        "or rename the TEST_CASE. See this script\x27s header and morph#464.\n")
    raise SystemExit(1)

print("ok: {} ctest test names under {} are unique".format(len(by_name), build_dir))
'
