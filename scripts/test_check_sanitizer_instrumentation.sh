#!/usr/bin/env bash
# Usage: bash scripts/test_check_sanitizer_instrumentation.sh
#
# Self-test for scripts/check_sanitizer_instrumentation.sh, the gate that fails
# when a sanitizer leg runs a binary carrying none of that sanitizer's runtime
# symbols (morph#542). A lint gate nobody tests reports green whether or not it
# still detects anything, and this one guards a silence: an uninstrumented
# sanitizer leg builds, runs the whole suite, passes, and costs its full
# runtime -- indistinguishable from a leg that found nothing wrong.
#
# morph#675 added a second mode (`--binary <file> <mode>`) for the question the
# sweep refused: "is this one binary instrumented?", asked by a developer who
# built a single target under a sanitizer preset. The important property of
# that mode is not what it reports but where it cannot be used -- if it could
# satisfy a CI invocation, the sweep's floor would be gone and the gate would
# be back to passing on one file. Case 6 is that property; case 7 is the floor
# itself, asserted unchanged.
#
# No compiler is needed and no sanitizer runtime is involved. The fixtures are
# ordinary binaries carrying a hand-written function whose *name* is what `nm`
# reports, which is exactly what the gate inspects -- so this runs in seconds
# in drift-guard.yml rather than behind a sanitizer build.
#
# Asserts eight directions:
#
#   1. sweep: two instrumented ctest binaries            -> pass
#   2. sweep: one of them uninstrumented                 -> fail, naming it
#   3. sweep: ctest lists no tests                       -> fail, not a vacuous pass
#   4. narrow: an instrumented binary                    -> pass, reporting the count
#   5. narrow: an uninstrumented binary                  -> fail, naming it
#   6. narrow: GITHUB_ACTIONS set                        -> refused, exit 2
#   7. sweep: a single-binary tree                       -> still fails on the floor
#   8. narrow: a missing path and a non-binary           -> fail, not a vacuous pass
#
# 3, 6, 7 and 8 matter as much as 2. Without 7 the floor could have been
# lowered rather than sidestepped, which is the change this gate must not have
# made; without 6 the narrow mode would be a way to satisfy a workflow step
# having examined one file; and without 8 it would report success over a path
# that is not there.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_sanitizer_instrumentation.sh"

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

for tool in cc ctest jq nm file; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'error: %s is required by this self-test and is not on PATH\n' "$tool" >&2
        exit 1
    fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ── Fixtures ────────────────────────────────────────────────────────────────
# The gate's entire measurement is `nm -C <file> | grep -c __<mode>_`, so a
# binary that defines a function of that name is indistinguishable from an
# instrumented one *to the gate*, which is what makes it the right fixture: it
# exercises the gate rather than the compiler. `-fsanitize=undefined` would
# make the fixture depend on a sanitizer runtime being installed, and would
# test clang instead of this script.
#
# A complementary gate (a gate removed on 2026-09-23) did compile real
# instrumentation behaviourally, against the flags apply_sanitizers() emits.
# It was removed with the meta-gates, so nothing covers that half now.

case_dir() {
    local dir="${work}/$1"
    rm -rf "$dir"
    mkdir -p "$dir"
    printf '%s' "$dir"
}

# make_binary <dir> <name> <symbol-prefix|->
#   `-` produces a binary with no sanitizer-shaped symbol at all.
make_binary() {
    local dir="$1" name="$2" prefix="$3"
    local src="${work}/${name}.c"
    if [ "$prefix" = "-" ]; then
        printf 'int main(void) { return 0; }\n' > "$src"
    else
        printf 'void %shandle_type_mismatch_v1(void) { }\nint main(void) { return 0; }\n' \
            "$prefix" > "$src"
    fi
    # No -s / no strip: the gate reads the symbol table, so the fixture has to
    # keep one. Default cc output on both Linux and macOS does.
    cc -O0 -o "${dir}/${name}" "$src"
    printf '%s' "${dir}/${name}"
}

