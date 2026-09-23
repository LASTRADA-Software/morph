#!/usr/bin/env bash
# Usage: bash scripts/test_report_mutation_failure.sh
#
# Self-test for scripts/report_mutation_failure.sh -- morph#731's acceptance
# condition, executed rather than argued: **force two different failures in one
# scope and confirm both are recorded.**
#
# The failure it guards against is specific. The reporting step used to skip
# when any open issue carried the scope's title, so the second failure in a
# scope produced nothing anywhere: no issue, no comment, no label, only a line
# in a run log nobody reads. That is not a state a workflow run can be asked
# about afterwards -- it is the *absence* of a record -- so the only way to
# check it is to drive the reporter against a `gh` whose calls can be counted.
#
# So `gh` here is a stub on PATH. It keeps its issues in a directory, answers
# `issue list` from it, and appends `issue create` / `issue comment` calls to a
# transcript. The cases below assert on that transcript.
#
# The two failures are the real ones from the two runs morph#731 cites:
#
#   * run 34836153375 (2026-09-14): a survivor regression, 203 up from 199 --
#     which opened morph#517;
#   * run 35592789912 (2026-09-21): the campaign never ran, mull's warm-up
#     timing out (morph#732) -- which was recorded nowhere.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly reporter="${repo_root}/scripts/report_mutation_failure.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# ── The stub `gh` ────────────────────────────────────────────────────────────
#
# Deliberately not a mock of the whole CLI: it implements the three calls the
# reporter makes and refuses anything else loudly, so a reporter that starts
# calling something new fails this test rather than silently passing it.
mkdir -p "${scratch}/bin"
cat > "${scratch}/bin/gh" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail
state="${GH_STUB_STATE:?}"
mkdir -p "${state}/issues"
transcript="${state}/transcript"

subject="${1:-}"; verb="${2:-}"; shift 2 || true

title=""; body_file=""; number=""; search=""
case "$verb" in
    comment) number="${1:-}"; shift || true ;;
esac
while [ "$#" -gt 0 ]; do
    case "$1" in
        --title) title="$2"; shift 2 ;;
        --body-file) body_file="$2"; shift 2 ;;
        --search) search="$2"; shift 2 ;;
        --state|--limit|--json|--jq|--label) shift 2 ;;
        *) shift ;;
    esac
done

if [ "$subject" != "issue" ]; then
    echo "gh stub: unexpected subject '${subject}'" >&2; exit 64
fi

