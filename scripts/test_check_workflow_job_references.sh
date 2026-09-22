#!/usr/bin/env bash
# Usage: bash scripts/test_check_workflow_job_references.sh
#
# Self-test for scripts/check_workflow_job_references.py, the gate that keeps a
# workflow comment from naming a job that does not exist (morph#637,
# morph#645).
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one is unusually exposed: its subject is prose. The
# checker matches one phrase shape, and a tree whose comments drift out of that
# shape -- "the ladder-tests leg", "the job that runs clang-tidy" -- gives it
# nothing to match while it keeps exiting 0. So the cases below are not only
# "does it reject a bad reference"; the ones that matter are the two that ask
# whether it is still reading anything at all.
#
# Every drift is reintroduced into a scratch copy of the tree, one at a time,
# and must be caught for the stated reason. One mutation at a time matters:
# applied together, a single detection would mask every other.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_workflow_job_references.py"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# The checker reads .github/workflows/*.yml and its own EXEMPT table; a copy of
# those two paths is the whole tree it needs.
readonly pristine="${scratch}/pristine"
mkdir -p "${pristine}/.github/workflows" "${pristine}/scripts"
cp "${repo_root}"/.github/workflows/*.yml "${pristine}/.github/workflows/"
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

# Each mutation must be caught, and caught *for the stated reason*. `$expected`
# is a substring the diagnostic must contain; without it a mutation that broke
# the tree some unrelated way would count as a detection, and this self-test
# would report a gate that no longer detects anything as fully working.
expect_caught() {
    local description="$1" mutator="$2" expected="$3"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && python3 "$checker" . 2>&1 )"; then
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

# The mirror, for false positives. A gate that rejected every tree would
# "catch" every case below while being worthless.
expect_accepted() {
    local description="$1" mutator="$2"
    local tree="${scratch}/case" output
    make_tree "$tree"
    if ! ( cd "$tree" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "$tree" && python3 "$checker" . 2>&1 )"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it should accept:"
        printf '%s\n' "$output" >&2
    fi
}

# -- The unmodified tree must pass -------------------------------------------
make_tree "${scratch}/clean"
if output="$( cd "${scratch}/clean" && python3 "$checker" . 2>&1 )"; then
    note "the unmodified tree passes"
else
    fail "the unmodified tree was rejected by the gate:"
    printf '%s\n' "$output" >&2
fi

# -- The defect this gate exists for -----------------------------------------
# morph#637 exactly: a comment naming the script where the job id belongs.
expect_caught "a comment naming a script that is not a job id" \
    "edit .github/workflows/ci.yml -e 's|The option-coverage job below|The check-workflow-option-coverage job below|'" \
    "names a \`check-workflow-option-coverage\` job"

# The other half of the same defect: the job is real, then it is renamed and
# the comment is not. Renaming `ladder-tests` leaves four references stranded.
expect_caught "a job renamed out from under the comments that name it" \
    "edit .github/workflows/ci.yml -e 's|^  ladder-tests:|  ladder-suites:|'" \
    "names a \`ladder-tests\` job"

# A brand-new comment inventing a job that never existed -- the shape a
# copy-pasted paragraph takes.
expect_caught "a new comment naming a job nothing declares" \
    "edit .github/workflows/drift-guard.yml -e '1i\\
# See the nightly-fuzz job for the same argument.'" \
    "names a \`nightly-fuzz\` job"

# -- The vacuity cases -------------------------------------------------------
# These are what decide whether the gate measures anything. Prose can drift out
# of the shape the checker reads without a single reference being wrong, and
# then it passes having matched nothing.
expect_caught "every reference reworded out of the shape the gate reads" \
    "edit ${checker} -e 's|^    \"drift-guard.yml\": {|    \"unused.yml\": {|' && python3 - <<'PY'
import pathlib, re
for p in pathlib.Path('.github/workflows').glob('*.yml'):
    out = []
    for line in p.read_text().split('\n'):
        if line.lstrip().startswith('#'):
            line = re.sub(r'\bjobs?\b', 'leg', line)
        out.append(line)
    p.write_text('\n'.join(out))
PY" \
    "no \"<hyphenated-id> job\" reference found in any workflow"

# The same blindness one level down: the parser stops finding job keys, so
# every reference resolves against an empty set. Failing loudly here is the
# difference between "nothing to check" and "checked nothing".
expect_caught "the job-key syntax moved out from under the parser" \
    "for f in .github/workflows/*.yml; do edit \"\$f\" -e 's|^jobs:|x-jobs:|'; done" \
    "no job ids found in any workflow"

# -- The exemption set must stay necessary -----------------------------------
# An exemption whose sentence has been rewritten: the hand-written record that
# is no longer true and that nothing reads back against reality.
expect_caught "an exemption whose phrase no longer appears" \
    "edit .github/workflows/drift-guard.yml -e 's|dependency-free job|standalone job|'" \
    "EXEMPT names \`dependency-free\`, which no longer appears"

# An exemption for a token that has since become a real job id. The entry is
# then excusing a rule the token satisfies, which hides the next rename.
expect_caught "an exemption for a token that is now a job" \
    "edit .github/workflows/drift-guard.yml -e 's|^  prose-lint:|  dependency-free:\n    runs-on: ubuntu-24.04\n    steps:\n      - uses: actions/checkout@v4\n\n  prose-lint:|'" \
    "which is now a real job id"

# -- False positives ---------------------------------------------------------
# The 193 English occurrences of "<word> job" in this tree's comments are the
# reason the rule is restricted to hyphenated tokens. If any of these were
# flagged the gate would be unusable, so they are asserted rather than assumed.
expect_accepted "ordinary English before the word job" \
    "edit .github/workflows/docs.yml -e '1i\\
# This job, every job, a sanitizer job, the next job'\\''s build, one per job.'"

# A cross-file reference: ci.yml's comments name drift-guard.yml's jobs and the
# reverse. A per-file rule would reject every one of them.
expect_accepted "a comment naming a job declared in another workflow" \
    "edit .github/workflows/docs.yml -e '1i\\
# The automoc-include-lint job in drift-guard.yml makes the same argument.'"

# Backticks and possessives are formatting, not meaning.
expect_accepted "a backticked and possessive reference" \
    "edit .github/workflows/docs.yml -e '1i\\
# See the \`linux-compilers\` job'\\''s Configure step.'"

# A hyphenated word that is not before "job" is not a reference. The gate reads
# one phrase shape, and must not start guessing at every hyphenated token.
expect_accepted "a hyphenated token that is not a job reference" \
    "edit .github/workflows/docs.yml -e '1i\\
# doxygen-awesome-css is fetched at configure time by this file'\\''s CMakeLists.'"

# Shell comments inside a `run:` block are code, not prose about the workflow.
expect_accepted "a hash comment inside a run block" \
    "edit .github/workflows/docs.yml -e 's|^          tar -xzf /tmp/doxygen.tar.gz -C /tmp|          echo \"not-a-real job\"\n          tar -xzf /tmp/doxygen.tar.gz -C /tmp|'"

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
