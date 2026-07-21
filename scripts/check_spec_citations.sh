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
# all of docs/. A blanket `docs/` scan false-positives on this very feature's
# own historical planning documents: docs/superpowers/plans/*.md is a
# permanent, append-only record of past implementation plans (never deleted,
# per repo convention -- see git history), and at least one such plan
# discusses the banned phrase *as an example of what to look for* while
# describing this exact check, which trips a naive recursive grep despite
# being neither a spec nor code. Historical planning prose is not something
# this lint can meaningfully enforce against; only the standing design
# reference and the code it describes can regress.
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

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Prose lint failed. Either restore the missing citation or remove the"
    echo "banned term, or (if this is a legitimate, coordinated change) update"
    echo "docs/spec/pinned_facts.toml, the code, and this script's citation list"
    echo "together in the same commit."
    exit 1
fi

echo "Prose lint OK: every pinned fact is still cited; no banned terminology found."
