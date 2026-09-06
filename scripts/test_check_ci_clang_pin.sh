#!/usr/bin/env bash
# Usage: bash scripts/test_check_ci_clang_pin.sh
#
# Self-test for scripts/check_ci_clang_pin.sh, the gate that keeps every stated
# CI clang major in step with .github/workflows/ci.yml's CLANG_VERSION.
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one is especially exposed to that: the tree it guards is
# correct today, so the gate passes today whether or not it is looking at
# anything at all. That is exactly the shape of the defect it was written for
# (morph#457) -- six sentences that were true when written and silently stopped
# being true when the pin moved, with nothing anywhere returning nonzero.
#
# So the gate is checked in both directions: the unmodified tree must pass, and
# every drift it claims to catch is reintroduced into a scratch copy of the
# tree, one at a time, and must be caught -- for the stated reason, not merely
# with a nonzero exit.
#
# One mutation at a time matters: applied together, a single detection would
# mask every other.
#
# The checker enumerates the tree with `git ls-files`, so each case runs
# against a throwaway git repository holding a copy of this one's tracked files
# rather than against a directory argument.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_ci_clang_pin.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# One pristine copy of every tracked file, in its own git repository (the
# checker enumerates the tree with `git ls-files`, so a plain directory would
# not do). Built once; each case gets a directory copy of it.
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

# Each mutation must be caught, and caught *for the stated reason*. `$expected`
# is a substring the resulting diagnostic must contain; without it a mutation
# that broke the tree in some unrelated way -- a mangled sed, a file the
# mutator emptied -- would count as a detection, and this self-test would
# report a gate that no longer detects anything as fully working.
expect_caught() {
    local description="$1" mutator="$2" expected="$3"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && bash "$checker" 2>&1 )"; then
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
# that rejected every mention of a clang version anywhere would "catch" every
# case above while being useless.
expect_accepted() {
    local description="$1" mutator="$2"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && bash "$checker" 2>&1 )"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it should accept:"
        printf '%s\n' "$output" >&2
    fi
}

# -- The unmodified tree must pass -------------------------------------------
make_tree "${scratch}/clean"
if output="$( cd "${scratch}/clean" && bash "$checker" 2>&1 )"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# -- The exact drift morph#457 recorded --------------------------------------
# A document asserting a clang major CI does not pin. This is the fixture the
# issue's acceptance criteria ask for: it goes red, and it names both versions.
expect_caught "a doc asserting CI pins clang 20 while ci.yml pins 22" \
    "printf '%s\n' 'Mutation testing does not run on CI pins clang 20 for its own legs.' \
        >> docs/spec/testing_charter.md" \
    "states 'CI pins clang 20', but .github/workflows/ci.yml pins clang 22"

# The same drift arriving the other way round -- the pin moves and the prose
# stays put. This is how morph#457 actually happened, and it is the direction
# that will happen again when CI bumps to 23.
expect_caught "ci.yml bumped to clang 23 while every doc still says 22" \
    "edit .github/workflows/ci.yml -e 's/^  CLANG_VERSION: \"22\"/  CLANG_VERSION: \"23\"/'" \
    "but .github/workflows/ci.yml pins clang 23"

# A stale assertion in a script comment rather than a doc: the gate reads every
# tracked file, not a curated list of markdown.
expect_caught "a stale assertion in a shell script's comments" \
    "printf '%s\n' '# CI pins clang 19, so the score does not carry over.' >> scripts/mutation.sh" \
    "scripts/mutation.sh"

# -- Rule B: the rewording that would defeat rule A alone ---------------------
# Six sites said the same wrong thing six different ways. A gate matching only
# its own canonical phrasing would pass a tree whose claims had drifted into
# any of the other five.
expect_caught "a CI clang claim in an unrecognised phrasing, even with the right version" \
    "printf '%s\n' 'The CI coverage leg pins clang 22 for its own legs.' \
        >> docs/spec/testing_charter.md" \
    "phrasing this gate cannot check"

expect_caught "a CI clang claim in an unrecognised phrasing with the wrong version" \
    "printf '%s\n' 'Measured on clang 22.1.8 while CI runs clang-20.' \
        >> docs/spec/testing_charter.md" \
    "phrasing this gate cannot check"

# -- Anti-vacuity ------------------------------------------------------------
# The gate must not pass a tree in which it has nothing left to check. Deleting
# the assertions is how a check quietly becomes decorative.
expect_caught "every canonical assertion removed from the tree" \
    "for f in \$(git grep -lF 'CI pins clang' -- . ':!scripts/check_ci_clang_pin.sh' \
                                                 ':!scripts/test_check_ci_clang_pin.sh'); do
         edit \"\$f\" -e 's/CI pins clang [0-9][0-9]*/the pinned compiler/g'
     done" \
    "no 'CI pins clang <N>' assertion found anywhere in the tree"

# The source of truth going missing must fail rather than default to something.
expect_caught "ci.yml with no CLANG_VERSION to read" \
    "edit .github/workflows/ci.yml -e 's/^  CLANG_VERSION: \"22\"/  UNRELATED_VERSION: \"22\"/'" \
    'no `CLANG_VERSION: "<major>"` found'

# -- The false positives the gate must not manufacture -----------------------
# A clang version discussed without invoking CI in the same breath is not this
# gate's business; rejecting it would make the gate unsatisfiable for any
# document that records a local measurement.
expect_accepted "a local toolchain version named with no CI reference on the line" \
    "printf '%s\n' 'Measured on clang 22.1.8 against Mull 0.34.0.' \
        >> docs/spec/testing_charter.md"

# The documented escape hatch, for a dated record of a past state rather than a
# claim about the pin now. Without it, honest history could not be written down.
expect_accepted "a historical record carrying the documented marker" \
    "printf '%s\n' 'Until 2026-07-19 CI pinned clang 20 (ci-clang-pin: historical).' \
        >> docs/spec/testing_charter.md"

# A tracked file naming a clang major with no digits to compare -- every
# ci.yml-shaped \${{ env.CLANG_VERSION }} interpolation in the archived plans
# under docs/superpowers/ is this shape, and flagging them would make the gate
# fire on every workflow snapshot in the tree.
expect_accepted "an interpolated clang version with no literal major" \
    "printf '%s\n' 'CI installs clang-\${{ env.CLANG_VERSION }} from apt.llvm.org.' \
        >> docs/spec/testing_charter.md"

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all CI-clang-pin checker self-tests passed"
