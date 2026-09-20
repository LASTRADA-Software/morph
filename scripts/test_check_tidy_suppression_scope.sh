#!/usr/bin/env bash
# Usage: bash scripts/test_check_tidy_suppression_scope.sh [CLANG_TIDY_BINARY]
#
# Self-test for scripts/check_tidy_suppression_scope.sh, the gate that keeps
# tests/.clang-tidy's record of its own reach true (morph#632).
#
# A lint gate nobody tests reports green whether or not it still detects
# anything. This one is exposed to that twice over: the record it checks is
# written correct in the same commit that adds the gate, and its behavioural
# half asserts a *negative* -- that a check does not fire -- which is the
# single easiest assertion in this repository to satisfy by accident. A probe
# that failed to compile, a check name misspelt, a clang-tidy invocation that
# analysed nothing: each produces "no finding" and each would score as a pass.
#
# So the drifts the gate claims to catch are reintroduced into a scratch copy,
# one at a time, and must be caught for the stated reason -- including the
# broken-probe case, where the gate must fail rather than report suppression.
#
# Needs a clang-tidy binary. Each case runs the real gate against a scratch
# tree, so the wall clock is a few clang-tidy invocations per case.
set -euo pipefail

readonly tidy="${1:-clang-tidy}"
readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_tidy_suppression_scope.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# The gate reads .clang-tidy, tests/.clang-tidy and its own source; a copy of
# those three paths is the whole tree it needs.
readonly pristine="${scratch}/pristine"
mkdir -p "${pristine}/tests" "${pristine}/scripts"
cp "${repo_root}/.clang-tidy" "${pristine}/.clang-tidy"
cp "${repo_root}/tests/.clang-tidy" "${pristine}/tests/.clang-tidy"
cp "${repo_root}/${checker}" "${pristine}/scripts/"

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

expect_caught() {
    local description="$1" mutator="$2" expected="$3"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && bash "$checker" "$tidy" . 2>&1 )"; then
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

expect_accepted() {
    local description="$1" mutator="$2"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && bash "$checker" "$tidy" . 2>&1 )"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it should accept:"
        printf '%s\n' "$output" >&2
    fi
}

# -- The unmodified tree must pass -------------------------------------------
make_tree "${scratch}/clean"
if output="$( cd "${scratch}/clean" && bash "$checker" "$tidy" . 2>&1 )"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# -- The record going stale --------------------------------------------------
# A fourteenth suppression added without extending the record. This is how the
# note in tests/.clang-tidy stops being true, and it is the drift with the
# highest chance of actually happening.
expect_caught "a suppression added without a header-reach: entry" \
    "edit tests/.clang-tidy -e 's|^  -bugprone-chained-comparison,|  -bugprone-chained-comparison,\n  -performance-unnecessary-value-param,|'" \
    "only in Checks:      performance-unnecessary-value-param"

# The mirror: a record that claims more than the file suppresses is just as
# wrong, and tells a reader a check is unreportable when it is not.
expect_caught "a header-reach: entry for a check that is not suppressed" \
    "edit tests/.clang-tidy -e 's|^# header-reach: bugprone-chained-comparison|# header-reach: bugprone-chained-comparison\n# header-reach: misc-const-correctness|'" \
    "only in header-reach: misc-const-correctness"

# A suppression removed from Checks: but left in the record.
expect_caught "a suppression removed but left in the record" \
    "edit tests/.clang-tidy -e '/^  -readability-identifier-length,$/d'" \
    "only in header-reach: readability-identifier-length"

# -- The gate must not go blind ----------------------------------------------
# If the Checks: block is reworded out from under the parser, the honest
# answer is failure. Both lists would read as empty and match each other --
# the exact shape of "a control that reports success while measuring nothing".
expect_caught "the Checks: block becomes unparseable" \
    "edit tests/.clang-tidy -e 's|^Checks: >|Disabled: >|'" \
    "subtracts no checks at all"

# -- The behavioural half ----------------------------------------------------
# The mechanism reversing: the suppression stops reaching headers. Simulated
# by taking the probed check out of tests/.clang-tidy's Checks: while leaving
# the record intact -- from the probe's point of view that is exactly what
# per-diagnostic-file config resolution would look like. The list mismatch is
# caught too; the assertion below is that the *behavioural* diagnostic fires,
# which is what tells a maintainer the note is now wrong rather than untidy.
expect_caught "the probed check stops being suppressed in headers" \
    "edit tests/.clang-tidy -e '/^  -cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,$/d'" \
    "resolve configuration per diagnostic file"

# The anti-vacuity control, and the reason this self-test exists. Break the
# probe's ability to produce a finding at all -- here by disabling the probed
# checks at the repository root, so they fire from nowhere -- and the gate must
# report a broken probe. A gate that instead said "suppressed in the header"
# would be asserting a negative it never measured.
expect_caught "the probe can no longer fire from anywhere" \
    "edit .clang-tidy -e 's|^  -misc-include-cleaner|  -misc-include-cleaner,\n  -cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,\n  -readability-identifier-length|'" \
    "probe control failed"

# -- False positives ---------------------------------------------------------
# Rewording the prose around the record is not drift. Only the machine-readable
# list and the behaviour are the gate's business.
expect_accepted "the surrounding prose rewritten" \
    "edit tests/.clang-tidy -e 's|^# Measured, not inferred\..*|# Some other wording entirely.|'"

# Adding a suppression *and* its record entry together: the ordinary way the
# file grows. If this failed, the gate would be a tax on maintaining it.
expect_accepted "a suppression added together with its header-reach: entry" \
    "edit tests/.clang-tidy -e 's|^  -bugprone-chained-comparison,|  -bugprone-chained-comparison,\n  -performance-unnecessary-value-param,|' \
        && edit tests/.clang-tidy -e 's|^# header-reach: bugprone-chained-comparison|# header-reach: bugprone-chained-comparison\n# header-reach: performance-unnecessary-value-param|'"

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
