#!/usr/bin/env bash
# Usage: bash scripts/check_spec_citations.sh
#
# Prose-vs-manifest lint for the spec <-> code drift guard (see
# docs/spec/pinned_facts.toml and tests/test_pinned_facts.cpp). Two checks:
#
#   1. Citation check: every pinned fact must still be *mentioned* in the
#      spec markdown file that documents it, so a spec cannot silently stop
#      citing a value the manifest (and the compiled test) still track.
#   2. Banned-terminology check: phrasing from a superseded design must not
#      reappear anywhere in the authoritative design docs (docs/spec/,
#      docs/ARCHITECTURE.md) or code (include/) -- e.g. the pipe-delimited-era
#      "N-part protocol" wording the JSON Envelope superseded (see
#      docs/spec/core/wire.md, "Envelope").
#
#   3. Dangling-reference check: every docs/spec/*.md path and every
#      morph/*.hpp path mentioned anywhere in the tree must resolve to a file
#      that exists. Checks 1 and 2 run from a
#      hand-maintained list outward and so cannot fail for a citation pointing
#      at nothing (morph#251, morph#235).
#   4. wire.md table-completeness: every Envelope field, every discriminator
#      kind dispatchMessage handles, and every make* factory must appear as a
#      row in docs/spec/core/wire.md, whose tables claim to be exhaustive and
#      are what a third-party protocol implementer reads (morph#233).
#   5. Section-citation check: a citation that names a *section* of a markdown
#      file -- `<file>.md`, "<Section>" -- must name a section that is really
#      there. Check 3 stops at the path, so a citation could point a reader at
#      a heading that was renamed or never written and still lint green
#      (morph#316).
#
# This is a prose lint, not a value check: it does not parse
# docs/spec/pinned_facts.toml or re-derive expected values (that is
# tests/test_pinned_facts.cpp's job, checked at compile/run time). It only
# asserts each pinned fact is still *mentioned*, by name or literal
# substring, in its spec file. Keep this list in sync with
# docs/spec/pinned_facts.toml by hand when adding a new pinned fact.
#
# Scope note (deviation from the original design): the banned-terminology
# scan is restricted to docs/spec, docs/ARCHITECTURE.md, and include -- the
# repo's *authoritative*, currently-in-force prose and code -- rather than
# all of docs/. A blanket `docs/` scan false-positives on docs/planned/, which
# tracks not-yet-implemented designs in prose: docs/planned/drift_guard.md
# (the very design doc this feature implements, in the repo until this
# feature's final task deletes it) discusses the banned phrase *as a worked
# example* while describing this exact check, tripping a naive recursive grep
# despite being neither a spec nor code. Planning prose that merely discusses
# a superseded term, rather than reintroducing it into live docs or code, is
# not something this lint can meaningfully enforce against -- only the
# standing design reference and the code it describes can regress.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

fail=0

# ---------------------------------------------------------------------------
# 1. Citation check: "<spec file>|<required substring>"
# ---------------------------------------------------------------------------
citations=(
    "docs/spec/core/wire.md|kMaxEnvelopeBytes"
    "docs/spec/core/wire.md|8 MiB"
    "docs/spec/core/wire.md|error_on_unknown_keys = false"
    "docs/spec/util/rational.md|kMaxDecimalPlaces"
    "docs/spec/security.md|kClockSkewMs"
    "docs/spec/security.md|60s"
    "docs/spec/error_handling.md|AuthError"
    "docs/spec/error_handling.md|NotYetValid"
    "docs/spec/core/logger.md|LogLevel"
    "docs/spec/offline/offline.md|ReconnectOutcome"
    "docs/spec/core/backend.md|backend changed before completion resolved"
    "docs/spec/core/backend.md|bridge destroyed before completion resolved"
    "docs/spec/core/backend.md|transport disconnected before completion resolved"
    "docs/spec/core/backend.md|unauthorized"
    "docs/spec/core/backend.md|model not found"
    "docs/spec/core/backend.md|register requires a typeId"
)

for entry in "${citations[@]}"; do
    file="${entry%%|*}"
    needle="${entry#*|}"
    if [ ! -f "$file" ]; then
        echo "::error::pinned-facts citation check: spec file missing: $file"
        fail=1
        continue
    fi
    if ! grep -qF -- "$needle" "$file"; then
        echo "::error::pinned-facts citation check: $file no longer mentions \"$needle\" (see docs/spec/pinned_facts.toml)"
        fail=1
    fi
done

