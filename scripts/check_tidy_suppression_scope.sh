#!/usr/bin/env bash
# Usage: bash scripts/check_tidy_suppression_scope.sh [CLANG_TIDY_BINARY] [REPO_ROOT]
#
# Keeps tests/.clang-tidy's record of its own reach true.
#
# Why this gate exists (morph#632): clang-tidy resolves its configuration from
# the path of the translation unit it is analysing, not from the path of the
# file a diagnostic lands in. tests/.clang-tidy's thirteen suppressions are
# each argued as *test idiom* -- Catch2's REQUIRE expansion, a raw-syscall
# harness -- and that argument is correct for test sources and says nothing
# about include/morph/**. But the suppressions apply there too, whenever the
# header is reached from a TU under tests/. A finding's visibility therefore
# depends on which TU happened to reach it, and a header gate driven by test
# TUs is green on findings it was built to catch.
#
# The reach was measured rather than assumed: over the 134 tests/ TUs in the
# clang-tidy job's own compile database, distinct findings inside
# include/morph/** went 262 with tests/.clang-tidy present to 595 with it
# removed -- 333 hidden across 25 headers. The numbers and the per-check split
# are recorded in tests/.clang-tidy's own header.
#
# A record nothing reads back is the thing this repository distrusts, so this
# gate checks both halves of that record:
#
#   A. Behaviourally, that the reach still exists. A probe header under
#      include/morph/ is compiled from a TU under tests/ and from a TU that is
#      not, using this repository's *real* .clang-tidy files, and the finding
#      must be absent from the first and present from the second. Behavioural
#      rather than a grep for the mechanism, for the reason morph#298
#      established -- and this direction matters twice over: if clang-tidy ever
#      resolves configuration per diagnostic file, the note goes red rather
#      than quietly stale.
#
#      The second probe is the anti-vacuity control. Without it, assertion A
#      would also pass against a broken probe that produces no findings at all,
#      which is exactly the failure this whole issue is about.
#
#   B. Textually, that the `header-reach:` list in tests/.clang-tidy's header
#      still names exactly the checks its `Checks:` key disables. Adding a
#      fourteenth suppression without extending the list is the way the record
#      goes stale, and it is the only way a reader finds out which checks a
#      test-TU-driven gate cannot report.
#
# Finding zero suppressions in tests/.clang-tidy is a failure, not a pass: a
# gate with nothing left to check reports green exactly as loudly as one that
# checked everything.
#
# Needs a clang-tidy binary; compiles nothing and reads no compile database.
set -euo pipefail

readonly tidy="${1:-clang-tidy}"
readonly repo_root="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

readonly tests_config="${repo_root}/tests/.clang-tidy"
readonly root_config="${repo_root}/.clang-tidy"

# The two checks the probe drives. Both are in tests/.clang-tidy's list, both
# are cheap to trigger from a header, and together they are 304 of the 333
# findings measured as hidden.
readonly probe_checks=(
    "cppcoreguidelines-pro-bounds-avoid-unchecked-container-access"
    "readability-identifier-length"
)

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

for f in "$tests_config" "$root_config"; do
    if [ ! -f "$f" ]; then
        printf 'error: %s not found\n' "$f" >&2
        exit 1
    fi
done

if ! command -v "$tidy" > /dev/null 2>&1; then
    printf 'error: clang-tidy binary %s not found on PATH\n' "$tidy" >&2
    exit 1
fi

# -- The two lists -----------------------------------------------------------
# Everything `Checks:` subtracts, one per line, sorted. The folded block ends
# at the next unindented key.
disabled="$(awk '
    /^Checks:/        { in_block = 1; next }
    in_block && /^[^[:space:]]/ { in_block = 0 }
    in_block          { print }
' "$tests_config" \
    | tr -d ' ' | tr ',' '\n' \
    | sed -n 's/^-\([A-Za-z0-9.-]\+\)$/\1/p' | sort -u)"

# Everything the header comment records as unreportable in a header.
recorded="$(sed -n 's/^#[[:space:]]*header-reach:[[:space:]]*\([A-Za-z0-9.-]\+\)[[:space:]]*$/\1/p' \
    "$tests_config" | sort -u)"

disabled_n="$(printf '%s' "$disabled" | grep -c . || true)"
recorded_n="$(printf '%s' "$recorded" | grep -c . || true)"