# A hand-written CTestTestfile.cmake is all `ctest --show-only=json-v1` needs,
# which is what keeps this self-test free of a configure.
ctest_file() {
    local dir="$1"
    shift
    : > "${dir}/CTestTestfile.cmake"
    local path
    for path in "$@"; do
        printf 'add_test([=[%s]=] "%s")\n' "$(basename "$path")" "$path" >> "${dir}/CTestTestfile.cmake"
    done
}

# `grep <<<` rather than a pipe, for the reason
# scripts/test_check_coverage_objects.sh states: under `set -o pipefail` a
# `grep -q` that matches early can close the pipe under its producer and the
# pipeline then reports the producer's SIGPIPE status rather than grep's match.
mentions() {
    grep -q -- "$1" <<< "$2"
}

# Every invocation below runs with GITHUB_ACTIONS unset, except case 6 which
# sets it deliberately. The self-test itself runs *in* GitHub Actions, where
# the variable is set for every step, so inheriting it would turn case 4 into
# an accidental re-run of case 6 and leave the narrow mode's actual verdict
# untested in the one place this file runs.
run_checker() {
    env -u GITHUB_ACTIONS bash "$checker" "$@"
}

# ── 1. sweep: two instrumented ctest binaries -> pass ───────────────────────
dir="$(case_dir sweep_clean)"
a="$(make_binary "$dir" morph_tests __ubsan_)"
b="$(make_binary "$dir" morph_net_tests __ubsan_)"
ctest_file "$dir" "$a" "$b"

if output="$(run_checker "$dir" ubsan 2>&1)"; then
    if mentions '2 ctest binaries' "$output"; then
        note "ok: a fully instrumented tree is accepted, and the count reported"
    else
        fail "the tree was accepted but the summary does not report two binaries:"
        printf '%s\n' "$output" >&2
    fi
else
    fail "a fully instrumented tree was rejected:"
    printf '%s\n' "$output" >&2
fi

# ── 2. sweep: one uninstrumented binary -> fail, naming it ──────────────────
# morph#542 itself, in miniature. A nonzero exit alone is not enough: the gate
# has several failure paths and one of them firing for an unrelated reason
# would look like a pass of this case, so the message must name the binary.
dir="$(case_dir sweep_dirty)"
a="$(make_binary "$dir" morph_tests __ubsan_)"
b="$(make_binary "$dir" morph_net_tests -)"
ctest_file "$dir" "$a" "$b"

if output="$(run_checker "$dir" ubsan 2>&1)"; then
    fail "an uninstrumented ctest binary was accepted -- this is the defect the gate exists for:"
    printf '%s\n' "$output" >&2
elif ! mentions 'morph_net_tests' "$output"; then
    fail "the uninstrumented binary was rejected, but the message does not name morph_net_tests:"
    printf '%s\n' "$output" >&2
else
    note "ok: an uninstrumented ctest binary is rejected, and named"
fi

# ── 2b. the symbol is keyed on the mode ─────────────────────────────────────
# The same tree that passes as `ubsan` must fail as `tsan`. Without this, an
# `__asan_`-only assertion -- the exact defect morph#542 records -- would
# satisfy every other case here.
dir="$(case_dir sweep_wrong_mode)"
a="$(make_binary "$dir" morph_tests __ubsan_)"
b="$(make_binary "$dir" morph_net_tests __ubsan_)"
ctest_file "$dir" "$a" "$b"

if output="$(run_checker "$dir" tsan 2>&1)"; then
    fail "a ubsan-only tree was accepted as tsan-instrumented -- the symbol is not keyed on the mode:"
    printf '%s\n' "$output" >&2
else
    note "ok: a ubsan-instrumented tree does not satisfy the tsan mode"
fi

# ── 3. sweep: ctest lists no tests -> fail, not a vacuous pass ──────────────
dir="$(case_dir sweep_empty)"
: > "${dir}/CTestTestfile.cmake"

