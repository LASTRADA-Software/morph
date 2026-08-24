#!/usr/bin/env bash
# Usage: bash scripts/check_rung_filters.sh [REPO_ROOT]
#
# Fails if any application-ladder rung named in examples/rungs.txt is missing
# from a consumer that cannot read that file for itself.
#
# Most consumers of the rung list now derive it at run time
# (scripts/ladder_rungs.sh), so they cannot fall behind. Two cannot:
#
#   .github/workflows/wasm-ladder.yml  `on.push.paths` / `on.pull_request.paths`
#       GitHub evaluates these to decide whether to start the workflow, before
#       any step of it exists to run a script.
#   codecov.yml
#       Read by Codecov's own service, not by anything in this repository.
#
# Both are therefore checked from the outside, here.
#
# Why this gate exists at all (morph#179): ci.yml's ladder path filter was a
# hand-copied rung alternation that stopped at kanban, so a change confined to
# examples/ledger/ (rung 5) or examples/lims/ (rung 6) matched nothing and
# skipped both ladder jobs -- including `ladder-sanitizers`, the only job in
# the repository that sanitizer-instruments a rung. Nothing reported this: a
# path filter that matches nothing succeeds exactly as loudly as one that
# correctly found nothing to do. The same list had already drifted the same way
# in scripts/coverage.sh and codecov.yml (morph#141).
#
# The checks below are behavioural, not textual, wherever the artefact has
# semantics we can reproduce: a rung passes the ci.yml check only if a path
# under its directory actually matches the generated regex, and passes the
# wasm-ladder check only if such a path actually matches one of the globs
# parsed out of the workflow. A grep for the rung's name would pass on a
# filter that had been rewritten into something that matched nothing.
set -euo pipefail

repo_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
readonly repo_root

readonly rung_reader="${repo_root}/scripts/ladder_rungs.sh"
readonly ci_workflow="${repo_root}/.github/workflows/ci.yml"
readonly wasm_workflow="${repo_root}/.github/workflows/wasm-ladder.yml"
readonly codecov_config="${repo_root}/codecov.yml"
readonly examples_dir="${repo_root}/examples"

failures=0
checks=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

# ── Translate a GitHub Actions path glob into an ERE ─────────────────────────
# `**` matches any characters including `/`; `*` matches any run of characters
# other than `/`; `?` matches one such character. Everything else is literal.
glob_to_regex() {
    local glob="$1"
    local out="" i=0 n="${#glob}" ch
    while [ "$i" -lt "$n" ]; do
        ch="${glob:i:1}"
        case "$ch" in
            '*')
                if [ "${glob:i:2}" = '**' ]; then
                    out="${out}.*"
                    i=$((i + 1))
                else
                    out="${out}[^/]*"
                fi
                ;;
            '?')   out="${out}[^/]" ;;
            '.'|'+'|'('|')'|'['|']'|'{'|'}'|'^'|'$'|'|'|'\\')
                   out="${out}\\${ch}" ;;
            *)     out="${out}${ch}" ;;
        esac
        i=$((i + 1))
    done
    printf '^%s$' "$out"
}

