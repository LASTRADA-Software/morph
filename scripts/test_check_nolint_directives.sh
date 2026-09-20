#!/usr/bin/env bash
# Usage: bash scripts/test_check_nolint_directives.sh
#
# Self-test for scripts/check_nolint_directives.sh, the gate for NOLINTNEXTLINE
# directives that cannot take effect (issue #627).
#
# This gate needs its own test more than most. What it guards against is a
# suppression that reports success while suppressing nothing -- so a gate that
# reports success while detecting nothing would be the same defect one level up,
# and would be invisible for exactly the same reason: nobody looks at a green
# lint, and the thing it failed to catch leaves no other trace.
#
# Asserts all four directions against the fixtures in
# tests/lint/nolint_directives/:
#
#   valid/    -- the two effective shapes (reason above the directive; reason on
#                the directive's line inside a clang-format guard), plus the
#                prose that merely NAMES the directive, which must not be
#                flagged: that sentence is fixed_string.hpp's in-tree warning
#                about this very hazard, and a gate that rejected it would make
#                the documentation unwritable. Must pass as a set.
#   invalid/  -- one subdirectory per inert shape; each must be rejected alone.
#
# plus the vacuity case: a directory holding no directives at all must be
# rejected rather than reported clean, since that is what a scan whose file set
# or directive pattern has drifted looks like.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_nolint_directives.sh"
readonly fixtures="${repo_root}/tests/lint/nolint_directives"

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

# ── valid/ must pass ─────────────────────────────────────────────────────────
if output="$(bash "$checker" "${fixtures}/valid" 2>&1)"; then
    note "ok: valid fixtures accepted"
else
    fail "valid fixtures were rejected by the checker:"
    printf '%s\n' "$output" >&2
fi

# ── every invalid/ subdirectory must be rejected on its own ─────────────────
shopt -s nullglob
invalid_dirs=("${fixtures}"/invalid/*/)
shopt -u nullglob

if [ "${#invalid_dirs[@]}" -eq 0 ]; then
    fail "no fixtures found in ${fixtures}/invalid -- the self-test would pass vacuously"
fi

# A nonzero exit is not enough on its own: the checker's other failure path is
# "no directives found", and a fixture whose files stopped matching the find
# patterns would take it -- so the fixture would still look "rejected" while the
# checker no longer scanned that kind of file at all. Assert the rejection is
# the inert-directive diagnostic and that it names a file under this fixture.
#
# Here-strings rather than `printf ... | grep -q`: `grep -q` exits on first
# match, and under `set -o pipefail` the SIGPIPE that kills the writer becomes
# the pipeline's status -- so a diagnostic long enough to fill the pipe buffer
# would read as *not* matching the pattern it contains.
for dir in "${invalid_dirs[@]}"; do
    name="$(basename "$dir")"
    if output="$(bash "$checker" "$dir" 2>&1)"; then
        fail "invalid fixture ${name} was accepted; the checker no longer detects it"
    elif ! grep -q 'NOLINT directive lint failed' <<<"$output"; then
        fail "invalid fixture ${name} was rejected, but not as an inert directive \
-- the checker failed for some other reason (a vacuous scan, most likely):"
        printf '%s\n' "$output" >&2
    elif ! grep -qF "$dir" <<<"$output"; then
        fail "invalid fixture ${name} was rejected without naming any file under \
${dir}; the diagnostic does not point at the offender:"
        printf '%s\n' "$output" >&2
    else
        note "ok: invalid fixture ${name} rejected"
    fi
done

# ── a directory with no directives at all must be rejected ──────────────────
# "Found nothing" is the one outcome this gate must never call clean: it is what
# a broken directive pattern, a narrowed file-extension list, or a wrong path
# argument all look like from the outside.
empty_dir="$(mktemp -d)"
trap 'rm -rf "$empty_dir"' EXIT
printf 'int main() { return 0; }\n' > "${empty_dir}/plain.cpp"
if bash "$checker" "$empty_dir" >/dev/null 2>&1; then
    fail "a directory containing no NOLINTNEXTLINE directives was accepted; the gate can pass vacuously"
else
    note "ok: directory with no directives rejected"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all NOLINT-directive checker self-tests passed"
