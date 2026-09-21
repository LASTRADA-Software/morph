#!/usr/bin/env bash
# Usage: bash scripts/check_catch2_pin.sh [REPO_ROOT] [--strict]
#
# Two halves, both about the same fact: which Catch2 the clang-tidy-diff job
# analyses against.
#
#   A. Textual. Every line in the tree asserting `CI pins catch2 <V>` must name
#      the version .github/workflows/ci.yml records in CATCH2_VERSION, and any
#      other line naming a Catch2 version beside a CI reference is rejected as
#      a phrasing this gate cannot check.
#   B. Behavioural. The Catch2 headers actually installed here are read and
#      compared against that same pin. Under `--strict` (how the CI job runs
#      it) a mismatch, or no Catch2 at all, fails. Without it -- a workstation
#      run -- a mismatch prints a divergence notice instead, because a
#      workstation is not required to carry the runner's package, only to know
#      that it does not.
#
# Why this gate exists (morph#666): a local `clang-tidy-diff` over the same
# diff, with the same clang-tidy version and the same job flags, can exit 0 on
# a diff the CI job fails -- silently, reporting nothing rather than reporting
# less. The variable is the Catch2 on the include path:
# `readability-function-cognitive-complexity` computes the *same* score under
# both, and what differs is whether `ClangTidyDiagnosticConsumer` classifies
# the finding as user code, which depends on which notes a given Catch2
# release's `TEST_CASE` expansion produces and where it puts them. That is how
# morph#656's branch shipped a NOLINT reason asserting a neighbouring TEST_CASE
# "scores under the threshold" while it scored 87 against a threshold of 25:
# the local gate agreed with it.
#
# What this gate does and does not do, stated plainly, because the distinction
# is the whole point of the ticket:
#
#   * It makes CI's own Catch2 a *decision* rather than an accident. `apt-get
#     install -y catch2` is unpinned; if the runner image's package moves, the
#     clang-tidy job's measurement changes with nothing anywhere saying so.
#     Half B under `--strict` turns that silent move into a failed job.
#   * It does not make a local clang-tidy-diff agree with CI's. Nothing short
#     of the job not depending on the runner's Catch2 at all does that --
#     morph#666's first closing condition, filed separately as a follow-up
#     because it needs a CMake change in a file this change does not own.
#     What half B does on a workstation is report, every time it is run, that
#     the two measurements are not the same one.
#
# Requires git and grep; compiles nothing.
set -euo pipefail

repo_root=""
strict=0
for arg in "$@"; do
    case "$arg" in
        --strict) strict=1 ;;
        *)        repo_root="$arg" ;;
    esac
done
if [ -z "$repo_root" ]; then
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
readonly repo_root strict

readonly ci_workflow=".github/workflows/ci.yml"
readonly self="scripts/check_catch2_pin.sh"
readonly self_test="scripts/test_check_catch2_pin.sh"

# `catch2 3.4.0`, `Catch2-3.4.0`, `Catch2 v3.8.1`. Not `Catch2.git` (no
# separator), and not a bare `3.4.0` with no product name on the line.
readonly catch2_version_re='[Cc]atch2[ -]v?[0-9]+\.[0-9]+(\.[0-9]+)?'
readonly ci_ref_re='(\bCI\b|ci\.yml|CATCH2_VERSION)'
readonly canonical_re='CI pins catch2 [0-9]+\.[0-9]+\.[0-9]+'
readonly historical_marker='catch2-pin: historical'

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

pinned="$(sed -nE 's/^[[:space:]]*CATCH2_VERSION:[[:space:]]*"?([0-9]+\.[0-9]+\.[0-9]+)"?[[:space:]]*$/\1/p' \
    "$ci_workflow" | head -n 1)"

if [ -z "$pinned" ]; then
    printf 'error: no `CATCH2_VERSION: "<x.y.z>"` found in %s -- this gate reads\n' \
        "$ci_workflow" >&2
    printf '       its expected value from there and cannot check anything without it\n' >&2
    exit 1
fi

note "${ci_workflow} pins catch2 ${pinned}"

# -- A. The textual half -----------------------------------------------------
mapfile -t files < <(git ls-files \
    | grep -vFx "$ci_workflow" \
    | grep -vFx "$self" \
    | grep -vFx "$self_test" \
    | { grep -v '^$' || true; })

if [ "${#files[@]}" -eq 0 ]; then
    printf 'error: git ls-files returned nothing under %s\n' "$repo_root" >&2
    exit 1
