#!/usr/bin/env bash
# Usage: bash scripts/test_check_deprecated_markers.sh
#
# Self-test for scripts/check_deprecated_markers.sh, the CI gate enforcing
# docs/spec/VERSIONING.md's deprecation-window format. A lint gate that is
# never itself tested reports green whether or not it still detects anything,
# so this asserts both directions against the fixtures in
# tests/lint/deprecated_markers/:
#
#   valid/    -- must be accepted (exit 0, no diagnostics)
#   invalid/  -- every file must be rejected, individually
#
# Checking invalid/ one file at a time matters: run as a set, a single
# detection would mask the rest.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_deprecated_markers.sh"
readonly fixtures="${repo_root}/tests/lint/deprecated_markers"

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

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all deprecation-marker checker self-tests passed"
