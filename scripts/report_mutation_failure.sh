#!/usr/bin/env bash
# Usage: bash scripts/report_mutation_failure.sh SCOPE RUN_URL [LOG...]
#
# Records one failed mutation-campaign run on GitHub: it opens the scope's
# issue if there is not one open, and comments on it if there is. Called from
# .github/workflows/mutation.yml's "Report the failure" step, and separated
# from it so that it can be driven by scripts/test_report_mutation_failure.sh
# against a stub `gh` -- the acceptance condition morph#731 asks for is "force
# two different failures in one scope and confirm both are recorded", and a
# dozen lines of shell inside a `run:` block cannot be driven at all.
#
# ── What this replaces, and why ──────────────────────────────────────────────
#
# The step used to build a fixed title per scope and *skip* when any issue
# carrying that title was open:
#
#     title="Mutation campaign failed or regressed (scope: ${scope})"
#     existing="$(gh issue list --state open --search "in:title \"${title}\"" …)"
#     if [ "$existing" != "0" ]; then
#       echo "An open issue already names this failure; not filing a duplicate."
#       exit 0
#     fi
#
# The title carries the scope and nothing about the failure, so "an open issue
# already names this failure" was false whenever the failure was a different
# one. It happened immediately (morph#731): morph#517 was filed on 2026-09-14
# for a genuine survivor regression, and the 2026-09-21 run -- in which the
# campaign never ran at all, mull's warm-up having timed out (morph#732) --
# printed that line and left no record anywhere outside its own run log.
#
# The dedup itself was right: a hundred identical weekly failures should not be
# a hundred issues. What was missing is the other half, so the suppressed run
# now lands as a comment on the issue that suppressed it. One thread per scope,
# and the thread is the campaign's history.
#
# ── Two details that are not incidental ──────────────────────────────────────
#
# **The title match is exact.** `gh issue list --search 'in:title "…"'` is a
# text search, not an equality test: it matches an issue whose title merely
# contains the phrase, and GitHub's search also tokenises. morph#731 recorded
# that as unverified. Rather than verify it, the reply is filtered here with an
# exact string comparison, so a near-miss title opens a second thread instead of
# commenting on the wrong issue.
#
# **The cause is classified from the logs**, and appears in the comment's first
# line, so the thread reads as a sequence of distinct events rather than a pile
# of run links. The classification is a small fixed table; anything it does not
# recognise is reported as unclassified *with* the log excerpt, never dropped.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: bash scripts/report_mutation_failure.sh SCOPE RUN_URL [LOG...]" >&2
    exit 2
fi

scope="$1"; shift
run_url="$1"; shift
readonly scope run_url

readonly title="Mutation campaign failed or regressed (scope: ${scope})"

# ── The cause, from whichever logs the run got far enough to write ───────────
#
# Ordered from "the campaign never started" outwards, because an earlier
# failure makes every later symptom meaningless: a run whose warm-up timed out
# also has no report for the regression check to read, and reporting the second
# names the consequence rather than the cause -- which is how morph#732 spent
# two weeks looking like morph#517.
cause="unclassified failure"
for log in "$@"; do
    [ -s "$log" ] || continue
    if grep -q "Original test failed (warmup run)" "$log"; then
        cause="the campaign could not run: mull's warm-up run of the unmutated suite failed or timed out"
        break
    fi
    if grep -q "carries no .mull_mutants section" "$log"; then
        cause="the campaign instrumented nothing: the binary carries no .mull_mutants section"
        break
    fi
    if grep -q "wrote no report" "$log"; then
        cause="the campaign errored: mull-runner exited non-zero and wrote no report"
        break
    fi
    if grep -q "0 mutants for scope" "$log"; then
        cause="the campaign produced an empty mutant population"
        break
    fi
    if grep -q "survivors this run, up from a" "$log"; then
        cause="survivors regressed against the recorded baseline"
        break
    fi
done
readonly cause

# ── The evidence ─────────────────────────────────────────────────────────────
#
# The tail of each log the run produced, rather than a paraphrase of it. A
# report whose reader has to open the run to learn what happened is the state
# morph#731 describes.
evidence="$(
    for log in "$@"; do
        if [ ! -e "$log" ]; then
            printf '### `%s`\n\n_not produced by this run._\n\n' "$log"
            continue
        fi
        if [ ! -s "$log" ]; then
            printf '### `%s`\n\n_empty._\n\n' "$log"
            continue
        fi
        printf '### `%s` (last 30 lines)\n\n```\n' "$log"
        tail -n 30 "$log"
        printf '```\n\n'
    done
)"

digest="$(printf '**%s**\n\nRun: %s\n\n%s' "$cause" "$run_url" "$evidence")"

# ── Open, or comment ─────────────────────────────────────────────────────────
#
# `--json number,title`, and the exact comparison is done *here* rather than in
# the `--jq` expression: see the header for why it has to be exact, and
# scripts/test_report_mutation_failure.sh for why it has to be in shell. A
# filter that lives inside jq is a filter only the real `gh` can run, which
# means the near-miss case cannot be tested at all.
#
# A search that *errors* must not be read as "no issue open" -- that would file
# a duplicate every week -- so it is fatal here, and the digest goes to the run
# log on the way out. A report step that fails is a report step that is seen;
# one that swallows the error is how this workflow got here.
listing="$(mktemp)"
trap 'rm -f "$listing" "${body_file:-}"' EXIT
if ! gh issue list --state open --limit 100 \
        --search "in:title \"${title}\"" \
        --json number,title \
        --jq '.[] | "\(.number)\t\(.title)"' > "$listing"; then
    echo "report_mutation_failure.sh: 'gh issue list' failed -- refusing to guess" >&2
    echo "  that no issue is open, which would file a duplicate every week." >&2
    echo "  The failure this run was reporting:" >&2
    printf '%s\n' "$digest" >&2
    exit 1
fi

existing=""
while IFS=$'\t' read -r number found_title; do
    [ -n "${number:-}" ] || continue
    if [ "$found_title" = "$title" ]; then
        existing="$number"
        break
    fi
    echo "ignoring #${number}: its title contains the phrase but is not it -- ${found_title}"
done < "$listing"

body_file="$(mktemp)"

if [ -n "$existing" ]; then
    {
        printf 'The scheduled mutation campaign failed again for scope `%s`.\n\n' "$scope"
        printf '%s\n' "$digest"
        printf '\nFiled here rather than as a new issue: one thread per scope is\n'
        printf 'deliberate (morph#731). If this cause is unrelated to the one this\n'
        printf 'issue was opened for, split it out -- but it is recorded either way,\n'
        printf 'which is what the previous behaviour did not do.\n'
    } > "$body_file"
    gh issue comment "$existing" --body-file "$body_file"
    echo "recorded on existing issue #${existing}: ${cause}"
    exit 0
fi

{
    printf 'The scheduled mutation campaign (.github/workflows/mutation.yml) failed for scope `%s`.\n\n' "$scope"
    printf '%s\n' "$digest"
    printf '\nSee `scripts/check_mutation_regression.py`'"'"'s own doc comment for what this\n'
    printf 'compares, `scripts/mutation_baseline.json` for the recorded baseline, and\n'
    printf '`scripts/mutation_survivors.json` for the triage process for a genuine new\n'
    printf 'survivor.\n\n'
    printf 'While this issue is open, every later failure in this scope is recorded as a\n'
    printf 'comment here rather than as a new issue.\n'
} > "$body_file"
gh issue create --title "$title" --label "area: ci" --body-file "$body_file"
echo "opened a new issue: ${cause}"