if output="$(run_checker "$dir" ubsan 2>&1)"; then
    fail "an empty ctest test list was reported as clean -- the gate verified nothing:"
    printf '%s\n' "$output" >&2
elif ! mentions 'listed no tests' "$output"; then
    fail "the empty test list was rejected, but not for being empty:"
    printf '%s\n' "$output" >&2
elif ! mentions 'genuinely registers no tests' "$output"; then
    fail "the empty test list was rejected, but the message does not say ctest succeeded -- it reads the same as a listing that failed, which is morph#690:"
    printf '%s\n' "$output" >&2
else
    note "ok: an empty ctest test list is rejected rather than passing vacuously, and named as empty rather than broken"
fi

# ── 3b. sweep: ctest *fails* to list -> the reason is printed (morph#690) ───
# The distinction case 3 cannot make on its own, and the one that cost three CI
# runs: "ctest enumerated a tree with no tests" and "ctest died before printing
# any JSON" both arrive here as an empty list. On the bank-ubsan leg it was the
# second -- one DISCOVERY_MODE PRE_TEST suite's binary aborted at listing time,
# ctest exited 8 with an empty stdout and a CMake FATAL_ERROR on stderr, and
# the checker's `2>/dev/null` threw that sentence away.
#
# The fixture reproduces the shape rather than the cause: a CTestTestfile.cmake
# that fails while being read makes ctest exit nonzero with nothing on stdout,
# which is exactly the state the checker has to tell apart. Asserting on the
# fixture's own marker string, not on ctest's wording, is what makes this a
# test of the pass-through rather than of ctest.
dir="$(case_dir sweep_listing_failed)"
printf 'message(FATAL_ERROR "morph690_fixture_marker: listing deliberately failed")\n' \
    > "${dir}/CTestTestfile.cmake"

if output="$(run_checker "$dir" ubsan 2>&1)"; then
    fail "a ctest listing that failed outright was reported as clean:"
    printf '%s\n' "$output" >&2
elif ! mentions 'morph690_fixture_marker' "$output"; then
    fail "the failed listing was rejected, but ctest's own reason was discarded -- the caller is left with 'listed no tests' and no cause, which is morph#690:"
    printf '%s\n' "$output" >&2
else
    note "ok: a ctest listing that failed prints the reason it failed"
fi

# ── 4. narrow: an instrumented binary -> pass, reporting the count ──────────
dir="$(case_dir narrow_clean)"
one="$(make_binary "$dir" morph_tests __tsan_)"

if output="$(run_checker --binary "$one" tsan 2>&1)"; then
    if ! mentions 'morph_tests carries' "$output"; then
        fail "the narrow mode passed but does not name the binary and its count:"
        printf '%s\n' "$output" >&2
    elif ! mentions 'not a substitute' "$output"; then
        fail "the narrow mode passed without saying it applied no floor -- a reader could take it for the sweep:"
        printf '%s\n' "$output" >&2
    else
        note "ok: --binary reports one instrumented binary, and says what it did not do"
    fi
else
    fail "--binary rejected an instrumented binary:"
    printf '%s\n' "$output" >&2
fi

# ── 5. narrow: an uninstrumented binary -> fail, naming it ──────────────────
dir="$(case_dir narrow_dirty)"
one="$(make_binary "$dir" morph_tests -)"

if output="$(run_checker --binary "$one" tsan 2>&1)"; then
    fail "--binary accepted a binary with no __tsan_ symbols:"
    printf '%s\n' "$output" >&2
elif ! mentions 'morph_tests' "$output"; then
    fail "--binary rejected the binary but did not name it:"
    printf '%s\n' "$output" >&2
else
    note "ok: --binary rejects an uninstrumented binary, and names it"
fi

# ── 6. narrow: refused under GITHUB_ACTIONS ────────────────────────────────
# The whole safety of morph#675's addition. If this passes, the narrow mode is
# reachable from a workflow step, and a leg could report "instrumented" having
# examined one file -- which is the sweep's floor removed by another route.
# Asserted on a fixture that would otherwise *pass* (case 4's), so a refusal
# here cannot be some other failure wearing the right exit code.
dir="$(case_dir narrow_in_ci)"
one="$(make_binary "$dir" morph_tests __tsan_)"

