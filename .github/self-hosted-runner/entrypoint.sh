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

# ci.yml's Linux jobs install their own toolchain per run (sccache, gcc-15,
# etc.) into paths that persist for this container's entire lifetime -- unlike
# a GitHub-hosted VM, which is thrown away after one job. A leftover sccache
# binary from an earlier job on this same container satisfies
# CompileCache.cmake's find_program(SCCACHE) even on a job that never
# installed it itself, which permanently blocks its FASTCACHE_AUTO_INSTALL
# auto-install path (that guard requires NONE of fastcache-cc/sccache/ccache
# to already be on PATH). Wiping it here means every container starts each
# registration genuinely clean of it, rather than carrying forward whatever a
# previous job happened to leave behind.
rm -f /usr/local/bin/sccache

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
