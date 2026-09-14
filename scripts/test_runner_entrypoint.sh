#!/usr/bin/env bash
# Usage: bash scripts/test_runner_entrypoint.sh
#
# Self-test for read_runner_token() in
# .github/self-hosted-runner/entrypoint.sh.
#
# Two callers deliver the registration token two different ways and both must
# keep working: systemd's run-runner.sh pipes it on stdin (so it never enters
# the container environment, where any job could read it), while the plain
# `docker run -e RUNNER_TOKEN=...` path documented for Docker Desktop hosts
# has no stdin to pipe. Getting the precedence wrong fails closed in the worst
# way -- the runner registers with an empty token and the container
# restart-loops, which is the exact failure this whole change exists to end.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly entrypoint="${repo_root}/.github/self-hosted-runner/entrypoint.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

# Sourcing must not run the registration; the guard makes that safe.
resolve() {
    # shellcheck disable=SC1090
    LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY=1 . "$entrypoint"
    read_runner_token
}

got="$(printf 'tok-from-stdin\n' | (RUNNER_TOKEN= resolve))"
if [ "$got" = "tok-from-stdin" ]; then
    note "stdin token is used"
else
    fail "stdin token not used; got '${got}'"
fi

got="$(printf 'tok-from-stdin\n' | (RUNNER_TOKEN=tok-from-env resolve))"
if [ "$got" = "tok-from-stdin" ]; then
    note "stdin wins over RUNNER_TOKEN"
else
    fail "stdin did not win over the environment; got '${got}'"
fi

got="$(printf '' | (RUNNER_TOKEN=tok-from-env resolve))"
if [ "$got" = "tok-from-env" ]; then
    note "falls back to RUNNER_TOKEN when stdin is empty"
else
    fail "no fallback to RUNNER_TOKEN; got '${got}'"
fi

got="$(printf '' | (RUNNER_TOKEN= resolve))"
if [ -z "$got" ]; then
    note "yields empty when neither source has a token"
else
    fail "invented a token from nowhere: '${got}'"
fi

# ── the registration itself must be org-scoped and group-aware ───────────────
if grep -q 'runnergroup' "$entrypoint"; then
    note "config.sh is passed --runnergroup"
else
    fail "entrypoint.sh does not pass --runnergroup"
fi

if grep -q 'RUNNER_SCOPE_URL' "$entrypoint"; then
    note "registration URL comes from RUNNER_SCOPE_URL"
else
    fail "entrypoint.sh still hardcodes a repository URL"
fi

if grep -q 'rm -f /usr/local/bin/sccache' "$entrypoint"; then
    fail "the start-time sccache wipe is still here; it belongs in the job hook"
else
    note "start-time sccache wipe removed"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall entrypoint token-resolution checks passed\n'
