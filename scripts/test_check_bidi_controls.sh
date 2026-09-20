#!/usr/bin/env bash
# Usage: bash scripts/test_check_bidi_controls.sh
#
# Self-test for scripts/check_bidi_controls.py, the gate that keeps raw Unicode
# bidirectional control characters out of first-party files (morph#628).
#
# This gate needs its own test more than most, for a reason its own subject
# makes sharp. The tree is already clean -- 0 raw controls across 1242 tracked
# files -- so the gate passes on day one whether or not it can detect anything
# at all, and the thing it would be failing to detect is by definition
# invisible. "0 occurrences found" is exactly what a broken detector prints.
#
# So the cases below are not about the tree. They are about whether the gate
# would still be a gate on the day something arrives:
#
#   - Every declared codepoint must be found in a fixture that contains it.
#     This list is written out HERE, independently of BIDI_CONTROLS in the
#     checker: the checker's own start-up probe iterates that same table, so it
#     cannot notice an entry being deleted from it. This case can.
#   - Each invalid fixture must be rejected on its own and must name its own
#     file, so a rejection for some unrelated reason does not read as detection.
#   - No diagnostic may contain a raw control. A gate against invisible
#     characters that prints them is reporting the defect by committing it.
#   - A directory with no scannable files must be rejected, not called clean.
#   - Both EXEMPT hygiene rules must fire, and an exemption must actually work.
#
# Mutations are applied one at a time to a scratch copy: applied together, a
# single detection would mask every other.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="scripts/check_bidi_controls.py"
readonly fixtures="tests/lint/bidi_controls"

# The twelve codepoints this gate claims to reject, restated independently of
# the checker. If the checker's table loses one, the coverage case below fails
# even though the checker's own probe is content.
readonly -a declared=(
    061C 200E 200F
    202A 202B 202C 202D 202E
    2066 2067 2068 2069
)

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# ── the real tree must pass ─────────────────────────────────────────────────
# Not evidence on its own -- that is this whole file's premise -- but a
# regression here means the gate cannot be landed at all.
if output="$( cd "$repo_root" && python3 "$checker" 2>&1 )"; then
    note "the repository passes: ${output}"
else
    fail "the gate rejects the repository as it stands:"
    printf '%s\n' "$output" >&2
fi

# ── valid fixtures must be accepted ─────────────────────────────────────────
# Including the one whose prose NAMES the codepoints in ASCII, which must not
# be flagged: a gate that rejected it would make its own documentation
# unwritable, the same allowance check_nolint_directives.sh makes for the
# sentence naming NOLINTNEXTLINE.
if output="$( cd "$repo_root" && python3 "$checker" "${fixtures}/valid" 2>&1 )"; then
    note "valid fixtures accepted"
else
    fail "valid fixtures were rejected -- escaped spellings or ASCII prose about \
these characters are being read as occurrences:"
    printf '%s\n' "$output" >&2
fi

