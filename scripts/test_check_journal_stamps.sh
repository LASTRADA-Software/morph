#!/usr/bin/env bash
# Usage: bash scripts/test_check_journal_stamps.sh
#
# Self-test for scripts/check_journal_stamps.sh, the CI gate enforcing that a
# hand-rolled `morph::journal::LogEntry` stamps its payload fingerprint. A lint
# gate that is never itself tested reports green whether or not it still
# detects anything -- and this particular checker has two ways to go quietly
# blind (a balanced `std::string{}` ending its scope scan early; a *different*
# function's `entry.schema =` vouching for an unstamped one), so both
# directions are asserted against the fixtures in tests/lint/journal_stamps/:
#
#   valid/    -- must be accepted (exit 0, no diagnostics)
#   invalid/  -- every file must be rejected, individually
#
# Checking invalid/ one file at a time matters: run as a set, a single
# detection would mask the rest.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_journal_stamps.sh"
readonly fixtures="${repo_root}/tests/lint/journal_stamps"

failures=0

note() { printf '%s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

# ── valid/ must pass ─────────────────────────────────────────────────────────
if output="$(bash "$checker" "${fixtures}/valid" 2>&1)"; then
    note "ok: valid fixtures accepted"
else
    fail "valid fixtures were rejected by the checker:"
    printf '%s\n' "$output" >&2
fi

# ── every invalid/ file must be rejected on its own ──────────────────────────
shopt -s nullglob
invalid_files=("${fixtures}"/invalid/*.hpp)
shopt -u nullglob

if [ "${#invalid_files[@]}" -eq 0 ]; then
    fail "no fixtures found in ${fixtures}/invalid -- the self-test would pass vacuously"
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

for file in "${invalid_files[@]}"; do
    name="$(basename "$file")"
    # The checker takes directories, so isolate each fixture in its own.
    rm -rf "${scratch:?}/one"
    mkdir -p "${scratch}/one"
    cp "$file" "${scratch}/one/"

    if bash "$checker" "${scratch}/one" >/dev/null 2>&1; then
        fail "invalid fixture ${name} was accepted; the checker no longer detects it"
    else
        note "ok: invalid fixture ${name} rejected"
    fi
done

# ── the tests/ exclusion must not swallow everything ─────────────────────────
# The checker skips any path with a `tests/` component, because a regression
# test for this defect has to be able to construct an unstamped entry. That
# exclusion is one typo away from disabling the gate entirely, so assert it is
# scoped to `tests/` and nothing else: the same rejected fixture, placed under
# a `tests/` directory, must be accepted -- and must still be rejected when it
# is not.
rm -rf "${scratch:?}/exclusion"
mkdir -p "${scratch}/exclusion/tests"
cp "${fixtures}/invalid/unstamped_entry.hpp" "${scratch}/exclusion/tests/"
if bash "$checker" "${scratch}/exclusion" >/dev/null 2>&1; then
    note "ok: a tests/ path is excluded from scanning"
else
    fail "a fixture under tests/ was rejected; the tests/ exclusion no longer applies"
fi

rm -rf "${scratch:?}/exclusion"
mkdir -p "${scratch}/exclusion/src"
cp "${fixtures}/invalid/unstamped_entry.hpp" "${scratch}/exclusion/src/"
if bash "$checker" "${scratch}/exclusion" >/dev/null 2>&1; then
    fail "the same fixture under src/ was also accepted; the exclusion is not scoped to tests/"
else
    note "ok: the exclusion is scoped to tests/, not applied everywhere"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all journal-stamp checker self-tests passed"