# ---------------------------------------------------------------------------
# 2. Banned-terminology check
# ---------------------------------------------------------------------------
# The JSON Envelope superseded the legacy pipe-delimited protocol
# (docs/spec/core/wire.md, "Envelope"); "N-part protocol" phrasing describing
# the old format must not reappear in the authoritative docs or code (see the
# scope note above for why this does not scan all of docs/).
banned_pattern='[0-9]+-part protocol'

hits="$(grep -rniE "$banned_pattern" docs/spec docs/ARCHITECTURE.md include 2>/dev/null || true)"
if [ -n "$hits" ]; then
    echo "::error::pinned-facts banned-terminology check: found superseded phrasing matching /${banned_pattern}/i:"
    echo "$hits"
    fail=1
fi

# ---------------------------------------------------------------------------
# 3. Dangling-reference check
# ---------------------------------------------------------------------------
# Checks 1 and 2 both run *from* a hand-maintained list outward: check 1 asks
# whether a pinned fact is still mentioned in its spec, check 2 asks whether a
# banned phrase reappeared. Neither ever asks the reverse question -- does a
# path cited *by* the code resolve to a file that exists? So this job's name
# promised citation checking while nothing here could fail for a dangling
# citation, and six of them accumulated (morph#251): spec files that moved when
# docs/spec/ was reorganised into per-subsystem directories, plus every
# `#include <morph/...>` in README.md naming a header that never existed at
# that path (morph#235).
#
# A cross-reference's entire value is that it resolves. This check walks every
# `docs/spec/**.md` and `morph/**.hpp` path mentioned anywhere in the tree and
# asserts the file is really there.
#
# Dated plans under docs/superpowers/plans/ are excluded: they are historical
# records of what was planned on a date, not reference documentation, and
# correcting a path inside a finished plan would rewrite the record.
refs_checked=0

while IFS=: read -r file line ref; do
    [ -n "$ref" ] || continue
    refs_checked=$((refs_checked + 1))
    if [ ! -f "$ref" ]; then
        echo "::error file=${file},line=${line}::dangling reference: ${ref} does not exist"
        fail=1
    fi
done < <(
    git ls-files -z '*.md' '*.hpp' '*.cpp' '*.sh' '*.yml' '*.qml' \
      | grep -zv '^docs/superpowers/plans/' \
      | xargs -0 grep -noE 'docs/spec/[A-Za-z0-9_/.-]+\.md' 2>/dev/null || true
)

# `morph/<path>.hpp` resolves relative to include/, and may carry an `include/`
# prefix when the prose names the repo-relative path.
while IFS=: read -r file line ref; do
    [ -n "$ref" ] || continue
    resolved="include/${ref#include/}"
    refs_checked=$((refs_checked + 1))
    if [ ! -f "$resolved" ]; then
        echo "::error file=${file},line=${line}::dangling reference: ${ref} does not resolve to a header (looked for ${resolved})"
        fail=1
    fi
done < <(
    git ls-files -z '*.md' \
      | grep -zv '^docs/superpowers/plans/' \
      | xargs -0 grep -noE '(include/)?morph/[A-Za-z0-9_/]+\.hpp' 2>/dev/null || true
)

# Without this the check passes silently when the scan matches nothing -- a
# broken glob, a moved directory, a grep that errored -- which is the exact
# failure mode that let the dangling citations accumulate under a job named
# for checking them.
if [ "$refs_checked" -lt 50 ]; then
    echo "::error::dangling-reference check only scanned ${refs_checked} references -- expected at least 50; the scan is not finding files and would pass vacuously"
    fail=1
else
    echo "Dangling-reference check: ${refs_checked} references scanned."
fi

# ---------------------------------------------------------------------------
# 4. wire.md table-completeness check
# ---------------------------------------------------------------------------
# docs/spec/core/wire.md presents itself as the authoritative protocol
# reference -- "Every field is present in the struct so the JSON shape is
# fixed" -- and a third-party client implementer reads its tables as
# exhaustive. They were not: the Envelope table omitted `primary` and `shared`,
# the discriminator table omitted "attach"/"assign"/"instances", and the
# factory tables omitted four factories (morph#233).
#
# Nothing could notice, because morph's own client and server both read
# wire.hpp -- so neither can observe that the *spec* disagrees with it. Only a
# check that reads both can. Each Envelope field, each kind dispatchMessage
# handles, and each make* factory must appear as a table row in wire.md.
wire_hpp="include/morph/core/wire.hpp"
wire_md="docs/spec/core/wire.md"
remote_hpp="include/morph/core/remote.hpp"
field_count=0
kind_count=0
factory_count=0

if [ ! -f "$wire_hpp" ] || [ ! -f "$wire_md" ] || [ ! -f "$remote_hpp" ]; then
    echo "::error::wire table check: expected $wire_hpp, $wire_md and $remote_hpp to exist"
    fail=1
