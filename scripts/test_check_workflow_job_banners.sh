#!/usr/bin/env bash
# Usage: bash scripts/test_check_workflow_job_banners.sh
#
# Self-test for scripts/check_workflow_job_banners.py, the gate that keeps every
# `  # ── … ──` section banner sitting above the job it describes (morph#621).
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one is maximally exposed to that: the seven displacements
# morph#621 reported are repaired in the same commit that adds the gate, so the
# gate passes on day one whether it parses anything at all.
#
# It is also the gate whose *vacuous* form is easiest to write by accident. A
# checker that only counted displacements would score a perfect zero on a tree
# with every banner deleted. So the deletion case below is not an extra: it is
# the case that decides whether this gate measures anything.
#
# Every drift the gate claims to catch is reintroduced into a scratch copy of
# the tree, one at a time, and must be caught for the stated reason. One
# mutation at a time matters: applied together, a single detection would mask
# every other.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_workflow_job_banners.py"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# The checker reads .github/workflows/*.yml and its own UNBANNERED table; a
# copy of those two paths is the whole tree it needs.
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

# Reproduce morph#621's defect mechanically: lift the banner line matching
# $2 out of $1 and re-insert it $3 lines earlier, i.e. back into the previous
# job's trailing steps. Done in Python because it is a line move, which sed
# cannot express without holding the whole file in the hold space.
displace_banner() {
    python3 - "$1" "$2" "$3" <<'PY'
import sys
path, needle, up = sys.argv[1], sys.argv[2], int(sys.argv[3])
lines = open(path, encoding="utf-8").read().split("\n")
hits = [i for i, l in enumerate(lines) if l.startswith("  # ── ") and needle in l]
assert len(hits) == 1, f"{needle!r} matched {len(hits)} banners"
i = hits[0]
banner = lines.pop(i)
lines.insert(i - up, banner)
open(path, "w", encoding="utf-8").write("\n".join(lines))
PY
}

# Each mutation must be caught, and caught *for the stated reason*. `$expected`
# is a substring the diagnostic must contain; without it a mutation that broke
# the tree some unrelated way -- a mangled sed, a file the mutator emptied --
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
# morph#621 exactly: the banner pushed back above the previous job's trailing
# `sccache stats` / `Save sccache` steps. Eight lines is enough to clear the
# `valgrind:` key and land inside `linux-all-features`'s cache steps.
expect_caught "a banner displaced into the previous job's trailing steps" \
    "displace_banner .github/workflows/ci.yml 'Valgrind (memcheck)' 8" \
    "this section banner does not introduce a job"

# The same for one with a multi-paragraph rationale under it: only the banner
# line moves, so the paragraphs stay behind and the gate must still object.
expect_caught "a banner with a rationale block displaced upward" \
    "displace_banner .github/workflows/ci.yml 'every optional feature enabled at once' 12" \
    "this section banner does not introduce a job"

# -- The vacuity case --------------------------------------------------------
# This is the case that decides whether the gate measures anything. Deleting a
# banner outright removes a displacement, so a checker that only counted
# displacements reports this tree as *more* correct than the real one.
expect_caught "a banner deleted rather than moved" \
    "edit .github/workflows/ci.yml -e '/# ── Valgrind (memcheck)/d'" \
    "job \`valgrind\` is introduced by no section banner"

# The same defect one step later: a job added to a bannered workflow without a
# banner. This is the day-that-has-not-happened-yet case -- the gate's value is
# what it does then, not on the tree it shipped with.
expect_caught "a new job added to a bannered workflow with no banner" \
    "printf '%s\n' '  brand-new-lint:' '    runs-on: ubuntu-24.04' '    steps:' '      - uses: actions/checkout@v4' >> .github/workflows/ci.yml" \
    "job \`brand-new-lint\` is introduced by no section banner"

# -- The exemption set must stay necessary -----------------------------------
# An exemption for a job that does have a banner: the hand-written record that
# is no longer true and that nothing reads back against reality.
expect_caught "an exemption for a job that now has a banner" \
    "edit ${checker} -e 's|^        \"probe-self-hosted\": |        \"valgrind\": \"stale\",\n        \"probe-self-hosted\": |'" \
    "but it now has a banner at line"

# An exemption for a job that no longer exists.
expect_caught "an exemption naming a job the workflow does not declare" \
    "edit ${checker} -e 's|^        \"probe-self-hosted\": |        \"long-gone\": \"stale\",\n        \"probe-self-hosted\": |'" \
    "which is not a job in this workflow"

# -- The gate must not go blind ----------------------------------------------
# If the banner syntax is reworded out from under the parser, every workflow
# reads as "not in the banner style" and both rules stop applying. The honest
# answer is failure, not the silent green a per-file skip would produce.
expect_caught "the banner syntax is reworded tree-wide" \
    "edit .github/workflows/ci.yml -e 's|^  # ── |  # == |' \
        && edit .github/workflows/drift-guard.yml -e 's|^  # ── |  # == |'" \
    "section banner found in any workflow"

# -- False positives ---------------------------------------------------------
# The gate is about placement, not wording. Rewriting what a banner says must
# not be drift, or every rename would have to touch this script.
expect_accepted "a banner's text rewritten in place" \
    "edit .github/workflows/ci.yml -e 's|^  # ── Valgrind (memcheck) .*|  # ── Valgrind, under memcheck ──|'"

# A banner separated from its job by blank lines and by its own continuation
# paragraphs. Both shapes are already in ci.yml; the gate must accept both, or
# it would be pinning a whitespace convention the tree does not follow.
expect_accepted "blank lines and rationale paragraphs between banner and job" \
    "edit .github/workflows/ci.yml -e 's|^  valgrind:|  #\n  # An added paragraph.\n\n  valgrind:|'"

# A whole new job introduced by its own banner: the ordinary way the workflow
# grows. If this failed, the gate would be a tax on adding jobs.
expect_accepted "a new job added together with its banner" \
    "printf '%s\n' '' '  # ── A brand new lint ──' '  brand-new-lint:' '    runs-on: ubuntu-24.04' '    steps:' '      - uses: actions/checkout@v4' >> .github/workflows/ci.yml"

# A workflow that has never used the banner style must not be dragged into it.
expect_accepted "a job added to a workflow with no banners at all" \
    "printf '%s\n' '' '  another-docs-job:' '    runs-on: ubuntu-24.04' '    steps:' '      - uses: actions/checkout@v4' >> .github/workflows/docs.yml"

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
