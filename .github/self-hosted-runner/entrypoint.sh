#!/usr/bin/env bash
# Registers this container as an organisation-level self-hosted runner for
# LASTRADA-Software, then blocks running jobs until stopped.
#
# The registration token arrives on **stdin** from run-runner.sh, which mints
# a fresh one per start. It is deliberately not an environment variable:
# every job on this container can read `env` and /proc/1/environ, these are
# public repositories running contributor-authored build code, and a live
# registration token would let such code enrol its own machine into the
# runner group. RUNNER_TOKEN is still honoured as a fallback for the plain
# `docker run` path documented in README.md for Docker Desktop hosts, which
# have no systemd to run the wrapper.
#
# Environment:
#   RUNNER_TOKEN            - fallback registration token when stdin carries none
#   RUNNER_SCOPE_URL        - defaults to the LASTRADA-Software organisation
#   RUNNER_GROUP            - runner group to join; defaults to linux-docker
#   RUNNER_NAME             - defaults to "lastrada-docker-$HOSTNAME"
#   RUNNER_LABELS           - defaults to "self-hosted,Linux,X64,lastrada-docker"
#   RUNNER_SELF_DEREGISTER  - set to "0" to skip this container's own EXIT-time
#                             deregistration; defaults to enabled (see below)
set -euo pipefail

# Reads the first line of stdin when there is one, else falls back to the
# environment. Kept as a function so scripts/test_runner_entrypoint.sh can
# assert the precedence without running a registration.
read_runner_token() {
    local from_stdin=""
    if [ ! -t 0 ]; then
        IFS= read -r from_stdin || true
    fi
    if [ -n "$from_stdin" ]; then
        printf '%s' "$from_stdin"
    else
        printf '%s' "${RUNNER_TOKEN:-}"
    fi
}

# Removes this runner's registration; the body of the EXIT trap below.
cleanup() {
    echo "Removing runner registration..."
    ./config.sh remove --token "${runner_token}" || true
}

# Installs the cleanup trap unless the host is already doing this job.
#
# Under systemd, run-runner.sh sets RUNNER_SELF_DEREGISTER=0 and
# lastrada-runner@.service.in's ExecStop runs deregister-runner.sh on the
# HOST instead: this container's only credential is the REGISTRATION token
# it started with, and GitHub expires those after ~1 hour, so by the time a
# long-lived container is stopped this trap's own `config.sh remove` would
# fail here and strand the registration -- exactly what host-side ExecStop
# exists to avoid. Standalone (`docker run`, no systemd -- the path
# documented in README.md for Docker Desktop/WSL2), there is no host
# supervisor to do it, so this trap must remain the only mechanism;
# RUNNER_SELF_DEREGISTER unset here defaults to enabled, leaving that path
# unchanged.
#
# Kept as a function, like read_runner_token, so
# scripts/test_runner_entrypoint.sh can assert -- behaviourally, by actually
# letting the trap fire -- whether cleanup runs, not just grep the source for
# a string.
install_deregister_trap() {
    if [ "${RUNNER_SELF_DEREGISTER:-1}" = "0" ]; then
        return 0
    fi
    trap cleanup EXIT
}

# Sourced by the self-test: define the functions above, do nothing else.
if [ -n "${LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY:-}" ]; then
    return 0 2>/dev/null || exit 0
fi

RUNNER_SCOPE_URL="${RUNNER_SCOPE_URL:-https://github.com/LASTRADA-Software}"
RUNNER_GROUP="${RUNNER_GROUP:-linux-docker}"
RUNNER_NAME="${RUNNER_NAME:-lastrada-docker-${HOSTNAME}}"
RUNNER_LABELS="${RUNNER_LABELS:-self-hosted,Linux,X64,lastrada-docker}"

cd /home/runner/actions-runner

runner_token="$(read_runner_token)"
if [ -z "$runner_token" ]; then
    echo "ERROR: no registration token on stdin and RUNNER_TOKEN is unset." >&2
    exit 1
fi

install_deregister_trap

# A restart re-runs this entrypoint. Under systemd the container is always
# fresh (--rm), so there is nothing left on disk -- but the plain `docker run`
# path reuses one container across restarts, where .runner/.credentials from
# the previous registration survive and make `--replace` refuse with "Cannot
# configure the runner because it is already configured". Removing them is a
# local operation; it does not call the GitHub API.
rm -f .runner .credentials .credentials_rsaparams

./config.sh \
    --url "${RUNNER_SCOPE_URL}" \
    --token "${runner_token}" \
    --name "${RUNNER_NAME}" \
    --labels "${RUNNER_LABELS}" \
    --runnergroup "${RUNNER_GROUP}" \
    --work "_work" \
    --unattended \
    --replace

# Under systemd the host owns deregistration (run-runner.sh sets
# RUNNER_SELF_DEREGISTER=0, ExecStop runs deregister-runner.sh) and no EXIT
# trap is installed here -- so replace this shell with run.sh rather than
# running it as a child.
#
# That matters because Docker signals only PID 1. As a child, run.sh never
# sees SIGTERM: bash takes it instead and dies, tearing the container down
# and killing the runner abruptly under whatever job it was running. As
# PID 1 it receives the signal itself, and run.sh already traps it
# (`trap 'kill -INT -$PID' INT TERM`) to forward an interrupt to the runner
# process group -- the shutdown path the runner is designed for.
# TimeoutStopSec in the unit still bounds how long that may take.
#
# Standalone (`docker run`, no systemd) the EXIT trap IS the deregistration
# mechanism and `exec` would discard it, so that path keeps run.sh as a child.
if [ "${RUNNER_SELF_DEREGISTER:-1}" = "0" ]; then
    exec ./run.sh
fi

./run.sh