else
    # Envelope struct fields -> rows of the `wire::Envelope` field table.
    for field in $(awk '/^struct Envelope/,/^};/' "$wire_hpp" \
                     | grep -E '^ +[A-Za-z0-9_:]+ [a-z][A-Za-z]*( = .*)?;' \
                     | sed -E 's/^ +[A-Za-z0-9_:]+ +([a-zA-Z]+).*/\1/'); do
        field_count=$((field_count + 1))
        grep -qE "^\| *\`${field}\`" "$wire_md" || {
            echo "::error file=${wire_md}::wire::Envelope field \`${field}\` is not a row in wire.md's field table"
            fail=1
        }
    done

    # Discriminator kinds dispatchMessage handles -> rows of the kind table.
    for kind in $(grep -oE 'kind == "[a-z]+"' "$remote_hpp" | grep -oE '"[a-z]+"' | tr -d '"' | sort -u); do
        kind_count=$((kind_count + 1))
        grep -qE "^\| *\`\"${kind}\"\`" "$wire_md" || {
            echo "::error file=${wire_md}::discriminator \"${kind}\" is handled by dispatchMessage but is not a row in wire.md's discriminator table"
            fail=1
        }
    done

    # Envelope factories -> rows of both factory tables.
    for fn in $(grep -oE '^inline Envelope make[A-Za-z]+' "$wire_hpp" | awk '{print $3}' | sort -u); do
        factory_count=$((factory_count + 1))
        rows="$(grep -cE "^\| *\`${fn}[\`(]" "$wire_md" || true)"
        if [ "$rows" -lt 2 ]; then
            echo "::error file=${wire_md}::factory ${fn}() appears in ${rows} of wire.md's 2 factory tables (prose + API reference)"
            fail=1
        fi
    done

    # Floors are per category, not on the total: a lumped count lets one
    # category parse to nothing while the other two carry it over the line.
    # That is how `uint64_t` fields were silently skipped while this check
    # reported green -- the type contains digits, and the field pattern did not.
    if [ "$field_count" -lt 13 ] || [ "$kind_count" -lt 8 ] || [ "$factory_count" -lt 9 ]; then
        echo "::error::wire table check parsed ${field_count} fields (>=13), ${kind_count} kinds (>=8), ${factory_count} factories (>=9) -- a category came up short, so the check would pass while ignoring it"
        fail=1
    else
        echo "wire.md table check: ${field_count} fields, ${kind_count} kinds, ${factory_count} factories all present."
    fi
fi

# ---------------------------------------------------------------------------
# 5. Section-citation check
# ---------------------------------------------------------------------------
# Check 3 asks whether a cited *path* resolves. It never asks whether the
# *section* the same citation names is really in that file, so a comment could
# send a reader to a heading that had been renamed, or that was never written
# at all, and the drift guard stayed green (morph#316). A section citation is
# how code points at the *reasoning* behind an invariant -- the part the code
# cannot express -- so one that resolves to nothing is the exact failure this
# guard exists to prevent, not a cosmetic typo.
#
# Deliberately narrow, per morph#316's own suggested direction: only the
# backticked `<file>.md`, "<Section>" shape is checked, including its
# multi-quote `<file>.md`, "<Section>", "<Subsection>" form. The unbackticked
# prose form (docs/spec/journal/journal.md, "Outcome") is *not* scanned,
# because there the quoted string is legitimately a table row or a bold label
# rather than a heading -- requiring a heading match there would manufacture
# failures for citations that are currently correct. Widening the shape is a
# separate change that has to teach this check about non-heading anchors first.
#
# A cited section matches a heading on its normalised text (backticks, bold
# markers, case and runs of whitespace ignored), and also matches a heading
# that carries an appended qualifier the citation drops -- `## Foo — bar` and
# `## Foo (bar)` are both cited as "Foo" throughout the tree, and both spell a
# complete title followed by a qualifier rather than a different title.
sections_checked=0

# Every accepted spelling of every heading in $1, normalised for comparison.
heading_anchors() {
    local raw
    raw="$(grep -E '^#{1,6} +' "$1" | sed -E 's/^#{1,6} +//')"
    {
        printf '%s\n' "$raw"
        printf '%s\n' "$raw" | sed -E 's/ +\([^()]*\)$//'
        printf '%s\n' "$raw" | sed -E 's/ +(—|--) .*$//'
        printf '%s\n' "$raw" | sed -E 's/ +\([^()]*\)$//' | sed -E 's/ +(—|--) .*$//'
    } | tr -d '`*' | tr -s ' ' | sed -E 's/^ +//; s/ +$//' | tr '[:upper:]' '[:lower:]' | sort -u
}

