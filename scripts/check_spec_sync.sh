#!/usr/bin/env bash
# Usage: git diff --name-only <base> HEAD | bash scripts/check_spec_sync.sh
#
# The header <-> spec sync gate .github/workflows/spec-sync.yml enforces: a
# change to a header sub-domain must come with a change to the docs that
# document that sub-domain, so the specs cannot silently drift behind the code.
#
# It lives in a script rather than inline in the workflow so it can be driven
# against a list of paths -- which is what makes it testable
# (scripts/test_check_spec_sync.sh) and what makes "this gate goes red on that
# commit" a thing anyone can reproduce, rather than a claim about a YAML block
# that only ever runs on a pull_request event.
#
# ── Why the sub-domain list is a table and not a word list (morph#560) ───────
#
# The gate used to read `subdomains="core journal offline session forms util"`
# and map `include/morph/<sub>/` to `docs/spec/<sub>/` by string substitution.
# Two sub-domains were missing from that list -- `net` and `render` -- and a
# sub-domain missing from it is not merely unchecked, it is indistinguishable
# from one that was deliberately exempted. Measured: commit 9445bc04 (morph#533)
# changed `WsFrameReader` to reject ten previously-accepted classes of
# WebSocket frame -- an interop-visible change to the reference transport --
# and touched no documentation at all, with every gate green.
#
# The naive repair does not work, which is why this is a table. There is no
# `docs/spec/net/` folder: `morph::net` is documented in
# `docs/spec/core/backend.md` and `docs/spec/security.md`, and `morph::render`
# in `docs/spec/forms/forms.md`. Adding the word `net` to that list would have
# demanded a `docs/spec/net/` change that no correct pull request could make --
# it would have failed morph#558, the pull request that documented the change
# properly.
#
# So each sub-domain names the doc paths that really document it, and -- the
# part that stops this recurring -- every directory under include/morph/ must
# appear either in that table or in the exempt list below. A new sub-domain is
# now a failure that names itself, instead of a silent exemption.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# ── The table: <sub-domain>|<accepted doc path regex>|... ───────────────────
#
# A sub-domain passes when the same change touches at least one path matching
# any of its regexes. Most are the 1:1 `docs/spec/<sub>/` mirror; the two that
# are not carry the reason beside them.
spec_map=(
    "core|^docs/spec/core/"
    "journal|^docs/spec/journal/"
    "offline|^docs/spec/offline/"
    "session|^docs/spec/session/"
    "forms|^docs/spec/forms/"
    "util|^docs/spec/util/"

    # morph::net has no folder of its own. The SocketBackend/SocketServer API
    # tables, the RFC 6455 frame-codec design-decision rows and the transport's
    # limitations list live in docs/spec/core/backend.md; its TLS and
    # resource-limit posture is docs/spec/security.md, "The Qt-free reference
    # transport (`morph::net`)". `^docs/spec/net/` is listed first so that if
    # the folder is ever created, it counts without a further edit here.
    "net|^docs/spec/net/|^docs/spec/core/backend\.md\$|^docs/spec/security\.md\$"

    # morph::render is the renderer-side seam morph::forms is specified
    # against: docs/spec/forms/forms.md documents resolveText(),
    # normalizeLocaleNumber() and the TranslationProvider contract by name.
    # `^docs/spec/render/` for the same forward-compatibility reason as net.
    "render|^docs/spec/render/|^docs/spec/forms/"
)

# Sub-domains with no spec of their own, deliberately exempt.
#
#   detail  -- implementation details (fixed_string, quantity_equation) that no
#              spec describes as a public surface.
#   qt      -- the Qt backend/renderer. Its *behaviour* is specified through
#              the core backend contract it realizes; there is no qt spec
#              folder to require a change in.
#
# Anything not in this list and not in the table above is a hard failure, not
# an exemption: that silence is the defect morph#560 recorded.
exempt_subdomains="detail qt"

changed="$(cat)"

if [ -z "${changed//[[:space:]]/}" ]; then
    echo "Spec sync OK: the change touches no files."
    exit 0
fi

fail=0

mapped_subdomains=""
for entry in "${spec_map[@]}"; do
    mapped_subdomains="${mapped_subdomains} ${entry%%|*}"
done
known_subdomains="${mapped_subdomains} ${exempt_subdomains}"

