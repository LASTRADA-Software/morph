#!/usr/bin/env bash
# Usage: bash scripts/test_check_catch2_pin.sh
#
# Self-test for scripts/check_catch2_pin.sh, the gate that keeps the Catch2 the
# clang-tidy-diff job analyses against a recorded, checked fact rather than
# whatever apt shipped that morning.
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one is doubly exposed: the tree it guards is correct
# today, and the runner's package is the pinned one today, so the gate passes
# today whether or not either half is looking at anything. Both halves are
# therefore driven from the wrong side as well as the right one -- every drift
# the gate claims to catch is reintroduced into a scratch copy of the tree, or
# into a synthetic include directory, and must be caught for the stated reason
# rather than merely with a nonzero exit.
#
# One mutation at a time: applied together, a single detection would mask
# every other.
#
# The checker enumerates the tree with `git ls-files`, so each case runs
# against a throwaway git repository holding a copy of this one's tracked
# files rather than against a directory argument.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_catch2_pin.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

readonly pristine="${scratch}/pristine"
mkdir -p "$pristine"
while IFS= read -r -d '' tracked; do
    mkdir -p "${pristine}/$(dirname "$tracked")"
    cp "${repo_root}/${tracked}" "${pristine}/${tracked}"
done < <(cd "$repo_root" && git ls-files -z)
git -C "$pristine" init -q
git -C "$pristine" add -A

make_tree() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -R "${pristine}/." "$dest"
}

# `sed -i` is not portable between GNU and BSD sed; edit through a temp file.
edit() {
    local file="$1"; shift
    sed "$@" "$file" > "${file}.new"
    mv "${file}.new" "$file"
}

# A synthetic Catch2 include tree carrying exactly the three version macros the
# checker reads. This is what lets the behavioural half be driven from a
# version the machine running the self-test does not have installed.
make_catch2() {
    local dir="$1" version="$2"
    local major="${version%%.*}" rest="${version#*.}"
    local minor="${rest%%.*}" patch="${rest#*.}"
    rm -rf "$dir"
    mkdir -p "${dir}/catch2"
    {
        printf '#define CATCH_VERSION_MAJOR %s\n' "$major"
        printf '#define CATCH_VERSION_MINOR %s\n' "$minor"
        printf '#define CATCH_VERSION_PATCH %s\n' "$patch"
    } > "${dir}/catch2/catch_version_macros.hpp"
}

# An include directory with no Catch2 in it at all.
readonly empty_include="${scratch}/no-catch2"
mkdir -p "$empty_include"

# `$expected` is a substring the resulting diagnostic must contain; without it
# a mutation that broke the tree in some unrelated way -- a mangled sed, a file
# the mutator emptied -- would count as a detection, and this self-test would
# report a gate that no longer detects anything as fully working.
expect_caught() {
    local description="$1" mutator="$2" expected="$3" extra_args="${4:-}" catch2_dir="${5:-}"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && MORPH_CATCH2_INCLUDE_DIR="$catch2_dir" \
            bash "$checker" . $extra_args 2>&1 )"; then
        fail "NOT caught: ${description} -- the gate passed a tree it should reject"
        printf '%s\n' "$output" >&2
        return
    fi
    if printf '%s' "$output" | grep -qF "$expected"; then
        note "caught: ${description}"
    else
        fail "caught for the WRONG reason: ${description} -- no diagnostic containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