# ── every invalid fixture directory must be rejected, on its own ────────────
shopt -s nullglob
invalid_dirs=("${repo_root}/${fixtures}"/invalid/*/)
shopt -u nullglob

if [ "${#invalid_dirs[@]}" -eq 0 ]; then
    fail "no fixtures found under ${fixtures}/invalid -- this self-test would pass vacuously"
fi

for dir in "${invalid_dirs[@]}"; do
    name="$(basename "$dir")"
    rel="${fixtures}/invalid/${name}"
    if output="$( cd "$repo_root" && python3 "$checker" "$rel" 2>&1 )"; then
        fail "invalid fixture ${name} was accepted; the checker no longer detects it"
        continue
    fi
    # A nonzero exit is not enough: the checker's other failure paths are "no
    # text files found" and the start-up probe, and a fixture whose files went
    # missing would take the first -- so the fixture would still look
    # "rejected" while nothing was scanned. Require the occurrence diagnostic,
    # and require it to name a file under this fixture.
    if ! grep -q 'raw U+' <<<"$output"; then
        fail "invalid fixture ${name} was rejected, but not as a raw bidi control \
-- the checker failed for some other reason (a vacuous scan, most likely):"
        printf '%s\n' "$output" >&2
    elif ! grep -qF "${rel}/" <<<"$output"; then
        fail "invalid fixture ${name} was rejected without naming any file under \
${rel}; the diagnostic does not point at the offender:"
        printf '%s\n' "$output" >&2
    else
        note "invalid fixture ${name} rejected"
    fi
done

# ── every declared codepoint must actually be found ─────────────────────────
# The case that decides whether the declared set still means anything. Run the
# checker over the whole invalid corpus and require each of the twelve above to
# be named. Delete a codepoint from BIDI_CONTROLS and this is what goes red.
all_output="$( cd "$repo_root" && python3 "$checker" "${fixtures}/invalid" 2>&1 || true )"
missing=()
for cp in "${declared[@]}"; do
    if ! grep -qF "raw U+${cp} " <<<"$all_output"; then
        missing+=("U+${cp}")
    fi
done
if [ "${#missing[@]}" -ne 0 ]; then
    fail "the fixtures contain these codepoints but the checker reported none of \
them: ${missing[*]}. Either the checker's declared set has shrunk or the \
fixture holding them has been edited away."
else
    note "all ${#declared[@]} declared codepoints found in the invalid fixtures"
fi

# ── the diagnostic must itself be readable ──────────────────────────────────
# A gate against invisible characters must not print them: a message a reader
# cannot see is the defect being reported, one level up. Every control in an
# echoed line is replaced by a visible <U+XXXX> marker.
if grep -qP '[\x{061C}\x{200E}\x{200F}\x{202A}-\x{202E}\x{2066}-\x{2069}]' <<<"$all_output"; then
    fail "the checker's own diagnostics contain raw bidi controls; the failure \
message reproduces the defect it reports"
else
    note "no diagnostic contains a raw bidi control"
fi

# ── a directory with no scannable files must be rejected ────────────────────
# "Found nothing" is the one outcome this gate must never call clean: it is
# what a wrong path, a swallowed prune rule or a dead file walk all look like.
empty_dir="${scratch}/empty"
mkdir -p "$empty_dir"
if ( cd "$repo_root" && python3 "$checker" "$empty_dir" >/dev/null 2>&1 ); then
    fail "a directory containing no files was accepted; the gate can pass vacuously"
else
    note "directory with no scannable files rejected"
fi

# ── mutation cases against a scratch copy ───────────────────────────────────
# `sample/` holds one clean file and one carrying a raw U+200E, so an exemption
# has something to be right about and something to be wrong about.
readonly pristine="${scratch}/pristine"
mkdir -p "${pristine}/scripts" "${pristine}/sample"
cp "${repo_root}/${checker}" "${pristine}/scripts/"
printf 'constexpr const char* kSign = "\\u200E+";\n' > "${pristine}/sample/clean.cpp"
python3 -c 'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text("constexpr const char* kSign = \"\u200E+\";\n", encoding="utf-8")' \
    "${pristine}/sample/dirty.cpp"

make_tree() {
    rm -rf "${scratch}/case"
    mkdir -p "${scratch}/case"
    cp -R "${pristine}/." "${scratch}/case"
}

# `sed -i` is not portable between GNU and BSD sed; edit through a temp file.
edit() {
    local file="$1"; shift
    sed "$@" "$file" > "${file}.new"
    mv "${file}.new" "$file"
}

expect_caught() {
    local description="$1" mutator="$2" expected="$3" output
    make_tree
    if ! ( cd "${scratch}/case" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "${scratch}/case" && python3 "$checker" sample 2>&1 )"; then
        fail "NOT caught: ${description} -- the gate passed a tree it should reject"
        printf '%s\n' "$output" >&2
        return
    fi
    if grep -qF "$expected" <<<"$output"; then
        note "caught: ${description}"
    else
        fail "caught for the WRONG reason: ${description} -- no diagnostic \
containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

expect_accepted() {
    local description="$1" mutator="$2" output
    make_tree
    if ! ( cd "${scratch}/case" && eval "$mutator" ); then
        fail "mutator failed to apply: ${description}"
        return
    fi
    if output="$( cd "${scratch}/case" && python3 "$checker" sample 2>&1 )"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a tree it \
should accept:"
        printf '%s\n' "$output" >&2
    fi
}

# The baseline the exemption cases are measured against: unexempted, the raw
# U+200E in sample/dirty.cpp is reported and the escaped one in
# sample/clean.cpp is not.
expect_caught "an unexempted file carrying a raw U+200E" \
    "true" \
    "sample/dirty.cpp:1:32: raw U+200E"

# The mechanism has to work, or every case below tests an exemption that could
# not be used. Exempting the file that is actually dirty clears the tree.
expect_accepted "an exemption for the file that carries the control" \
    "edit ${checker} -e 's|^EXEMPT: dict\[str, str\] = {}|EXEMPT: dict[str, str] = {\"sample/dirty.cpp\": \"a reason\"}|'"

# Hygiene rule one: an exemption for a file with nothing to exempt. This is the
# entry that outlives its cause and quietly covers whatever lands in that file
# next.
expect_caught "an exemption for a file with no raw controls" \
    "edit ${checker} -e 's|^EXEMPT: dict\[str, str\] = {}|EXEMPT: dict[str, str] = {\"sample/clean.cpp\": \"a reason\"}|'" \
    "contains no raw bidi control"

# Hygiene rule two: an exemption for a file that does not exist.
expect_caught "an exemption naming a file that does not exist" \
    "edit ${checker} -e 's|^EXEMPT: dict\[str, str\] = {}|EXEMPT: dict[str, str] = {\"sample/gone.cpp\": \"a reason\"}|'" \
    "no such file exists"

# The start-up probe: a detector narrowed to the two marks this repository has
# actually hit still passes over a clean tree, and would pass over the whole
# repository today. The probe is what makes that impossible -- and note it
# fires here even though sample/ is a tree the gate would otherwise reject for
# an ordinary reason, because it runs before any file is opened.
expect_caught "the detector narrowed to a subset of the declared codepoints" \
    "edit ${checker} -e 's|^    return {ord(ch) for ch in text if ord(ch) in BIDI_CONTROLS}|    return {ord(ch) for ch in text if ord(ch) == 0x200E}|'" \
    "is no longer detected"

# The same failure written the other way round: a detector that finds nothing
# at all, which is the shape a mistyped range or an emptied table produces.
expect_caught "the detector disabled outright" \
    "edit ${checker} -e 's|^    return {ord(ch) for ch in text if ord(ch) in BIDI_CONTROLS}|    return set()|'" \
    "is no longer detected"

if [ "$failures" -ne 0 ]; then
    printf '\n%d case(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nall cases passed.\n'