fi

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
        while IFS= read -r stated; do
            canonical_sites=$((canonical_sites + 1))
            if [ "$stated" = "$pinned" ]; then
                note "${location}:${lineno}: states catch2 ${stated}"
            else
                fail "${location}:${lineno}: states 'CI pins catch2 ${stated}', but ${ci_workflow} pins catch2 ${pinned}:
    ${text}"
            fi
        done < <(printf '%s' "$text" | grep -oE "$canonical_re" \
            | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
        continue
    fi

    fail "${location}:${lineno}: names a Catch2 version beside a CI reference in a
    phrasing this gate cannot check. Write it as 'CI pins catch2 ${pinned}', or
    append the marker '${historical_marker}' if it is a dated record rather
    than a claim about the pin now:
    ${text}"
done < <(grep -nHIE "$catch2_version_re" -- "${files[@]}" 2>/dev/null \
    | grep -E "$ci_ref_re" || true)

if [ "$canonical_sites" -eq 0 ]; then
    fail "no 'CI pins catch2 <x.y.z>' assertion found anywhere in the tree. Either
    the documentation stopped saying which Catch2 the clang-tidy job analyses
    against, or it was reworded out of the shape this gate reads -- both leave
    the gate checking nothing while still exiting 0, so it fails instead."
fi

# -- B. The behavioural half -------------------------------------------------
# Read the version out of the headers that are actually on this machine's
# include path, the same ones clang-tidy would expand TEST_CASE from.
#
# MORPH_CATCH2_INCLUDE_DIR, when set, *replaces* the default search rather than
# preceding it: the self-test needs a run in which no Catch2 is found, and a
# fallback to /usr/include would make that case pass or fail depending on what
# the machine running the self-test happens to have installed.
installed=""
installed_dir=""
if [ -n "${MORPH_CATCH2_INCLUDE_DIR:-}" ]; then
    search_prefixes="${MORPH_CATCH2_INCLUDE_DIR}"
else
    search_prefixes="/usr/include /usr/local/include"
fi
for prefix in $search_prefixes; do
    header="${prefix}/catch2/catch_version_macros.hpp"
    [ -f "$header" ] || continue
    major="$(sed -nE 's/^#define CATCH_VERSION_MAJOR ([0-9]+).*$/\1/p' "$header" | head -n 1)"
    minor="$(sed -nE 's/^#define CATCH_VERSION_MINOR ([0-9]+).*$/\1/p' "$header" | head -n 1)"
    patch="$(sed -nE 's/^#define CATCH_VERSION_PATCH ([0-9]+).*$/\1/p' "$header" | head -n 1)"
    if [ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ]; then
        installed="${major}.${minor}.${patch}"
        installed_dir="$prefix"
        break
    fi
done

if [ -z "$installed" ]; then
    if [ "$strict" -eq 1 ]; then
        fail "no Catch2 headers found on this machine, but --strict says this run *is*
    the clang-tidy job's own environment. The job installs catch2 from apt
    before this step; if that stopped happening, the measurement below this
    step is no longer the one the pin describes."
    else
        note "no Catch2 headers found here -- nothing to compare against the pin"
    fi
elif [ "$installed" = "$pinned" ]; then
    note "installed Catch2 ${installed} (${installed_dir}) matches the pin"
elif [ "$strict" -eq 1 ]; then
    fail "installed Catch2 is ${installed} (${installed_dir}) but ${ci_workflow} pins
    catch2 ${pinned}. The runner image's package moved. Every clang-tidy-diff
    result from this job is now a measurement against ${installed}, and the
    nine examples/*/tests/.clang-tidy comments that explain why a local run
    disagrees with CI describe ${pinned}. Update CATCH2_VERSION and those
    comments together, having checked that the suppressions they argue for
    still apply to ${installed}."
else
    printf '\n' >&2
    printf 'WARNING: this machine'"'"'s Catch2 is %s (%s); CI pins catch2 %s.\n' \
        "$installed" "$installed_dir" "$pinned" >&2
    printf '         A local clang-tidy-diff run is therefore NOT the measurement the\n' >&2
    printf '         clang-tidy-diff job makes. For checks whose evidence lives inside\n' >&2
    printf '         Catch2 macro expansions -- readability-function-cognitive-complexity\n' >&2
    printf '         on a TEST_CASE body is the known one -- it can exit 0 on a diff CI\n' >&2
    printf '         fails, reporting nothing rather than reporting less (morph#666).\n' >&2
    printf '         A green local run is not evidence for those checks. It is still\n' >&2
    printf '         evidence for every check whose finding lands on a line you wrote.\n' >&2
    printf '\n' >&2
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%s catch2-pin check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all ${canonical_sites} catch2-pin assertion(s) agree with ${ci_workflow}"