# The mirror, for the false positives this gate must not manufacture. A rule
# that rejected every mention of a Catch2 version anywhere would "catch" every
# case above while being useless.
expect_accepted() {
    local description="$1" mutator="$2" expected="${3:-}" extra_args="${4:-}" catch2_dir="${5:-}"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if ! output="$( cd "$tree" && MORPH_CATCH2_INCLUDE_DIR="$catch2_dir" \
            bash "$checker" . $extra_args 2>&1 )"; then
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it should accept:"
        printf '%s\n' "$output" >&2
        return
    fi
    if [ -z "$expected" ] || printf '%s' "$output" | grep -qF "$expected"; then
        note "accepted: ${description}"
    else
        fail "accepted but SILENT: ${description} -- no output containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

readonly pinned_catch2="${scratch}/catch2-3.4.0"
readonly moved_catch2="${scratch}/catch2-3.5.3"
make_catch2 "$pinned_catch2" 3.4.0
make_catch2 "$moved_catch2" 3.5.3

# -- The unmodified tree must pass -------------------------------------------
make_tree "${scratch}/clean"
if output="$( cd "${scratch}/clean" && MORPH_CATCH2_INCLUDE_DIR="$pinned_catch2" \
        bash "$checker" . --strict 2>&1 )"; then
    note "the unmodified tree passes against the pinned Catch2"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# -- A. The textual half -----------------------------------------------------
expect_caught "a doc asserting CI pins catch2 3.5.3 while ci.yml pins 3.4.0" \
    "printf '%s\n' 'The lint leg reproduces because CI pins catch2 3.5.3 there.' \
        >> docs/spec/testing_strategy.md" \
    "states 'CI pins catch2 3.5.3', but .github/workflows/ci.yml pins catch2 3.4.0"

# The direction morph#666 will actually take: the runner image moves, someone
# updates CATCH2_VERSION, and the nine .clang-tidy copies stay where they are.
expect_caught "ci.yml bumped to 3.5.3 while the nine copies still say 3.4.0" \
    "edit .github/workflows/ci.yml -e 's/^  CATCH2_VERSION: \"3.4.0\"/  CATCH2_VERSION: \"3.5.3\"/'" \
    "but .github/workflows/ci.yml pins catch2 3.5.3"

# Rule B: the rewording that would defeat rule A alone. The nine copies said
# the same sentence nine times; a tenth site wording it differently is exactly
# how the number went unchecked in the first place.
expect_caught "a CI Catch2 claim in an unrecognised phrasing, even with the right version" \
    "printf '%s\n' 'The CI lint job installs catch2 3.4.0 from apt.' \
        >> docs/spec/testing_strategy.md" \
    "phrasing this gate cannot check"

expect_caught "a CI Catch2 claim in an unrecognised phrasing with the wrong version" \
    "printf '%s\n' 'Measured against Catch2 3.16.0 while CI has catch2-3.5.3.' \
        >> docs/spec/testing_strategy.md" \
    "phrasing this gate cannot check"

expect_caught "every canonical assertion removed from the tree" \
    "for f in \$(git grep -lF 'CI pins catch2' -- . ':!scripts/check_catch2_pin.sh' \
                                                 ':!scripts/test_check_catch2_pin.sh'); do
         edit \"\$f\" -e 's/CI pins catch2 [0-9.]*/the pinned Catch2/g'
     done" \
    "no 'CI pins catch2 <x.y.z>' assertion found anywhere in the tree"

expect_caught "ci.yml with no CATCH2_VERSION to read" \
    "edit .github/workflows/ci.yml -e 's/^  CATCH2_VERSION: \"3.4.0\"/  UNRELATED_CATCH2: \"3.4.0\"/'" \
    'no `CATCH2_VERSION: "<x.y.z>"` found'

# A Catch2 version discussed without invoking CI in the same breath is not this
# gate's business; rejecting it would make the gate unsatisfiable for any
# document recording a local measurement or a FetchContent tag.
expect_accepted "a Catch2 version named with no CI reference on the line" \
    "printf '%s\n' 'Reproduced against Catch2 3.16.0 on this workstation.' \
        >> docs/spec/testing_strategy.md"

expect_accepted "a historical record carrying the documented marker" \
    "printf '%s\n' 'Before noble, CI had catch2 2.13.10 (catch2-pin: historical).' \
        >> docs/spec/testing_strategy.md"

# -- B. The behavioural half -------------------------------------------------
# The whole reason this gate is not just another prose checker: the runner's
# package moving under an unpinned `apt-get install -y catch2` must fail the
# job rather than quietly change what it measures.
expect_caught "--strict against a runner whose Catch2 package has moved" \
    "true" \
    "installed Catch2 is 3.5.3" \
    "--strict" "$moved_catch2"

expect_caught "--strict with no Catch2 installed at all" \
    "true" \
    "no Catch2 headers found on this machine" \
    "--strict" "$empty_include"

# Without --strict -- a workstation run -- a divergence is reported rather than
# failed, because a workstation is not required to carry the runner's package.
# But it must be *reported*: silence here is the defect morph#666 is about.
expect_accepted "a workstation whose Catch2 differs is warned, not failed" \
    "true" \
    "A local clang-tidy-diff run is therefore NOT the measurement" \
    "" "$moved_catch2"

# And a workstation that does match must not be warned, or the notice becomes
# noise that gets filtered out.
expect_accepted "a workstation whose Catch2 matches the pin is not warned" \
    "true" \
    "matches the pin" \
    "" "$pinned_catch2"

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all catch2-pin checker self-tests passed"