# A citation spells its target either repo-relative (`docs/spec/core/wire.md`),
# relative to the citing file (a spec cross-referencing its neighbour), rooted
# at docs/ (SECURITY.md's `spec/security.md`), or by bare basename from code
# (`backend.md`) -- resolved by unique basename, since an ambiguous one is not
# a reference a reader could follow either.
resolve_cited_md() {
    local from_dir="$1" cited="$2" cand matches
    for cand in "$cited" "${from_dir}/${cited}" "docs/${cited}"; do
        if [ -f "$cand" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    matches="$(git ls-files -- "$cited" "*/${cited}" | head -n 3)"
    if [ "$(printf '%s' "$matches" | grep -c . || true)" = "1" ]; then
        printf '%s\n' "$matches"
        return 0
    fi
    return 1
}

while IFS=$'\t' read -r file line cited section; do
    [ -n "$section" ] || continue
    sections_checked=$((sections_checked + 1))
    if ! target="$(resolve_cited_md "$(dirname "$file")" "$cited")"; then
        echo "::error file=${file},line=${line}::section citation names ${cited}, which does not resolve to a markdown file in the tree"
        fail=1
        continue
    fi
    normalised="$(printf '%s' "$section" | tr -d '`*' | tr -s ' ' | sed -E 's/^ +//; s/ +$//' | tr '[:upper:]' '[:lower:]')"
    if ! heading_anchors "$target" | grep -qxF -- "$normalised"; then
        echo "::error file=${file},line=${line}::dangling section citation: ${target} has no section \"${section}\""
        fail=1
    fi
done < <(
    git ls-files -z '*.md' '*.hpp' '*.cpp' '*.sh' '*.yml' '*.qml' \
      | grep -zv '^docs/superpowers/plans/' \
      | xargs -0 awk '
    FNR == 1 { flush(); buf = "" }
    {
        text = $0
        # Strip a leading comment marker so a citation Doxygen-wrapped across
        # two comment lines still reads as one logical line -- which the live
        # instance morph#316 reported (bridge.hpp) is.
        sub(/^[ \t]*(\/\/\/?[!<]?|\*|#+)[ \t]?/, "", text)
        if (buf == "") { buf = text; bufline = FNR; buffile = FILENAME } else { buf = buf " " text }
        # A citation continues onto the next line exactly when this one ends
        # on the comma separating the file from its section, or one section
        # from the next. Spelling the whole prefix out (rather than "ends in a
        # comma") keeps an unrelated line that happens to end in `",` from
        # swallowing the line after it and mis-attributing its line number.
        if (buf ~ /`[^`]+\.md`[ \t]*,([ \t]*"[^"]+"[ \t]*,)*[ \t]*$/) next
        flush()
        buf = ""
    }
    END { flush() }
    function flush(   rest, cit, cited, sec) {
        rest = buf
        while (match(rest, /`[A-Za-z0-9_\/.-]+\.md`[ \t]*,[ \t]*"[^"]+"([ \t]*,[ \t]*"[^"]+")*/)) {
            cit = substr(rest, RSTART, RLENGTH)
            rest = substr(rest, RSTART + RLENGTH)
            cited = cit
            sub(/^`/, "", cited)
            sub(/`.*$/, "", cited)
            while (match(cit, /"[^"]+"/)) {
                sec = substr(cit, RSTART + 1, RLENGTH - 2)
                cit = substr(cit, RSTART + RLENGTH)
                print buffile "\t" bufline "\t" cited "\t" sec
            }
        }
    }' 2>/dev/null || true
)

# Same lesson as check 3's floor and check 4's per-category floors, and its own
# counter rather than a share of either: a regex that stopped matching, a
# comment-marker shape this awk does not strip, or a glob that went stale would
# otherwise leave this check reporting green having verified no section at all
# -- reproducing, inside the fix, the silent pass it was written to close.
if [ "$sections_checked" -lt 40 ]; then
    echo "::error::section-citation check only scanned ${sections_checked} cited sections -- expected at least 40; the scan is not finding citations and would pass vacuously"
    fail=1
else
    echo "Section-citation check: ${sections_checked} cited sections scanned."
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Prose lint failed. Either restore the missing citation or remove the"
    echo "banned term, or (if this is a legitimate, coordinated change) update"
    echo "docs/spec/pinned_facts.toml, the code, and this script's citation list"
    echo "together in the same commit."
    exit 1
fi

echo "Prose lint OK: every pinned fact is still cited; no banned terminology found; every cited path resolves; every cited section exists."
