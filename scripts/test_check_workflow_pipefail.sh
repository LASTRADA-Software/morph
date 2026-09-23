#!/usr/bin/env bash
# Usage: bash scripts/test_check_workflow_pipefail.sh
#
# Self-test for scripts/check_workflow_pipefail.py, the gate that keeps a
# workflow pipeline from discarding its left-hand side's exit status
# (morph#479, morph#730).
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one repairs the tree it guards in the same commit that
# adds it -- so it passes on day one whether it parses a single `run:` block or
# none at all. Its whole value is what it does to the pipeline someone writes
# next month.
#
# The cases below are three kinds:
#
#   * the two defects it exists for, reintroduced verbatim -- morph#730's
#     `mutation.sh | tee` and morph#479's `clang-tidy-diff.py | tee`;
#   * the vacuity cases, which are what decide whether it measures anything:
#     a tree where the parser no longer finds `run:` blocks, and one where it
#     no longer recognises a pipeline, both of which it would otherwise pass
#     while reading nothing;
#   * the false-positive mirror. `||`, a quoted `|`, a YAML block scalar and a
#     `${{ a || b }}` expression all contain the character and none is a
#     pipeline. A gate that flagged them would be switched off within the day,
#     so they are asserted rather than assumed.
#
# Every mutation is applied to a scratch copy of the tree, one at a time.
# Applied together, a single detection would mask every other.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_workflow_pipefail.py"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# The checker reads .github/workflows/*.yml and nothing else; a copy of those
# plus the script itself is the whole tree it needs.
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
#
# Three checks the sibling self-tests' copy of this helper does not make, and
# each of them fired while this file was being written (morph#746):
#
#   * a `sed` that *fails* still leaves the redirection's empty output behind,
#     and `mv` then installs it -- so a mutator with a syntax error silently
#     truncates the file it was editing, and an `expect_accepted` case passes
#     over an empty workflow having asserted nothing;
#   * a `sed` that succeeds while matching *nothing* leaves the file identical,
#     and the case again passes having mutated nothing;
#   * either of those is invisible, because the harness only reads the
#     checker's exit status.
edit() {
    local file="$1"; shift
    if ! sed "$@" "$file" > "${file}.new"; then
        rm -f "${file}.new"
        printf 'edit: sed failed on %s\n' "$file" >&2
        return 1
    fi
    if [ ! -s "${file}.new" ]; then
        rm -f "${file}.new"
        printf 'edit: sed emptied %s\n' "$file" >&2
        return 1
    fi
    if cmp -s "${file}.new" "$file"; then
        rm -f "${file}.new"
        printf 'edit: expression matched nothing in %s\n' "$file" >&2
        return 1
    fi
    mv "${file}.new" "$file"
}

# Each mutation must be caught, and caught *for the stated reason*. `$expected`
# is a substring the diagnostic must contain; without it a mutation that broke
# the tree some unrelated way would count as a detection, and this self-test
# would report a dead gate as a working one.
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
# "catch" every case above while being worthless.
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

# -- The two defects this gate exists for ------------------------------------
# morph#730 verbatim: drop the campaign step's guard and the `| tee` is back to
# reporting `tee`'s status for a script that exited 1.
expect_caught "morph#730: the mutation campaign's pipefail removed" \
    "edit .github/workflows/mutation.yml -e '/^          set -o pipefail$/d'" \
    "mutation.sh"

# morph#479 verbatim: the clang-tidy gate's `set -o pipefail` sits *after* a
# deliberate `find | head`, so deleting it leaves the `| tee` below unguarded
# rather than removing a whole-step guard.
expect_caught "morph#479: the clang-tidy step's pipefail removed" \
    "python3 - <<'PY'
import pathlib
p = pathlib.Path('.github/workflows/ci.yml')
lines = p.read_text().split('\n')
anchor = next(i for i, l in enumerate(lines) if 'left this gate unable to fail' in l)
drop = next(i for i in range(anchor, len(lines)) if lines[i].strip() == 'set -o pipefail')
del lines[drop]
p.write_text('\n'.join(lines))
PY" \
    "nothing sets pipefail before it"

