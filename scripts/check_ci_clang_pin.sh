#!/usr/bin/env bash
# Usage: bash scripts/check_ci_clang_pin.sh [REPO_ROOT]
#
# Fails if anything in the tree states a clang major for CI other than the one
# .github/workflows/ci.yml actually pins.
#
# Why this gate exists (morph#457): ci.yml's CLANG_VERSION moved from 20 to 22
# on 2026-07-19 (10ecd064) and six places went on asserting 20 for over a month
# -- docs/spec/testing_charter.md, scripts/mutation.sh, two rows and a
# not_yet_done entry in scripts/mutation_survivors.json, codecov.yml,
# cmake/compiler_options.cmake and scripts/check_branch_coverage.py. The stale
# fact was load-bearing rather than cosmetic: it was the single largest stated
# obstacle to moving the mutation campaign into CI (morph#408), and the
# obstacle did not exist. testing_charter.md is a spec, so a reader resolving
# the question against the authoritative document was told the wrong thing.
#
# Nothing reported it, and nothing would have: a sentence about a compiler
# version compiles, tests and lints exactly as well after the version changes
# as before. It regresses again the moment CI bumps to 23, which is precisely
# how it got here, so it needs a check rather than a habit.
#
# Two rules, and the second is what makes the first hold:
#
#   A. Every canonical assertion `CI pins clang <N>` must name the version
#      ci.yml sets. This is the drift catcher.
#   B. Every *other* line that names a clang major next to a CI reference is
#      rejected as an unrecognised phrasing, whatever version it names. Rule A
#      alone would be defeated by rewording -- and rewording is how six sites
#      came to say the same wrong thing six different ways. Write the claim as
#      `CI pins clang <N>`, or mark the line `ci-clang-pin: historical` if it
#      is a dated record of a past measurement rather than a claim about the
#      pin now.
#
# A CI reference is `CI` as a word, `ci.yml`, or `CLANG_VERSION`; a clang major
# is `clang`/`Clang`/`clang++` followed by a space or hyphen and digits. Both
# have to be on the same line, which is what keeps the rule cheap to satisfy:
# prose that discusses a local toolchain without invoking CI in the same breath
# is not this gate's business.
#
# Finding zero canonical assertions is a failure, not a pass. A gate with
# nothing left to check reports green exactly as loudly as one that checked
# everything, which is the failure mode the whole drift-guard workflow exists
# for.
#
# Scans the tracked files git knows about, minus ci.yml itself (the source of
# truth) and this script and its self-test (which quote both a right and a
# wrong version by construction). Requires git and grep; compiles nothing.
set -euo pipefail

repo_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
readonly repo_root

readonly ci_workflow=".github/workflows/ci.yml"
readonly self="scripts/check_ci_clang_pin.sh"
readonly self_test="scripts/test_check_ci_clang_pin.sh"

# `clang 22`, `Clang-22`, `clang++-22`. Not `CLANG_VERSION` (underscore), and
# not `clang-${{ env.CLANG_VERSION }}` (no literal digits to compare).
readonly clang_major_re='[Cc]lang(\+\+)?[ -][0-9]+'
readonly ci_ref_re='(\bCI\b|ci\.yml|CLANG_VERSION)'
readonly canonical_re='CI pins clang [0-9]+'
readonly historical_marker='ci-clang-pin: historical'

failures=0
canonical_sites=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

cd "$repo_root"

# -- The source of truth -----------------------------------------------------
if [ ! -f "$ci_workflow" ]; then
    printf 'error: %s not found under %s\n' "$ci_workflow" "$repo_root" >&2
    exit 1
fi

pinned="$(sed -nE 's/^[[:space:]]*CLANG_VERSION:[[:space:]]*"?([0-9]+)"?[[:space:]]*$/\1/p' \
    "$ci_workflow" | head -n 1)"

if [ -z "$pinned" ]; then
    printf 'error: no `CLANG_VERSION: "<major>"` found in %s -- this gate reads\n' \
        "$ci_workflow" >&2
    printf '       its expected value from there and cannot check anything without it\n' >&2
    exit 1
fi

note "${ci_workflow} pins clang ${pinned}"

# -- The files to scan -------------------------------------------------------
# git ls-files rather than find: generated trees, build directories and vendored
# _deps checkouts are not this repository's claims to keep honest.
mapfile -t files < <(git ls-files \
    | grep -vFx "$ci_workflow" \
    | grep -vFx "$self" \
    | grep -vFx "$self_test" \
    | { grep -v '^$' || true; })

if [ "${#files[@]}" -eq 0 ]; then
    printf 'error: git ls-files returned nothing under %s\n' "$repo_root" >&2
    exit 1
fi

# -- The scan ----------------------------------------------------------------
# One pass: every line naming a clang major, filtered to those that also name
# CI. -I skips binary files; no match in any file is not an error, hence the
# `|| true` on grep's exit status.
while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    location="${hit%%:*}"
    rest="${hit#*:}"
    lineno="${rest%%:*}"
    text="${rest#*:}"

    case "$text" in
        *"$historical_marker"*)
            note "${location}:${lineno}: marked historical, not checked"
            continue
            ;;
    esac

    if printf '%s' "$text" | grep -qE "$canonical_re"; then
        # Rule A. Every canonical assertion on the line is checked, not just
        # the first -- `head -n 1` would miss a second one on the same line.
        while IFS= read -r stated; do
            canonical_sites=$((canonical_sites + 1))
            if [ "$stated" = "$pinned" ]; then
                note "${location}:${lineno}: states clang ${stated}"
            else
                fail "${location}:${lineno}: states 'CI pins clang ${stated}', but ${ci_workflow} pins clang ${pinned}:
    ${text}"
            fi
        done < <(printf '%s' "$text" | grep -oE "$canonical_re" | grep -oE '[0-9]+')
        continue
    fi

    # Rule B.
    fail "${location}:${lineno}: names a clang major beside a CI reference in a
    phrasing this gate cannot check. Write it as 'CI pins clang ${pinned}', or
    append the marker '${historical_marker}' if it is a dated record rather
    than a claim about the pin now:
    ${text}"
done < <(grep -nHIE "$clang_major_re" -- "${files[@]}" 2>/dev/null \
    | grep -E "$ci_ref_re" || true)

# -- Anti-vacuity ------------------------------------------------------------
if [ "$canonical_sites" -eq 0 ]; then
    fail "no 'CI pins clang <N>' assertion found anywhere in the tree. Either the
    documentation stopped saying which compiler CI uses, or it was reworded out
    of the shape this gate reads -- both leave the gate checking nothing while
    still exiting 0, so it fails instead."
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%s CI-clang-pin check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all ${canonical_sites} CI-clang-pin assertion(s) agree with ${ci_workflow}"
