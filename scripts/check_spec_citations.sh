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
#      that exists. Checks 1 and 2 run from a hand-maintained list outward and
#      so cannot fail for a citation pointing at nothing (morph#251, morph#235).
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

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Prose lint failed. Either restore the missing citation or remove the"
    echo "banned term, or (if this is a legitimate, coordinated change) update"
    echo "docs/spec/pinned_facts.toml, the code, and this script's citation list"
    echo "together in the same commit."
    exit 1
fi

echo "Prose lint OK: every pinned fact is still cited; no banned terminology found; every cited path resolves."