if [ "$disabled_n" -eq 0 ]; then
    fail "tests/.clang-tidy subtracts no checks at all -- either the file stopped
    suppressing anything (in which case delete it and this gate), or its
    \`Checks:\` block was reworded out of the shape this gate reads. Both leave
    the gate checking nothing while still exiting 0."
else
    note "tests/.clang-tidy subtracts ${disabled_n} check(s)"
fi

if [ "$disabled" != "$recorded" ]; then
    fail "tests/.clang-tidy's \`header-reach:\` list does not match what its
    \`Checks:\` key subtracts. Every suppression there is also off for every
    include/morph/** header reached from a TU under tests/ (morph#632), and the
    list is the only place a reader is told which ones. Differences:
$(diff <(printf '%s\n' "$disabled") <(printf '%s\n' "$recorded") \
        | sed 's/^< /    only in Checks:      /; s/^> /    only in header-reach: /' \
        | grep -E '^    only' || true)"
else
    note "the \`header-reach:\` list names all ${recorded_n} subtracted check(s)"
fi

# -- The behavioural probe ---------------------------------------------------
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

mkdir -p "${scratch}/include/morph" "${scratch}/tests" "${scratch}/src"
cp "$root_config" "${scratch}/.clang-tidy"
cp "$tests_config" "${scratch}/tests/.clang-tidy"

# One finding per probed check, both inside the header, both in ordinary
# non-template inline functions so no instantiation is needed to reach them.
cat > "${scratch}/include/morph/probe_scope.hpp" <<'HPP'
#pragma once
#include <vector>

inline int probeUncheckedContainerAccess(const std::vector<int>& values)
{
    return values[0];
}

inline int probeShortIdentifier(const std::vector<int>& values)
{
    const int ab = static_cast<int>(values.size());
    return ab;
}
HPP

cat > "${scratch}/tests/probe_scope_tu.cpp" <<'CPP'
#include "../include/morph/probe_scope.hpp"

int probeScopeFromTests(const std::vector<int>& values)
{
    return probeUncheckedContainerAccess(values) + probeShortIdentifier(values);
}
CPP
cp "${scratch}/tests/probe_scope_tu.cpp" "${scratch}/src/probe_scope_tu.cpp"

run_probe() {
    # clang-tidy exits non-zero on findings (WarningsAsErrors: "*"), which is
    # the normal case here, so its status is deliberately ignored: what the
    # assertions read is which check names appear against the probe header.
    ( cd "$scratch" && "$tidy" --quiet "$1" -- -std=c++23 2>&1 ) || true
}

from_tests="$(run_probe tests/probe_scope_tu.cpp)"
from_src="$(run_probe src/probe_scope_tu.cpp)"

for check in "${probe_checks[@]}"; do
    # The control first: if the probe does not fire from a TU outside tests/,
    # nothing below means anything.
    if printf '%s' "$from_src" | grep -q "probe_scope\.hpp.*\[${check}"; then
        note "probe: ${check} is reported in the header from src/probe_scope_tu.cpp"
    else
        fail "probe control failed: ${check} was NOT reported against
    include/morph/probe_scope.hpp from a TU outside tests/. The probe, not the
    repository, is broken -- and until it is fixed the assertion below proves
    nothing. Full output:
$(printf '%s' "$from_src" | sed 's/^/    /')"
        continue
    fi

    if printf '%s' "$from_tests" | grep -q "probe_scope\.hpp.*\[${check}"; then
        fail "${check} IS now reported against include/morph/probe_scope.hpp from a
    TU under tests/, where tests/.clang-tidy subtracts it. clang-tidy appears to
    resolve configuration per diagnostic file rather than per translation unit,
    which is the opposite of what morph#632 measured and of what the note in
    tests/.clang-tidy tells readers. Re-measure the reach, correct or delete
    that note, and revisit any header gate built on the old behaviour."
    else
        note "probe: ${check} is suppressed in the header from tests/probe_scope_tu.cpp"
    fi
done

if [ "$failures" -ne 0 ]; then
    printf '\n%s clang-tidy suppression-scope check(s) failed\n' "$failures" >&2
    exit 1
fi

printf '\nok: tests/.clang-tidy records the reach it actually has\n'
