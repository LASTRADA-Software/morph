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
#   RUNNER_TOKEN      - fallback registration token when stdin carries none
#   RUNNER_SCOPE_URL  - defaults to the LASTRADA-Software organisation
#   RUNNER_GROUP      - runner group to join; defaults to linux-docker
#   RUNNER_NAME       - defaults to "lastrada-docker-<hostname>"
#   RUNNER_LABELS     - defaults to "self-hosted,Linux,X64,lastrada-docker"
set -euo pipefail

RUNNER_SCOPE_URL="${RUNNER_SCOPE_URL:-https://github.com/LASTRADA-Software}"
RUNNER_GROUP="${RUNNER_GROUP:-linux-docker}"
RUNNER_NAME="${RUNNER_NAME:-lastrada-docker-$(hostname)}"
RUNNER_LABELS="${RUNNER_LABELS:-self-hosted,Linux,X64,lastrada-docker}"

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

# Sourced by the self-test: define the functions above, do nothing else.
if [ -n "${LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY:-}" ]; then
    return 0 2>/dev/null || exit 0
fi

cd /home/runner/actions-runner

runner_token="$(read_runner_token)"
if [ -z "$runner_token" ]; then
    echo "ERROR: no registration token on stdin and RUNNER_TOKEN is unset." >&2
    exit 1
fi

cleanup() {
    echo "Removing runner registration..."
    ./config.sh remove --token "${runner_token}" || true
}
trap cleanup EXIT

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

./run.sh