# ── Extract one `on.<event>.paths` list from a workflow ──────────────────────
# A hand-rolled reader rather than a YAML library: this must run on a bare
# runner with nothing installed, and the shape it reads is two levels deep and
# fully literal. It prints one glob per line.
workflow_paths() {
    local file="$1" event="$2"
    awk -v want="$event" '
        # A top-level key ends whatever section we were in.
        /^[^[:space:]#]/ { section = ""; inpaths = 0 }
        /^  [a-z_]+:[[:space:]]*$/ {
            section = $1; sub(/:$/, "", section); inpaths = 0; next
        }
        section == want && /^    paths:[[:space:]]*$/ { inpaths = 1; next }
        section == want && inpaths && /^      - / {
            line = $0
            sub(/^      - /, "", line)
            gsub(/^'"'"'|'"'"'$/, "", line)
            gsub(/^"|"$/, "", line)
            print line
            next
        }
        section == want && inpaths && /^    [^[:space:]]/ { inpaths = 0 }
    ' "$file"
}

# ── The authoritative list ───────────────────────────────────────────────────
if ! rungs="$(bash "$rung_reader" list)"; then
    printf 'error: %s could not read the rung list\n' "$rung_reader" >&2
    exit 1
fi

rung_count="$(printf '%s\n' "$rungs" | grep -c '[^[:space:]]' || true)"
if [ "$rung_count" -eq 0 ]; then
    fail "examples/rungs.txt named no rungs -- every check below would pass vacuously"
    exit 1
fi
note "examples/rungs.txt names ${rung_count} rung(s): $(printf '%s' "$rungs" | tr '\n' ' ')"

# ── 1. ci.yml's generated filter must match every rung ───────────────────────
# Behavioural: build the regex the workflow builds, and match a real path under
# each rung against it.
ci_regex="$(bash "$rung_reader" ci-path-regex)"
while IFS= read -r rung; do
    [ -n "$rung" ] || continue
    probe="examples/${rung}/src/models/probe.cpp"
    checks=$((checks + 1))
    if printf '%s\n' "$probe" | grep -qE "$ci_regex"; then
        note "ci.yml ladder filter matches ${probe}"
    else
        fail "ci.yml ladder filter does NOT match ${probe} -- rung '${rung}' would skip ladder-tests and ladder-sanitizers"
    fi
done <<< "$rungs"

# ci.yml must actually use that generated pattern. Without this, someone could
# paste a literal alternation back into either filter step and every check
# above would still pass -- it would be testing the generator, not the
# workflow. Both ladder jobs' filter steps invoke the reader; a third
# invocation is fine, none or one is not.
uses="$(grep -c 'ladder_rungs\.sh ci-path-regex' "$ci_workflow" || true)"
checks=$((checks + 1))
if [ "$uses" -ge 2 ]; then
    note "ci.yml derives its ladder path filter from the rung list (${uses} call sites)"
else
    fail "ci.yml invokes 'ladder_rungs.sh ci-path-regex' ${uses} time(s); both ladder-tests and ladder-sanitizers must derive their filter from examples/rungs.txt rather than restating it"
fi

# Belt and braces: no hand-written rung alternation may survive anywhere in the
# workflow. This is the exact text shape that drifted.
checks=$((checks + 1))
if grep -qE 'examples/\(common\|' "$ci_workflow"; then
    fail "$ci_workflow still contains a hand-written rung alternation ('examples/(common|...'); it must be generated from examples/rungs.txt"
else
    note "ci.yml contains no hand-written rung alternation"
fi

# ── 2. wasm-ladder.yml's literal path lists must cover every rung ────────────
for event in push pull_request; do
    globs="$(workflow_paths "$wasm_workflow" "$event")"
    if [ -z "$globs" ]; then
        fail "could not read on.${event}.paths from $wasm_workflow -- the parser found no entries, so the checks below would pass vacuously"
        continue
    fi
    while IFS= read -r rung; do
        [ -n "$rung" ] || continue
        probe="examples/${rung}/gui_wasm/main.cpp"
        matched=0
        while IFS= read -r glob; do
            [ -n "$glob" ] || continue
            if printf '%s\n' "$probe" | grep -qE "$(glob_to_regex "$glob")"; then
                matched=1
                break
            fi
        done <<< "$globs"
        checks=$((checks + 1))
        if [ "$matched" -eq 1 ]; then
            note "wasm-ladder.yml on.${event}.paths matches ${probe}"
        else
            fail "wasm-ladder.yml on.${event}.paths does NOT match ${probe} -- add \"- 'examples/${rung}/**'\" to that list"
        fi
    done <<< "$rungs"
done

# ── 3. codecov.yml must declare a component per rung ─────────────────────────
# Textual, unavoidably: Codecov's component matching happens on their service.
# A missing component does not fail anything there either -- it reports nothing,
# which is how ledger's carefully targeted component came to score a file set
# the uploaded report did not contain (morph#141).
while IFS= read -r rung; do
    [ -n "$rung" ] || continue
    checks=$((checks + 1))
    if grep -qE "^[[:space:]]*-[[:space:]]*component_id:[[:space:]]*${rung}[[:space:]]*$" "$codecov_config"; then
        note "codecov.yml declares a component for ${rung}"
    else
        fail "codecov.yml has no 'component_id: ${rung}' -- rung '${rung}' is absent from the coverage components"
    fi
done <<< "$rungs"

# ── 4. Every declared rung must be in the list ───────────────────────────────
# The other direction. examples/CMakeLists.txt enforces this too, and fails the
# configure -- but that only bites once someone configures the ladder, which no
# lint-only CI leg does. Checked here so it fails on the PR that introduces it.
shopt -s nullglob
for cmakelists in "${examples_dir}"/*/CMakeLists.txt; do
    declared="$(sed -nE 's/^morph_add_rung\(NAME[[:space:]]+([A-Za-z0-9_]+).*/\1/p' "$cmakelists" | head -1)"
    [ -n "$declared" ] || continue
    checks=$((checks + 1))
    if printf '%s\n' "$rungs" | grep -qxF "$declared"; then
        note "${declared} is declared by ${cmakelists#"${repo_root}/"} and listed in examples/rungs.txt"
    else
        fail "${cmakelists#"${repo_root}/"} declares rung '${declared}', which is not listed in examples/rungs.txt -- nothing would build, test or filter on it"
    fi
done
shopt -u nullglob

# ── Verdict ─────────────────────────────────────────────────────────────────
if [ "$checks" -eq 0 ]; then
    printf 'error: this gate ran no checks at all; that is a failure, not a pass\n' >&2
    exit 1
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d of %d rung-filter check(s) failed.\n' "$failures" "$checks" >&2
    exit 1
fi

printf '\nAll %d rung-filter checks passed.\n' "$checks"
