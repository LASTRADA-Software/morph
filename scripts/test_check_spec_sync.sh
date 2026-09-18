#!/usr/bin/env bash
# Usage: bash scripts/test_check_spec_sync.sh
#
# Self-test for scripts/check_spec_sync.sh, the header <-> spec sync gate.
#
# This gate's whole history is a gate that could not fail: `include/morph/net/`
# was absent from its sub-domain list, so no change to the reference transport
# could ever make it red, and commit 9445bc04 shipped an interop-visible
# WebSocket behaviour change with no documentation at all past a green tick
# (morph#560). A gate nobody drives against a tree it must reject is
# indistinguishable from that state, so every rejection it claims is
# reintroduced here, one at a time, and must be caught -- including the case
# where the gate's own scan goes blind.
#
# The two acceptance cases from morph#560 run against real history when it is
# present: the gate must reject commit 9445bc04 (the undocumented change) and
# accept a72787ed (morph#558, which documents it). They are skipped rather than
# failed in a shallow checkout, and the synthetic cases below cover the same
# two shapes unconditionally.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_spec_sync.sh"

failures=0

note() { printf 'ok: %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# ── Cases driven by a path list alone, against this repository ──────────────

# Each rejection must be a rejection *for the stated reason*: without the
# expected diagnostic, a checker that had started failing for an unrelated
# reason -- a syntax error, a missing file -- would read as fully working.
expect_rejected() {
    local description="$1" paths="$2" expected="$3" output
    if output="$(printf '%s\n' "$paths" | bash "$checker" 2>&1)"; then
        fail "NOT caught: ${description} -- the gate passed a change it must reject"
        return
    fi
    if printf '%s' "$output" | grep -qF "$expected"; then
        note "rejected: ${description}"
    else
        fail "rejected for the WRONG reason: ${description} -- no diagnostic containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

expect_accepted() {
    local description="$1" paths="$2" output
    if output="$(printf '%s\n' "$paths" | bash "$checker" 2>&1)"; then
        note "accepted: ${description}"
    else
        fail "FALSE POSITIVE: ${description} -- the gate rejected a change it must accept:"
        printf '%s\n' "$output" >&2
    fi
}

# The live gap morph#560 reported, in its minimal form.
expect_rejected "a morph::net header change with no spec change" \
    "include/morph/net/detail/ws_frame.hpp" \
    "include/morph/net/** changed but none of its spec paths did"

# The sub-domain the same read found alongside net: morph::render was never in
# the list either, and is documented in docs/spec/forms/forms.md.
expect_rejected "a morph::render header change with no spec change" \
    "include/morph/render/locale_format.hpp" \
    "include/morph/render/** changed but none of its spec paths did"

# The 1:1 mirrored sub-domains must still be gated -- the mapping table has to
# keep doing what the word list did.
expect_rejected "a core header change with no spec change" \
    "include/morph/core/backend.hpp" \
    "include/morph/core/** changed but none of its spec paths did"

expect_rejected "a forms header change with no spec change" \
    "include/morph/forms/forms.hpp" \
    "include/morph/forms/** changed but none of its spec paths did"

# A sub-domain that is in neither list arrives *exempt* under the old word
# list. That silence is the defect, so it is a failure that names itself.
expect_rejected "a new sub-domain that is in neither the map nor the exempt list" \
    "include/morph/newthing/thing.hpp" \
    "is neither in this gate's spec map nor in its exempt list"

# ── The false positives it must not manufacture ─────────────────────────────

# morph#558's shape: the spec that really documents morph::net, not a
# docs/spec/net/ folder that does not exist. A gate demanding the latter would
# have failed the pull request that fixed the documentation.
expect_accepted "a net change documented in docs/spec/core/backend.md" \
    "$(printf 'include/morph/net/detail/ws_frame.hpp\ndocs/spec/core/backend.md')"

expect_accepted "a net change documented in docs/spec/security.md" \
    "$(printf 'include/morph/net/socket_server.hpp\ndocs/spec/security.md')"

expect_accepted "a render change documented in docs/spec/forms/forms.md" \
    "$(printf 'include/morph/render/i18n.hpp\ndocs/spec/forms/forms.md')"

expect_accepted "a core change documented in docs/spec/core/" \
    "$(printf 'include/morph/core/backend.hpp\ndocs/spec/core/backend.md')"

# The two sub-domains that are deliberately exempt stay exempt.
expect_accepted "a qt and detail change with no spec change" \
    "$(printf 'include/morph/qt/qt_executor.hpp\ninclude/morph/detail/fixed_string.hpp')"

expect_accepted "a change touching no headers at all" \
    "$(printf 'README.md\ntests/test_bridge.cpp')"

# ── Cases that need a mutated tree ──────────────────────────────────────────
#
# The checker resolves its own root with `git rev-parse` and reads the tree for
# the sub-domain list and the mapping targets, so these run in a throwaway
# repository holding just the directory shape it looks at.
make_skeleton() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    ( cd "$dest" \
      && mkdir -p include/morph/core include/morph/journal include/morph/offline \
                  include/morph/session include/morph/forms include/morph/util \
                  include/morph/net include/morph/render include/morph/detail include/morph/qt \
                  docs/spec/core docs/spec/journal docs/spec/offline docs/spec/session \
                  docs/spec/forms docs/spec/util \
      && : > docs/spec/core/backend.md \
      && : > docs/spec/security.md \
      && git init -q . )
}

run_in_tree() {
    local tree="$1" paths="$2"
    ( cd "$tree" && printf '%s\n' "$paths" | bash "$checker" 2>&1 )
}

expect_rejected_in_tree() {
    local description="$1" tree="$2" paths="$3" expected="$4" output
    if output="$(run_in_tree "$tree" "$paths")"; then
        fail "NOT caught: ${description} -- the gate passed a tree it must reject"
        return
    fi
    if printf '%s' "$output" | grep -qF "$expected"; then
        note "rejected: ${description}"
    else
        fail "rejected for the WRONG reason: ${description} -- no diagnostic containing '${expected}':"
        printf '%s\n' "$output" >&2
    fi
}

# The skeleton itself must pass, or every mutation below would "pass" for the
# wrong reason.
make_skeleton "${scratch}/clean"
if output="$(run_in_tree "${scratch}/clean" "$(printf 'include/morph/net/x.hpp\ndocs/spec/security.md')")"; then
    note "the unmutated skeleton tree is accepted"
else
    fail "the unmutated skeleton tree was rejected:"
    printf '%s\n' "$output" >&2
fi

# A mapping target that was renamed away. The gate would otherwise demand a
# change to a path no correct pull request can produce -- which is exactly what
# adding the bare word `net` to the old list would have done.
#
# The new name deliberately sits outside docs/spec/: spelling a
# docs/spec/<something>.md path that does not exist would be caught by
# check_spec_citations.sh's own dangling-reference scan, which reads this file
# like any other.
make_skeleton "${scratch}/renamed"
mv "${scratch}/renamed/docs/spec/core/backend.md" "${scratch}/renamed/docs/renamed-away.md"
rm -f "${scratch}/renamed/docs/spec/security.md"
expect_rejected_in_tree "a mapping whose doc targets have all been renamed away" \
    "${scratch}/renamed" "README.md" \
    "none of the doc paths mapped to include/morph/net/ exist any more"

# Vacuity guard on the gate's own scan. A glob that stopped matching would
# leave it classifying nothing and reporting success over every header in the
# tree -- the same shape of silence it exists to close.
make_skeleton "${scratch}/blind"
rm -rf "${scratch}/blind/include/morph"
expect_rejected_in_tree "the sub-domain scan finding no sub-domains at all" \
    "${scratch}/blind" "README.md" \
    "the scan is not seeing the tree and would pass vacuously"

# ── morph#560's two acceptance cases ────────────────────────────────────────
#
# The gate consumes one thing -- a list of changed paths -- so the two commits
# are pinned here as the exact lists `git diff --name-only <sha>^ <sha>`
# produces for them. Recorded rather than computed because neither commit is an
# ancestor of master (both live on origin/batch/533-535-536, which a branch
# deletion would take with it), and an acceptance case that silently stops
# running is the failure this whole gate is about. The live-history form runs
# too, whenever the objects are in the checkout.
readonly commit_9445bc04_paths='include/morph/net/detail/ws_frame.hpp
include/morph/net/socket_backend.hpp
include/morph/net/socket_server.hpp
tests/net/test_socket_backend.cpp
tests/net/test_socket_server.cpp
tests/net/test_ws_frame.cpp'

readonly commit_a72787ed_paths='docs/spec/core/backend.md
docs/spec/security.md
docs/todo.md
include/morph/net/detail/tcp_socket.hpp
include/morph/net/detail/ws_frame.hpp
include/morph/net/socket_backend.hpp
include/morph/net/socket_server.hpp
scripts/branch_partial_allowlist.json
tests/net/test_socket_backend.cpp
tests/net/test_socket_server.cpp
tests/net/test_ws_frame.cpp'

expect_rejected "commit 9445bc04's paths (the undocumented WebSocket behaviour change)" \
    "$commit_9445bc04_paths" \
    "include/morph/net/** changed but none of its spec paths did"

expect_accepted "commit a72787ed's paths (morph#558, the same change with its docs)" \
    "$commit_a72787ed_paths"

# When the objects are present, the recorded lists are checked against the
# commits themselves -- a fixture nobody reconciles with its source is how a
# test keeps passing about something that has changed.
for pinned in 9445bc04 a72787ed; do
    if ! git -C "$repo_root" cat-file -e "${pinned}^{commit}" 2>/dev/null; then
        note "skipped: commit ${pinned} is not in this checkout, so only its recorded path list ran"
        continue
    fi
    live="$(git -C "$repo_root" diff --name-only "${pinned}^" "${pinned}")"
    case "$pinned" in
        9445bc04) recorded="$commit_9445bc04_paths" ;;
        a72787ed) recorded="$commit_a72787ed_paths" ;;
    esac
    if [ "$live" = "$recorded" ]; then
        note "the recorded path list for ${pinned} still matches the commit"
    else
        fail "the recorded path list for ${pinned} no longer matches the commit:"
        diff <(printf '%s\n' "$recorded") <(printf '%s\n' "$live") >&2 || true
    fi
done

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed.\n' "$failures" >&2
    exit 1
fi

printf '\nscripts/check_spec_sync.sh rejects every drift it claims to, and none of the changes it must accept.\n'
