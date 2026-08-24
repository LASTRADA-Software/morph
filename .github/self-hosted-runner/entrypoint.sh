#!/usr/bin/env bash
# Registers this container as a repo-level self-hosted runner for
# LASTRADA-Software/morph, then blocks running jobs until stopped.
#
# Required env vars:
#   RUNNER_TOKEN   - registration token from
#                    POST /repos/LASTRADA-Software/morph/actions/runners/registration-token
#                    (expires after ~1 hour; generate a fresh one per (re)registration)
# Optional env vars:
#   RUNNER_NAME    - defaults to "morph-docker-<hostname>"
#   RUNNER_LABELS  - defaults to "self-hosted,Linux,X64,morph-docker"
set -euo pipefail

REPO_URL="https://github.com/LASTRADA-Software/morph"
RUNNER_NAME="${RUNNER_NAME:-morph-docker-$(hostname)}"
RUNNER_LABELS="${RUNNER_LABELS:-self-hosted,Linux,X64,morph-docker}"

cd /home/runner/actions-runner

cleanup() {
    echo "Removing runner registration..."
    ./config.sh remove --token "${RUNNER_TOKEN}" || true
}
trap cleanup EXIT

if [ -z "${RUNNER_TOKEN:-}" ]; then
    echo "ERROR: RUNNER_TOKEN is not set." >&2
    exit 1
fi

./config.sh \
    --url "${REPO_URL}" \
    --token "${RUNNER_TOKEN}" \
    --name "${RUNNER_NAME}" \
    --labels "${RUNNER_LABELS}" \
    --work "_work" \
    --unattended \
    --replace

./run.sh
