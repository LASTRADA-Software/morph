#!/usr/bin/env bash
# Usage: bash scripts/test_check_rung_filters.sh
#
# Self-test for scripts/check_rung_filters.sh, the gate that keeps the rung
# lists CI cannot generate in step with examples/rungs.txt.
#
# A lint gate nobody tests reports green whether or not it still detects
# anything -- and this particular gate exists because a *filter* that detected
# nothing reported green for two whole rungs (morph#179). So the gate is
# checked in both directions: the unmodified tree must pass, and each
# individual drift it claims to catch is reintroduced into a scratch copy of
# the tree, one at a time, and must be caught.
#
# One mutation at a time matters: applied together, a single detection would
# mask every other.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_rung_filters.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# A scratch copy holding exactly the files the checker reads.
make_tree() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest/scripts" "$dest/.github/workflows" "$dest/examples"
    cp "${repo_root}/scripts/ladder_rungs.sh" "$dest/scripts/"
    cp "${repo_root}/scripts/check_rung_filters.sh" "$dest/scripts/"
    cp "${repo_root}/examples/rungs.txt" "$dest/examples/"
    cp "${repo_root}/.github/workflows/ci.yml" "$dest/.github/workflows/"
    cp "${repo_root}/.github/workflows/wasm-ladder.yml" "$dest/.github/workflows/"
    cp "${repo_root}/codecov.yml" "$dest/"
    local cmakelists rung
    for cmakelists in "${repo_root}"/examples/*/CMakeLists.txt; do
        rung="$(basename "$(dirname "$cmakelists")")"
        mkdir -p "$dest/examples/$rung"
        cp "$cmakelists" "$dest/examples/$rung/"
    done
}

# `sed -i` is not portable between GNU and BSD sed; edit through a temp file.
edit() {
    local file="$1"; shift
    sed "$@" "$file" > "${file}.new"
    mv "${file}.new" "$file"
}

# Each mutation must be caught, and caught *for the stated reason*. `$tree` is
# a fresh copy the mutator edits; `$expected` is a substring the resulting
# diagnostic must contain. Without that third argument a mutation that broke
# the tree in some unrelated way -- a mangled sed, a file the mutator emptied --
# would count as a detection, and this self-test would report a gate that no
# longer detects anything as fully working.
expect_caught() {
    local description="$1" mutator="$2" expected="$3"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$(bash "${tree}/${checker}" "$tree" 2>&1)"; then
        fail "NOT caught: ${description} -- the gate passed a tree it should reject"
        return
    fi
    if printf '%s' "$output" | grep -qF "$expected"; then
        note "caught: ${description}"
    else
        fail "caught for the WRONG reason: ${description} -- no diagnostic containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

# ── The unmodified tree must pass ────────────────────────────────────────────
make_tree "${scratch}/clean"
if output="$(bash "${scratch}/clean/${checker}" "${scratch}/clean" 2>&1)"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# ── Each drift the gate claims to catch ──────────────────────────────────────

# The original defect, in the workflow that still has to spell the list out:
# a rung missing from wasm-ladder.yml's push / pull_request path lists.
expect_caught "a rung missing from wasm-ladder.yml's on.push.paths" \
    "awk '!(/examples\\/lims\\/\\*\\*/ && ++n == 1)' .github/workflows/wasm-ladder.yml > w && mv w .github/workflows/wasm-ladder.yml" \
    "on.push.paths does NOT match examples/lims/gui_wasm/main.cpp"

expect_caught "a rung missing from wasm-ladder.yml's on.pull_request.paths" \
    "awk '!(/examples\\/lims\\/\\*\\*/ && ++n == 2)' .github/workflows/wasm-ladder.yml > w && mv w .github/workflows/wasm-ladder.yml" \
    "on.pull_request.paths does NOT match examples/lims/gui_wasm/main.cpp"

# A rung with no coverage component: reports nothing rather than failing, on
# Codecov's side where we cannot see it.
expect_caught "a rung missing from codecov.yml's components" \
    "edit codecov.yml -e '/^[[:space:]]*-[[:space:]]*component_id:[[:space:]]*lims[[:space:]]*\$/d'" \
    "codecov.yml has no 'component_id: lims'"

# The regression that would make check 1 test the generator instead of the
# workflow: a literal alternation pasted back into ci.yml's filter steps.
expect_caught "ci.yml's filter hand-written again instead of generated" \
    "edit .github/workflows/ci.yml -e 's@pattern=\"\$(bash scripts/ladder_rungs.sh ci-path-regex)\"@pattern=\x27^(examples/(common|pastebin|bookmarks|polls|kanban)/)\x27@'" \
    "hand-written rung alternation"

# Proof that check 1 is behavioural: nothing textual changes about ci.yml here.
# The generator itself is made to drop a rung -- exactly what a stale
# hand-copied list did -- and the gate must still notice.
expect_caught "the generated ci.yml regex silently omitting a rung" \
    "edit scripts/ladder_rungs.sh -e 's@alternation+=\"examples/@[ \"\$rung\" = lims ] || alternation+=\"examples/@'" \
    "ci.yml ladder filter does NOT match examples/lims/src/models/probe.cpp"

# The other direction: a rung directory that declares itself but is absent from
# the authority, so nothing builds, tests or filters on it.
expect_caught "a declared rung missing from examples/rungs.txt" \
    "edit examples/rungs.txt -e '/^lims\$/d'" \
    "declares rung 'lims', which is not listed in examples/rungs.txt"

# Vacuity guard on this script's own YAML reader. If the parser stops finding
# the path lists, every wasm-ladder check would "pass" having compared nothing.
expect_caught "wasm-ladder.yml's path lists becoming unreadable to the parser" \
    "awk '{ if (/^    paths:\$/ && ++n == 1) print \"    paths_renamed:\"; else print }' .github/workflows/wasm-ladder.yml > w && mv w .github/workflows/wasm-ladder.yml" \
    "could not read on.push.paths"

# An empty authority must fail rather than pass every check vacuously.
expect_caught "examples/rungs.txt listing no rungs at all" \
    "edit examples/rungs.txt -e '/^[a-z]/d'" \
    "named no rungs"

# ── Verdict ─────────────────────────────────────────────────────────────────
if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nscripts/check_rung_filters.sh detects every drift it claims to.\n'
