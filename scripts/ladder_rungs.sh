#!/usr/bin/env bash
# Usage: bash scripts/ladder_rungs.sh [list|ci-path-regex]
#
# Reads examples/rungs.txt — the application ladder's single authoritative
# rung list — and emits it in the form the caller needs.
#
#   list            (default) one rung name per line, in ladder order.
#   ci-path-regex   the extended regular expression that
#                   .github/workflows/ci.yml's `ladder-tests` and
#                   `ladder-sanitizers` jobs match `git diff --name-only`
#                   output against, to decide whether the ladder needs to run.
#
# Why this exists: the rung list used to be hand-copied into every consumer,
# and every copy that was not load-bearing drifted. ci.yml's filter had stopped
# at kanban (rung 4), so a change confined to examples/ledger/ or
# examples/lims/ matched nothing and skipped both ladder jobs — silently, since
# a filter that matches nothing reports success just as loudly as one that
# matches everything (morph#179). Deriving the regex here means it cannot fall
# behind the list again.
#
# Consumers that genuinely cannot call this (GitHub evaluates a workflow's
# `on.*.paths` before any step runs; codecov.yml is read by Codecov, not by us)
# are checked against the list by scripts/check_rung_filters.sh instead.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly rung_file="${repo_root}/examples/rungs.txt"

# Non-rung paths that must also trigger the ladder jobs. Kept here beside the
# rung list rather than inline in the workflow so that both filter steps —
# which must stay identical, since the two jobs build the same tree and differ
# only in instrumentation — cannot diverge.
#
#   examples/common/       the shared testkit/GUI/app libraries every rung links
#   examples/CMakeLists.txt, examples/rungs.txt
#                          which rungs get configured at all
#   include/morph/         the library the ladder exists to exercise
#   src/qt/                the compiled bodies of morph_qt_impl — the very
#                          thing the testkit conformance-tests
#   cmake/, CMakeLists.txt, CMakePresets.json
#                          a change to any can break or silently skip the job
#   .github/workflows/ci.yml, scripts/ladder_rungs.sh
#                          this job's own definition, and this filter itself
#   examples/{LADDER,IMPLEMENTATION,TESTING}.md
#                          the ladder's normative rules
# Not `readonly`: this script has to stay runnable under the bash 3.2 that
# macOS still ships, where `readonly` on an array assignment is a syntax error.
_extra_patterns=(
    'examples/common/'
    'examples/CMakeLists\.txt$'
    'examples/rungs\.txt$'
    'include/morph/'
    'src/qt/'
    'cmake/'
    'CMakeLists\.txt$'
    'CMakePresets\.json$'
    '\.github/workflows/ci\.yml$'
    'scripts/ladder_rungs\.sh$'
    'examples/LADDER\.md'
    'examples/IMPLEMENTATION\.md'
    'examples/TESTING\.md'
)

read_rungs() {
    local line
    local -a rungs=()
    if [ ! -f "$rung_file" ]; then
        printf 'error: %s does not exist\n' "$rung_file" >&2
        return 1
    fi
    # Whole-line comments only, and every other line must be a bare rung name.
    # A malformed line is an error rather than something to skip: silently
    # skipping one would drop a rung from every derived filter, which is the
    # failure this file exists to prevent. Kept byte-identical in rule to
    # examples/CMakeLists.txt's parser so the two can never disagree about
    # what the list contains.
    while IFS= read -r line || [ -n "$line" ]; do
        # Leading/trailing only, matching CMake's string(STRIP).
        line="$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        [ -n "$line" ] || continue
        case "$line" in '#'*) continue ;; esac
        if ! printf '%s' "$line" | grep -qE '^[A-Za-z0-9_]+$'; then
            printf "error: %s: '%s' is not a bare rung name\\n" "$rung_file" "$line" >&2
            return 1
        fi
        rungs+=("$line")
    done < "$rung_file"

    # A reader that silently yields nothing would turn every derived filter
    # into one that matches nothing -- the exact failure this file replaces.
    if [ "${#rungs[@]}" -eq 0 ]; then
        printf 'error: %s named no rungs\n' "$rung_file" >&2
        return 1
    fi
    printf '%s\n' "${rungs[@]}"
}

case "${1:-list}" in
    list)
        read_rungs
        ;;
    ci-path-regex)
        # `while read` rather than `mapfile`: bash 3.2 (macOS) has no mapfile.
        # `read_rungs` is called outside the loop's subshell so its failure
        # still aborts this script under `set -e`.
        rung_names="$(read_rungs)"
        alternation=""
        while IFS= read -r rung; do
            [ -n "$rung" ] || continue
            alternation+="examples/${rung}/|"
        done <<< "$rung_names"
        for pattern in "${_extra_patterns[@]}"; do
            alternation+="${pattern}|"
        done
        printf '^(%s)\n' "${alternation%|}"
        ;;
    *)
        printf 'usage: %s [list|ci-path-regex]\n' "$0" >&2
        exit 2
        ;;
esac
