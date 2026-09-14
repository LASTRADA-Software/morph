#!/usr/bin/env bash
# Usage: bash scripts/test_runner_job_hook.sh
#
# Self-test for .github/self-hosted-runner/job-started-hook.sh.
#
# CompileCache.cmake auto-installs fastcache-cc only when NONE of
# fastcache-cc/sccache/ccache is already on PATH. Now that one container
# serves morph, fastcached and Lightweight, a previous job can leave a
# compile-cache binary behind -- hendrikmuhs/ccache-action apt-installs
# ccache; fastcached's own jobs install sccache -- and silently cost the next
# job its connection to the host fastcached daemon. The loss is invisible:
# the build still succeeds, just uncached.
#
# The asymmetry is the subtle part and is asserted in both directions:
# sccache and ccache must go, fastcache-cc must STAY. Removing fastcache-cc
# would force a fresh download on every single job.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly hook="${repo_root}/.github/self-hosted-runner/job-started-hook.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "${scratch}/bin_a" "${scratch}/bin_b"

for f in "${scratch}/bin_a/sccache" "${scratch}/bin_a/ccache" \
         "${scratch}/bin_a/fastcache-cc" "${scratch}/bin_b/ccache"; do
    printf '#!/bin/sh\nexit 0\n' >"$f"
    chmod +x "$f"
done

LASTRADA_RUNNER_HOOK_DIRS="${scratch}/bin_a ${scratch}/bin_b" bash "$hook" >/dev/null

for gone in "${scratch}/bin_a/sccache" "${scratch}/bin_a/ccache" \
            "${scratch}/bin_b/ccache"; do
    if [ -e "$gone" ]; then
        fail "$(basename "$gone") survived in $(dirname "$gone")"
    else
        note "removed $(basename "$gone") from $(basename "$(dirname "$gone")")"
    fi
done

if [ -e "${scratch}/bin_a/fastcache-cc" ]; then
    note "fastcache-cc left in place"
else
    fail "fastcache-cc was removed; every job would re-download it"
fi

# A directory that does not exist must not fail the job before it starts.
if LASTRADA_RUNNER_HOOK_DIRS="${scratch}/absent" bash "$hook" >/dev/null 2>&1; then
    note "a missing directory is tolerated"
else
    fail "a missing directory made the hook fail, which would fail the job"
fi

# Running twice must be a no-op, not an error.
if LASTRADA_RUNNER_HOOK_DIRS="${scratch}/bin_a ${scratch}/bin_b" \
   bash "$hook" >/dev/null 2>&1; then
    note "re-running on an already-clean tree succeeds"
else
    fail "the hook is not idempotent"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall job-hook checks passed\n'
