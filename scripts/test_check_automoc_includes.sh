#!/usr/bin/env bash
# Usage: bash scripts/test_check_automoc_includes.sh
#
# Self-test for scripts/check_automoc_includes.sh, the CI gate for moc output
# that includes its header by an ascending relative path (issue #372). A lint
# gate that is never itself tested reports green whether or not it still
# detects anything -- and this one is especially exposed to that, because the
# defect it guards against is invisible in an ordinary checkout: the ascending
# include is present in every build, and only becomes a compile error where the
# directory arithmetic happens to land on a second same-named header.
#
# Asserts all three directions against the fixtures in
# tests/lint/automoc_includes/:
#
#   valid/    -- non-ascending includes, dots inside file names, and Qt's own
#                angle-bracket includes; must pass as a set
#   invalid/  -- each subdirectory holds one ascending shape; each must be
#                rejected on its own
#
# plus the vacuity case, which needs no fixture: a directory holding no moc
# output at all must be rejected rather than reported as clean, since that is
# what an unbuilt or wrongly-pointed build directory looks like.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_automoc_includes.sh"
readonly fixtures="${repo_root}/tests/lint/automoc_includes"

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

# A nonzero exit is not enough on its own. The checker has a second failure
# path -- "no moc-generated sources found" -- and a fixture directory whose
# files stopped matching the checker's find patterns would take it, so the
# fixture would still be "rejected" while the checker no longer looks at that
# kind of file at all. Dropping '*.moc' from the checker's find is exactly
# that: mid_path_climb holds only board.moc, so it would keep reporting
# rejected while every real .moc in a build tree went unscanned. So assert the
# rejection is the ascending-include diagnostic, and that it names a file in
# this fixture directory.
for dir in "${invalid_dirs[@]}"; do
    name="$(basename "$dir")"
    if output="$(bash "$checker" "$dir" 2>&1)"; then
        fail "invalid fixture ${name} was accepted; the checker no longer detects it"
    elif ! printf '%s\n' "$output" | grep -q 'AUTOMOC include lint failed'; then
        fail "invalid fixture ${name} was rejected, but not as an ascending include \
-- the checker failed for some other reason (a vacuous scan, most likely):"
        printf '%s\n' "$output" >&2
    elif ! printf '%s\n' "$output" | grep -qF "$dir"; then
        fail "invalid fixture ${name} was rejected without naming any file under \
${dir}; the diagnostic does not point at the offender:"
        printf '%s\n' "$output" >&2
    else
        note "ok: invalid fixture ${name} rejected"
    fi
done

# ── a directory with no moc output at all must be rejected ──────────────────
# The gate runs against a build tree, so "found nothing" is the one outcome it
# must never call clean: it is indistinguishable from a tree that was never
# built, or a build directory argument that no longer exists.
empty_dir="$(mktemp -d)"
trap 'rm -rf "$empty_dir"' EXIT
if bash "$checker" "$empty_dir" >/dev/null 2>&1; then
    fail "a directory containing no moc output was accepted; the gate can pass vacuously"
else
    note "ok: directory with no moc output rejected"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all AUTOMOC-include checker self-tests passed"