is_known() {
    case " ${known_subdomains} " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

# ── 1. Every sub-domain is classified ───────────────────────────────────────
#
# Both the sub-domains that exist in the tree and the ones this change
# introduces: a pull request that adds include/morph/<new>/ has to say which of
# the two lists above it belongs in, in the same change, rather than arriving
# exempt by omission.
subdomains_seen=""
for dir in include/morph/*/; do
    [ -d "$dir" ] || continue
    sub="$(basename "$dir")"
    subdomains_seen="${subdomains_seen} ${sub}"
done
while IFS= read -r path; do
    case "$path" in
        include/morph/*/*)
            sub="${path#include/morph/}"
            sub="${sub%%/*}"
            case " ${subdomains_seen} " in
                *" ${sub} "*) ;;
                *) subdomains_seen="${subdomains_seen} ${sub}" ;;
            esac
            ;;
    esac
done <<< "$changed"

classified=0
for sub in ${subdomains_seen}; do
    classified=$((classified + 1))
    if ! is_known "$sub"; then
        echo "::error::include/morph/${sub}/ is neither in this gate's spec map nor in its exempt list, so every change to it is silently exempt -- the defect morph#560 recorded."
        echo "  Add \"${sub}|^docs/spec/<the docs that document it>\" to spec_map in scripts/check_spec_sync.sh,"
        echo "  or add '${sub}' to exempt_subdomains with the reason it has no spec."
        fail=1
    fi
done

# A floor, for the same reason check_spec_citations.sh's table checks carry
# one: a glob that stopped matching would leave this loop classifying nothing
# and reporting success, which is the exact shape of failure this gate is for.
if [ "${classified}" -lt 8 ]; then
    echo "::error::spec-sync: only ${classified} header sub-domain(s) found under include/morph/ -- expected at least 8; the scan is not seeing the tree and would pass vacuously."
    fail=1
fi

# ── 2. Every mapping still points at something that exists ──────────────────
#
# A mapping whose target was renamed is worse than no mapping: it demands a
# change to a path no correct pull request can produce. Anchored file regexes
# are checked as files, directory prefixes as directories.
for entry in "${spec_map[@]}"; do
    sub="${entry%%|*}"
    rest="${entry#*|}"
    reachable=0
    while [ -n "$rest" ]; do
        pattern="${rest%%|*}"
        if [ "$rest" = "$pattern" ]; then rest=""; else rest="${rest#*|}"; fi
        literal="${pattern#^}"
        case "$literal" in
            *'$')
                literal="${literal%'$'}"
                literal="${literal//\\./.}"
                [ -f "$literal" ] && reachable=1
                ;;
            */)
                [ -d "$literal" ] && reachable=1
                ;;
        esac
    done
    if [ "${reachable}" -eq 0 ]; then
        echo "::error::spec-sync: none of the doc paths mapped to include/morph/${sub}/ exist any more, so that mapping can never be satisfied. Fix the mapping in scripts/check_spec_sync.sh."
        fail=1
    fi
done

# ── 3. The gate itself ──────────────────────────────────────────────────────
missing=""
for entry in "${spec_map[@]}"; do
    sub="${entry%%|*}"
    rest="${entry#*|}"
    if ! grep -qE "^include/morph/${sub}/" <<< "$changed"; then
        continue
    fi
    satisfied=0
    patterns=""
    while [ -n "$rest" ]; do
        pattern="${rest%%|*}"
        if [ "$rest" = "$pattern" ]; then rest=""; else rest="${rest#*|}"; fi
        patterns="${patterns} ${pattern}"
        if grep -qE "$pattern" <<< "$changed"; then
            satisfied=1
        fi
    done
    if [ "${satisfied}" -eq 0 ]; then
        missing="${missing}${sub}|${patterns# }"$'\n'
        fail=1
    fi
done

if [ -n "$missing" ]; then
    echo ""
    while IFS='|' read -r sub patterns; do
        [ -n "$sub" ] || continue
        echo "::error::include/morph/${sub}/** changed but none of its spec paths did."
        echo "  Accepted (any one of): ${patterns}"
    done <<< "$missing"
    echo ""
    echo "Update the design spec(s) for the affected sub-domain(s), or add the"
    echo "label 'no docs update' to this PR if no spec change is warranted."
fi

if [ "${fail}" -ne 0 ]; then
    exit 1
fi

echo "Spec sync OK: ${classified} sub-domain(s) classified; every touched header sub-domain has a matching spec change."
