#!/usr/bin/env bash
# Usage: bash scripts/test_check_catch_test_names.sh
#
# Self-test for scripts/check_catch_test_names.sh, the CI gate for issue
# #466's bug class (a Catch2 test-case name that, handed back to the binary as
# catch_discover_tests' positional filter argument, selects something other
# than the test it names -- most damagingly a leading `~`, which makes the
# ctest entry run every OTHER test and report success).
#
# A lint gate that is never itself tested reports green whether or not it still
# detects anything, so this asserts both directions against the fixtures in
# tests/lint/catch_test_names/:
#
#   valid/    -- must be accepted, as a set. This half matters as much as the
#                other: the rejected shapes sit one character away from names
#                that are entirely legitimate (`[tags]`, commas, a mid-name
#                `*`, a non-leading `"` or `~`), and an over-broad gate that
#                failed those would be reverted rather than fixed.
#   invalid/  -- every file must be rejected, individually, AND for the
#                expected reason. Checking the reason is what stops a fixture
#                from passing the self-test while being caught by an unrelated
#                rule; checking one file at a time is what stops a single
#                detection from masking the rest.
#
# It also pins that the accepted set is non-empty: a checker whose parser had
# stopped recognising TEST_CASE altogether would accept valid/ vacuously and
# look identical to a healthy one.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_catch_test_names.sh"
readonly fixtures="${repo_root}/tests/lint/catch_test_names"

# Fixture basename -> a distinctive fragment of the diagnostic it must draw.
# Keeping the expectation here, rather than in the fixture, makes a fixture
# that is rejected for the wrong reason a self-test failure.
declare -A expected_reason=(
    [tilde_leading.cpp]="begins with a tilde"
    [tilde_leading_after_space.cpp]="begins with a tilde"
    [exclude_prefix.cpp]="begins with exclude:"
    [wildcard_leading.cpp]="begins or ends with an asterisk"
    [wildcard_trailing.cpp]="begins or ends with an asterisk"
    [wildcard_trailing_concatenated.cpp]="begins or ends with an asterisk"
    [scenario_trailing_wildcard.cpp]="begins or ends with an asterisk"
    [hyphen_leading.cpp]="begins with a hyphen"
    [quote_leading.cpp]="begins with a double quote"
    [test_case_method_tilde.cpp]="begins with a tilde"
    [non_literal_name.cpp]="is not a plain string literal"
)

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

# ── valid/ must pass, and must not pass vacuously ────────────────────────────
if valid_output="$(bash "$checker" "${fixtures}/valid" 2>&1)"; then
    note "ok: valid fixtures accepted"

    checked="$(printf '%s\n' "$valid_output" \
               | sed -n 's/^checked \([0-9]*\) test-case name(s).*/\1/p')"
    if [ -z "$checked" ]; then
        fail "the checker printed no 'checked N test-case name(s)' line, so a" \
             "vacuous pass cannot be told from a real one"
    elif [ "$checked" -lt 10 ]; then
        fail "the checker reported only ${checked} name(s) in valid/, which has" \
             "more than that -- its parser has stopped recognising registrations," \
             "and it would accept anything"
    else
        note "ok: ${checked} valid names actually parsed (not a vacuous pass)"
    fi
else
    fail "valid fixtures were rejected by the checker:"
    printf '%s\n' "$valid_output" >&2
fi

# ── every invalid/ file must be rejected on its own, for its own reason ──────
shopt -s nullglob
invalid_files=("${fixtures}"/invalid/*.cpp)
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

    if output="$(bash "$checker" "${scratch}/one" 2>&1)"; then
        fail "invalid fixture ${name} was accepted; the checker no longer detects it"
        continue
    fi

    want="${expected_reason[$name]-}"
    if [ -z "$want" ]; then
        fail "invalid fixture ${name} has no expected diagnostic recorded in" \
             "this self-test; add one so a wrong-reason rejection cannot pass"
    elif printf '%s\n' "$output" | grep -qF -- "$want"; then
        note "ok: invalid fixture ${name} rejected (${want})"
    else
        fail "invalid fixture ${name} was rejected, but not for the expected" \
             "reason (${want}); got:"
        printf '%s\n' "$output" >&2
    fi
done

# Every recorded expectation must correspond to a fixture that still exists --
# otherwise a deleted fixture leaves a rule with no case exercising it.
for name in "${!expected_reason[@]}"; do
    if [ ! -f "${fixtures}/invalid/${name}" ]; then
        fail "expected diagnostic recorded for ${name}, but no such fixture exists"
    fi
done

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all Catch2 test-name checker self-tests passed"
