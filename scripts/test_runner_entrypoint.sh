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
# These assert against the actual config.sh argument line, not just that the
# variable name appears somewhere in the file -- a hardcoded --url value with
# the RUNNER_SCOPE_URL comment/default left untouched must fail here.
if grep -Eq '^[[:space:]]*--url[[:space:]]+"\$\{RUNNER_SCOPE_URL\}"[[:space:]]*\\?[[:space:]]*$' "$entrypoint"; then
    note "config.sh's --url argument is \"\${RUNNER_SCOPE_URL}\""
else
    fail "config.sh's --url argument is not literally \"\${RUNNER_SCOPE_URL}\""
fi

if grep -Eq '^[[:space:]]*--runnergroup[[:space:]]+"\$\{RUNNER_GROUP\}"[[:space:]]*\\?[[:space:]]*$' "$entrypoint"; then
    note "config.sh's --runnergroup argument is \"\${RUNNER_GROUP}\""
else
    fail "config.sh's --runnergroup argument is not literally \"\${RUNNER_GROUP}\""
fi

if grep -Eq '^[[:space:]]*--labels[[:space:]]+"\$\{RUNNER_LABELS\}"[[:space:]]*\\?[[:space:]]*$' "$entrypoint"; then
    note "config.sh's --labels argument is \"\${RUNNER_LABELS}\""
else
    fail "config.sh's --labels argument is not literally \"\${RUNNER_LABELS}\""
fi

# The start-time sccache wipe belongs in the per-job hook, not here. Match
# case-insensitively with no fixed wording so quoting/rm-flag/whitespace
# reformatting of a reintroduced wipe can't slip past this check.
if grep -qi 'sccache' "$entrypoint"; then
    fail "entrypoint.sh still mentions sccache; the wipe belongs in the job hook"
else
    note "start-time sccache wipe removed"
fi

# ── labels must be the agreed set, and the retired name must be fully gone ──
if grep -qF 'RUNNER_LABELS="${RUNNER_LABELS:-self-hosted,Linux,X64,lastrada-docker}"' "$entrypoint"; then
    note "RUNNER_LABELS defaults to self-hosted,Linux,X64,lastrada-docker"
else
    fail "RUNNER_LABELS default is not exactly self-hosted,Linux,X64,lastrada-docker"
fi

if grep -qi 'morph-docker' "$entrypoint"; then
    fail "entrypoint.sh still mentions the retired morph-docker label"
else
    note "morph-docker does not appear in entrypoint.sh"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall entrypoint token-resolution checks passed\n'