case "$verb" in
    list)
        # Emits `<number>\t<title>` per matching issue -- what real `gh` plus
        # the reporter's `--jq '.[] | "\(.number)\t\(.title)"'` produces.
        #
        # The match is a *substring* one, deliberately: `in:title "…"` is a
        # text search, not an equality test, so an issue whose title merely
        # contains the phrase comes back. Answering only exact matches here
        # would hide the case the reporter's own filter exists for.
        phrase="${search#in:title \"}"; phrase="${phrase%\"}"
        for f in "${state}/issues"/*; do
            [ -e "$f" ] || continue
            recorded="$(head -n 1 "$f")"
            case "$recorded" in
                *"$phrase"*) printf '%s\t%s\n' "$(basename "$f")" "$recorded" ;;
            esac
        done
        exit 0
        ;;
    create)
        n=$(( $(ls "${state}/issues" | wc -l) + 900 ))
        { printf '%s\n' "$title"; cat "$body_file"; } > "${state}/issues/${n}"
        printf 'create %s\n' "$n" >> "$transcript"
        cp "$body_file" "${state}/body-create-${n}"
        echo "https://github.com/LASTRADA-Software/morph/issues/${n}"
        ;;
    comment)
        if [ ! -e "${state}/issues/${number}" ]; then
            echo "gh stub: comment on a nonexistent issue ${number}" >&2; exit 65
        fi
        printf 'comment %s\n' "$number" >> "$transcript"
        cat "$body_file" >> "${state}/issues/${number}"
        cp "$body_file" "${state}/body-comment-${number}-$(date +%s%N)"
        ;;
    *)
        echo "gh stub: unexpected verb '${verb}'" >&2; exit 64
        ;;
esac
STUB
chmod +x "${scratch}/bin/gh"
export PATH="${scratch}/bin:${PATH}"

# ── The two failures, verbatim from the runs morph#731 cites ─────────────────
mkdir -p "${scratch}/logs"

cat > "${scratch}/logs/warmup-timeout.log" <<'LOG'
[info] Warm up run (threads: 1)
       [################################] 1/1. Finished in 1m0.0s
[error] Original test failed (warmup run)
status: Timedout
stdout: ''
stderr: ''
[error] Error messages are treated as fatal errors. Exiting now.
scripts/mutation.sh: mull-runner exited 1 and wrote no
  report at build/mutation-core-forms/mutation-core-forms.txt.
LOG

cat > "${scratch}/logs/regression.log" <<'LOG'
error: regression for scope 'core-forms': 203 survivors this run, up from a
recorded baseline of 199 (784 mutants this run vs 773 baseline). A new survivor
means the suite stopped noticing a mutation it used to catch (or would have).
LOG

run_reporter() {
    local state="$1"; shift
    GH_STUB_STATE="$state" bash "$reporter" "$@"
}

transcript_of() { cat "${1}/transcript" 2>/dev/null || true; }

# ── Case 1: two different failures in one scope, both recorded ───────────────
state="${scratch}/case1"
mkdir -p "${state}/issues"

if ! out1="$(run_reporter "$state" core-forms https://example/run/1 \
        "${scratch}/logs/regression.log" 2>&1)"; then
    fail "the first failure was not reported at all: ${out1}"
fi

if ! out2="$(run_reporter "$state" core-forms https://example/run/2 \
        "${scratch}/logs/warmup-timeout.log" 2>&1)"; then
    fail "the second failure was not reported at all: ${out2}"
fi

transcript="$(transcript_of "$state")"
expected="$(printf 'create 900\ncomment 900')"
if [ "$transcript" = "$expected" ]; then
    note "two different failures in one scope: the first opens the issue, the second comments on it"
else
    fail "both failures must be recorded. Transcript was:
${transcript}
expected:
${expected}"
fi

# Recorded is not enough: the second record has to carry the *second* failure's
# evidence. A comment that repeated the first would be the same silence with
# extra steps.
comment_body="$(cat "${state}"/body-comment-900-* 2>/dev/null || true)"
if printf '%s' "$comment_body" | grep -q "Original test failed (warmup run)"; then
    note "the comment carries the second failure's own log, not the first's"
else
    fail "the comment does not quote the warm-up timeout it was reporting:
${comment_body}"
fi
if printf '%s' "$comment_body" | grep -q "warm-up run of the unmutated suite"; then
    note "the comment names the cause it classified"
else
    fail "the comment does not name the classified cause:
${comment_body}"
fi
if printf '%s' "$comment_body" | grep -q "https://example/run/2"; then
    note "the comment names the run it came from"
else
    fail "the comment does not name run 2"
fi

# ── Case 2: the regression's own evidence in the opening issue ───────────────
create_body="$(cat "${state}/body-create-900")"
if printf '%s' "$create_body" | grep -q "203 survivors this run"; then
    note "the opening issue quotes the regression rather than paraphrasing it"
else
    fail "the opening issue does not quote the regression:
${create_body}"
fi

# ── Case 3: a near-miss title must not be commented on ──────────────────────
#
# `gh issue list --search 'in:title "…"'` is a text search, not equality --
# morph#731 filed that as unverified. The reporter filters for an exact title,
# so an issue whose title merely *contains* the phrase gets a new thread rather
# than someone else's.
state="${scratch}/case3"
mkdir -p "${state}/issues"
printf 'Mutation campaign failed or regressed (scope: core-forms) -- triage notes\n' \
    > "${state}/issues/870"
run_reporter "$state" core-forms https://example/run/3 \
    "${scratch}/logs/regression.log" > /dev/null
transcript="$(transcript_of "$state")"
if [ "$transcript" = "create 901" ]; then
    note "an issue whose title merely contains the phrase is not commented on"
else
    fail "a near-miss title was treated as the scope's issue. Transcript was:
${transcript}"
fi

# ── Case 4: two scopes are two threads ──────────────────────────────────────
state="${scratch}/case4"
mkdir -p "${state}/issues"
run_reporter "$state" core-forms https://example/run/4 "${scratch}/logs/regression.log" > /dev/null
run_reporter "$state" net        https://example/run/5 "${scratch}/logs/regression.log" > /dev/null
transcript="$(transcript_of "$state")"
if [ "$transcript" = "$(printf 'create 900\ncreate 901')" ]; then
    note "a failure in a different scope opens its own thread"
else
    fail "scopes were not kept apart. Transcript was:
${transcript}"
fi

# ── Case 5: an unclassifiable failure is still reported ─────────────────────
#
# The classification table is a convenience. A failure it does not recognise
# must be reported *with* its log, never dropped -- dropping it is the defect
# this script exists for, one level down.
state="${scratch}/case5"
mkdir -p "${state}/issues"
printf 'ninja: build stopped: subcommand failed.\n' > "${scratch}/logs/odd.log"
run_reporter "$state" core-forms https://example/run/6 "${scratch}/logs/odd.log" > /dev/null
body="$(cat "${state}/body-create-900")"
if printf '%s' "$body" | grep -q "unclassified failure" \
        && printf '%s' "$body" | grep -q "ninja: build stopped"; then
    note "an unrecognised failure is reported as unclassified, with its log"
else
    fail "an unrecognised failure lost its evidence:
${body}"
fi

# ── Case 6: a log the run never produced ────────────────────────────────────
#
# The campaign failing before the regression check means regression-<scope>.log
# does not exist. The reporter must still report, and must say which log is
# missing rather than exiting non-zero on the `tail`.
state="${scratch}/case6"
mkdir -p "${state}/issues"
if ! run_reporter "$state" core-forms https://example/run/7 \
        "${scratch}/logs/warmup-timeout.log" "${scratch}/logs/does-not-exist.log" > /dev/null 2>&1; then
    fail "a missing log file made the reporter fail instead of report"
else
    body="$(cat "${state}/body-create-900")"
    if printf '%s' "$body" | grep -q "not produced by this run"; then
        note "a log the run never wrote is named as missing, and the report still lands"
    else
        fail "the missing log was not named:
${body}"
    fi
fi

# ── Case 7: a failing search is fatal, not a silent duplicate ───────────────
#
# If `gh issue list` errors, reading that as "no issue open" files a duplicate
# every week. The reporter must fail instead -- and must put the digest in the
# run log on its way out, so the failure is not lost with it.
state="${scratch}/case7"
mkdir -p "${state}/issues"
cat > "${scratch}/bin/gh" <<'STUB'
#!/usr/bin/env bash
echo "gh: could not connect to api.github.com" >&2
exit 1
STUB
chmod +x "${scratch}/bin/gh"
if out="$(run_reporter "$state" core-forms https://example/run/8 \
        "${scratch}/logs/warmup-timeout.log" 2>&1)"; then
    fail "a failing search was treated as 'no issue open'"
elif printf '%s' "$out" | grep -q "Original test failed (warmup run)"; then
    note "a failing search is fatal, and the failure still reaches the run log"
else
    fail "a failing search took the evidence down with it:
${out}"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