# The shape the next one will take: a new step, written by someone who has read
# none of the above, piping a fallible command into something that cannot fail.
expect_caught "a newly added unguarded pipeline" \
    "edit .github/workflows/docs.yml -e 's%^      - name: Install dependencies%      - name: Count the headers\n        run: |\n          find include -name \"*.hpp\" | wc -l\n\n      - name: Install dependencies%'" \
    "this pipeline's exit status is its last command's"

# -- The vacuity cases -------------------------------------------------------
# These decide whether the gate measures anything. A checker that stops finding
# `run:` blocks reports a clean tree it never read.
expect_caught "the run-block syntax moved out from under the parser" \
    "for f in .github/workflows/*.yml; do edit \"\$f\" -e 's|^\\( *\\)\\(- \\)\\?run:|\\1\\2x-run:|'; done" \
    "\`run:\` block(s) parsed"

# And one that stops recognising pipelines: every pipe in the tree becomes a
# `;`. Nothing is then wrong -- and nothing is being checked either.
expect_caught "every pipeline rewritten out of the shape the gate reads" \
    "python3 - <<'PY'
import pathlib, re
for p in pathlib.Path('.github/workflows').glob('*.yml'):
    out = []
    for line in p.read_text().split('\n'):
        if not line.rstrip().endswith('|'):
            line = re.sub(r'(?<![|>])\|(?!\|)', ';', line)
        out.append(line)
    p.write_text('\n'.join(out))
PY" \
    "pipeline(s) found across"

# -- The exemption has to be necessary, and has to say why -------------------
expect_caught "an exemption with no reason" \
    "edit .github/workflows/ci.yml -e 's|# pipefail-ok: find.*\$|# pipefail-ok:|'" \
    "nothing sets pipefail before it"

expect_caught "an exemption left behind after its pipeline went away" \
    "edit .github/workflows/ci.yml -e 's|^          set -euo pipefail\$|          set -euo pipefail  # pipefail-ok: stale, excuses nothing|'" \
    "excuses nothing"

# -- False positives ---------------------------------------------------------
# `||` is a disjunction. This tree is full of `cmd || fallback`, and flagging
# one would make the gate unusable.
expect_accepted "a disjunction is not a pipeline" \
    "edit .github/workflows/docs.yml -e 's%^      - name: Install dependencies%      - name: Probe\n        run: |\n          command -v doxygen || echo missing\n\n      - name: Install dependencies%'"

# A `|` inside quotes is a literal character.
expect_accepted "a quoted pipe character is not a pipeline" \
    "edit .github/workflows/docs.yml -e 's%^      - name: Install dependencies%      - name: Print\n        run: echo \"a | b\"\n\n      - name: Install dependencies%'"

# `${{ a || b }}` is a GitHub expression, substituted before bash sees it.
expect_accepted "a GitHub expression containing ||" \
    "edit .github/workflows/docs.yml -e 's%^      - name: Install dependencies%      - name: Echo the ref\n        run: echo \"\${{ github.head_ref || github.ref_name }}\"\n\n      - name: Install dependencies%'"

# `shell: bash` is documented as `bash --noprofile --norc -eo pipefail {0}`, so
# it carries the guard. Only the *absent* key does not, and that asymmetry is
# most of why this defect keeps recurring.
expect_accepted "a pipeline under an explicit shell: bash" \
    "edit .github/workflows/docs.yml -e 's%^      - name: Install dependencies%      - name: Count\n        shell: bash\n        run: find include -name \"*.hpp\" | wc -l\n\n      - name: Install dependencies%'"

# Other shells are a different question, and guessing at them would be noise.
expect_accepted "a pipeline in a pwsh step" \
    "edit .github/workflows/docs.yml -e 's%^      - name: Install dependencies%      - name: Count\n        shell: pwsh\n        run: Get-ChildItem | Measure-Object\n\n      - name: Install dependencies%'"

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
