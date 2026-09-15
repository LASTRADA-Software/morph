#!/usr/bin/env bash
# Usage: bash scripts/test_runner_entrypoint.sh
#
# Self-test for read_runner_token() and install_deregister_trap() in
# .github/self-hosted-runner/entrypoint.sh.
#
# Two callers deliver the registration token two different ways and both must
# keep working: systemd's run-runner.sh pipes it on stdin (so it never enters
# the container environment, where any job could read it), while the plain
# `docker run -e RUNNER_TOKEN=...` path documented for Docker Desktop hosts
# has no stdin to pipe. Getting the precedence wrong fails closed in the worst
# way -- the runner registers with an empty token and the container
# restart-loops, which is the exact failure this whole change exists to end.
#
# Separately, exactly one side must ever deregister a runner: under systemd
# the host's ExecStop does it (deregister-runner.sh, with a credential that
# hasn't expired); standalone (`docker run`, no systemd) this container's own
# EXIT trap is the only mechanism there is. install_deregister_trap() picks
# between them based on RUNNER_SELF_DEREGISTER, and that choice is asserted
# below behaviourally -- by actually letting the trap fire -- not textually.
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

# ── the EXIT deregistration trap is owned by exactly one side ────────────────
# Behavioural, not textual: source entrypoint.sh into a real subshell, point
# it at a stub ./config.sh standing in for the runner binary, call
# install_deregister_trap, and let the subshell actually exit -- then look at
# what the stub observed, not at what the source text says. Under systemd,
# run-runner.sh sets RUNNER_SELF_DEREGISTER=0 and the host's ExecStop
# (deregister-runner.sh) removes the registration instead; standalone
# (`docker run`, no systemd) this trap remains the only mechanism.
trap_scratch="$(mktemp -d)"
trap 'rm -rf "$trap_scratch"' EXIT

cat >"${trap_scratch}/config.sh" <<'STUB'
#!/usr/bin/env bash
printf 'config.sh called: %s\n' "$*" >>"${TRAP_STUB_LOG}"
STUB
chmod +x "${trap_scratch}/config.sh"

run_trap_subshell() {
    # $1: value to export as RUNNER_SELF_DEREGISTER, or "" to leave it unset.
    (
        cd "$trap_scratch"
        export TRAP_STUB_LOG="${trap_scratch}/config-calls.log"
        if [ -n "$1" ]; then
            export RUNNER_SELF_DEREGISTER="$1"
        else
            unset RUNNER_SELF_DEREGISTER 2>/dev/null || true
        fi
        # shellcheck disable=SC1090
        LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY=1 . "$entrypoint"
        runner_token="tok-for-trap-test"
        install_deregister_trap
    )
}

: >"${trap_scratch}/config-calls.log"
run_trap_subshell "0"
if [ -s "${trap_scratch}/config-calls.log" ]; then
    fail "cleanup ran ./config.sh remove even though RUNNER_SELF_DEREGISTER=0: $(cat "${trap_scratch}/config-calls.log")"
else
    note "trap is skipped when RUNNER_SELF_DEREGISTER=0"
fi

: >"${trap_scratch}/config-calls.log"
run_trap_subshell ""
if grep -q 'remove' "${trap_scratch}/config-calls.log"; then
    note "trap is installed and runs ./config.sh remove when RUNNER_SELF_DEREGISTER is unset"
else
    fail "trap did not run ./config.sh remove when RUNNER_SELF_DEREGISTER is unset"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall entrypoint token-resolution checks passed\n'