set +e
output="$(GITHUB_ACTIONS=true bash "$checker" --binary "$one" tsan 2>&1)"
status=$?
set -e

if [ "$status" -eq 0 ]; then
    fail "--binary ran under GITHUB_ACTIONS -- the floor can now be bypassed from a workflow step:"
    printf '%s\n' "$output" >&2
elif [ "$status" -ne 2 ]; then
    fail "--binary under GITHUB_ACTIONS exited ${status}; expected 2 (a usage refusal, not a finding):"
    printf '%s\n' "$output" >&2
elif ! mentions 'refused under GITHUB_ACTIONS' "$output"; then
    fail "--binary failed under GITHUB_ACTIONS, but not with the refusal:"
    printf '%s\n' "$output" >&2
else
    note "ok: --binary is refused under GITHUB_ACTIONS, on a fixture that otherwise passes"
fi

# ── 7. sweep: a single-binary tree still fails on the floor ────────────────
# morph#675 is about making the narrow case answerable, not about lowering the
# floor. This is the regression guard for the difference: the sweep over a
# one-binary tree must still refuse, with the floor's own message, even though
# that binary is instrumented.
dir="$(case_dir sweep_single)"
one="$(make_binary "$dir" morph_tests __ubsan_)"
ctest_file "$dir" "$one"

if output="$(run_checker "$dir" ubsan 2>&1)"; then
    fail "the sweep accepted a one-binary tree -- the floor has been lowered:"
    printf '%s\n' "$output" >&2
elif ! mentions 'too few for this check to mean anything' "$output"; then
    fail "the one-binary tree was rejected, but not by the floor:"
    printf '%s\n' "$output" >&2
else
    note "ok: the sweep's floor is unchanged by the narrow mode"
fi

# ── 8. narrow: a missing path and a non-binary -> fail ─────────────────────
if output="$(run_checker --binary "${work}/definitely-absent" ubsan 2>&1)"; then
    fail "--binary reported success over a path that does not exist:"
    printf '%s\n' "$output" >&2
elif ! mentions 'no such file' "$output"; then
    fail "--binary rejected the missing path, but not for being missing:"
    printf '%s\n' "$output" >&2
else
    note "ok: --binary over a missing path is a failure, not a vacuous pass"
fi

dir="$(case_dir narrow_not_a_binary)"
printf 'this is a shell script, not an ELF file\n' > "${dir}/not_a_binary"
chmod +x "${dir}/not_a_binary"

if output="$(run_checker --binary "${dir}/not_a_binary" ubsan 2>&1)"; then
    fail "--binary reported success over a file nm cannot read:"
    printf '%s\n' "$output" >&2
elif ! mentions 'not an ELF or Mach-O binary' "$output"; then
    fail "--binary rejected the non-binary, but not for being one:"
    printf '%s\n' "$output" >&2
else
    note "ok: --binary over a non-binary is a failure, not a vacuous pass"
fi

# ── 9. an unknown mode is refused in both modes ────────────────────────────
dir="$(case_dir unknown_mode)"
one="$(make_binary "$dir" morph_tests __ubsan_)"
ctest_file "$dir" "$one" "$one"

for args in "$dir msan" "--binary $one msan"; do
    # shellcheck disable=SC2086
    if output="$(run_checker $args 2>&1)"; then
        fail "the unknown mode 'msan' was accepted (args: ${args}):"
        printf '%s\n' "$output" >&2
    elif ! mentions "unknown mode 'msan'" "$output"; then
        fail "'msan' was rejected, but not as an unknown mode (args: ${args}):"
        printf '%s\n' "$output" >&2
    else
        note "ok: the unknown mode 'msan' is refused (args: ${args})"
    fi
done

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

printf '\nall self-test checks passed\n'
