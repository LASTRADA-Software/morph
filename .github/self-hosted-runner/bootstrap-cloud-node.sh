#!/usr/bin/env bash
# Bootstraps a fresh Linux cloud VM (e.g. a Hetzner box) to run BOTH a
# fastcached compile-cache daemon and N self-hosted GitHub Actions runner
# containers for LASTRADA-Software/morph, sized dynamically from whatever
# CPU/RAM the machine actually has -- nothing here is a hardcoded "2 workers,
# 2 CPUs, 4 GB" assumption. Run as root (or with sudo) on a bare Ubuntu/Debian
# VM; safe to re-run (idempotent: reuses Docker if already installed, rebuilds
# images, replaces existing morph-runner-*/fastcached containers by name).
#
# Usage:
#   RUNNER_TOKEN=... ./bootstrap-cloud-node.sh
#
# Required env var:
#   RUNNER_TOKEN   - a fresh registration token from
#                    POST /repos/LASTRADA-Software/morph/actions/runners/registration-token
#                    (expires ~1 hour; mint it right before running this
#                    script, e.g. `gh api -X POST
#                    repos/LASTRADA-Software/morph/actions/runners/registration-token
#                    --jq '.token'` from a machine with gh admin auth).
#
# Optional env vars (override the dynamic sizing below):
#   RUNNER_NAME_PREFIX     - defaults to "morph-cloud-$(hostname)"
#   FASTCACHED_MEMORY_GB   - defaults to 2 (fixed, not scaled with machine size)
#   FASTCACHED_DISK_GB     - defaults to 10
#   FASTCACHED_CPUS        - defaults to 1
#   FASTCACHED_STORAGE_DIR - defaults to /var/lib/fastcached (persistent cache
#                             storage, survives container recreation)
#   WORKER_CPUS            - defaults to 2 (each worker gets this many)
#   OS_RESERVE_MEM_GB      - defaults to max(1, 10% of total RAM), left
#                             unallocated to any container for the OS/Docker
#                             daemon/sshd/etc.
#
# fastcached's --storage-max-value is fixed at 256m (matches the maintainer's
# already-tuned Windows daemon config -- debug objects have been observed up
# to ~122 MB) rather than left at whatever the daemon's own compiled-in
# default is.
set -euo pipefail

if [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
    echo "ERROR: run as root, or install sudo first." >&2
    exit 1
fi
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

if [ -z "${RUNNER_TOKEN:-}" ]; then
    echo "ERROR: RUNNER_TOKEN is not set. Mint one with:" >&2
    echo "  gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token'" >&2
    exit 1
fi

RUNNER_NAME_PREFIX="${RUNNER_NAME_PREFIX:-morph-cloud-$(hostname)}"
FASTCACHED_MEMORY_GB="${FASTCACHED_MEMORY_GB:-2}"
FASTCACHED_DISK_GB="${FASTCACHED_DISK_GB:-10}"
FASTCACHED_CPUS="${FASTCACHED_CPUS:-1}"
WORKER_CPUS="${WORKER_CPUS:-2}"

# ── 1. Detect this machine's actual resources ──────────────────────────────
# Never hardcode a worker count/size here -- the whole point of this script
# is that it reads the box it's actually running on, so the same script
# works on a 2-CPU/4GB dev box and a 16-CPU/64GB one without editing anything.
TOTAL_CPUS="$(nproc)"
TOTAL_MEM_KB="$(awk '/MemTotal/ {print $2}' /proc/meminfo)"
TOTAL_MEM_GB=$(( TOTAL_MEM_KB / 1024 / 1024 ))

OS_RESERVE_MEM_GB="${OS_RESERVE_MEM_GB:-$(( TOTAL_MEM_GB / 10 > 1 ? TOTAL_MEM_GB / 10 : 1 ))}"

echo "Detected: ${TOTAL_CPUS} CPUs, ${TOTAL_MEM_GB} GiB RAM."
echo "Reserving: fastcached=${FASTCACHED_CPUS} CPU/${FASTCACHED_MEMORY_GB}GiB, OS/Docker=${OS_RESERVE_MEM_GB}GiB."

# ── 2. Work out how many workers fit in what's left ────────────────────────
REMAINING_CPUS=$(( TOTAL_CPUS - FASTCACHED_CPUS ))
REMAINING_MEM_GB=$(( TOTAL_MEM_GB - FASTCACHED_MEMORY_GB - OS_RESERVE_MEM_GB ))

if [ "$REMAINING_CPUS" -lt "$WORKER_CPUS" ]; then
    echo "ERROR: only ${REMAINING_CPUS} CPU(s) left after fastcached's reservation," >&2
    echo "       but each worker needs ${WORKER_CPUS}. Lower WORKER_CPUS or" >&2
    echo "       FASTCACHED_CPUS, or use a bigger machine." >&2
    exit 1
fi

WORKER_COUNT=$(( REMAINING_CPUS / WORKER_CPUS ))

if [ "$WORKER_COUNT" -lt 1 ]; then
    echo "ERROR: computed 0 workers (remaining CPUs=${REMAINING_CPUS}, per-worker=${WORKER_CPUS})." >&2
    exit 1
fi

WORKER_MEM_GB=$(( REMAINING_MEM_GB / WORKER_COUNT ))

if [ "$WORKER_MEM_GB" -lt 1 ]; then
    echo "ERROR: ${WORKER_COUNT} workers would get ${WORKER_MEM_GB} GiB RAM each --" >&2
    echo "       too little for a real C++ compile. Reduce WORKER_COUNT (lower" >&2
    echo "       total CPUs / raise WORKER_CPUS) or use a bigger machine." >&2
    exit 1
fi

echo "Plan: ${WORKER_COUNT} worker(s) x ${WORKER_CPUS} CPU / ${WORKER_MEM_GB} GiB RAM each."
echo "      (uses $(( FASTCACHED_CPUS + WORKER_COUNT * WORKER_CPUS ))/${TOTAL_CPUS} CPUs," \
     "$(( FASTCACHED_MEMORY_GB + WORKER_COUNT * WORKER_MEM_GB + OS_RESERVE_MEM_GB ))/${TOTAL_MEM_GB} GiB RAM)"

# ── 3. Docker ────────────────────────────────────────────────────────────
if ! command -v docker >/dev/null 2>&1; then
    echo "Installing Docker..."
    curl -fsSL https://get.docker.com | $SUDO sh
fi

# ── 4. Get the source ────────────────────────────────────────────────────
# Always a fixed, well-known path rather than "detect whether this script
# happens to be sitting inside a checkout already" -- this script is meant
# to be fetched standalone (curl, see the README's own quick-start), where
# there is no surrounding checkout to detect, and a relative-path heuristic
# would be one more thing to get subtly wrong depending on where it's run
# from. MORPH_ROOT/FASTCACHED_ROOT still override for anyone who already
# has a checkout somewhere specific.
MORPH_ROOT="${MORPH_ROOT:-$HOME/morph}"
if [ ! -d "$MORPH_ROOT/.git" ]; then
    git clone https://github.com/LASTRADA-Software/morph.git "$MORPH_ROOT"
else
    git -C "$MORPH_ROOT" pull --ff-only
fi

FASTCACHED_ROOT="${FASTCACHED_ROOT:-$HOME/fastcached}"
if [ ! -d "$FASTCACHED_ROOT/.git" ]; then
    git clone https://github.com/LASTRADA-Software/fastcached.git "$FASTCACHED_ROOT"
else
    git -C "$FASTCACHED_ROOT" pull --ff-only
fi

# ── 5. Build and start fastcached ───────────────────────────────────────
# Built from source (the Dockerfile in fastcached's own repo) rather than
# downloading the prebuilt release binary used for fastcache-cc: the daemon
# needs to keep running as a long-lived container, which is what its own
# Dockerfile is already set up for, including the runtime-stage slimming.
echo ""
echo "=== Building fastcached ==="
$SUDO docker build -t fastcached:latest "$FASTCACHED_ROOT"

$SUDO docker rm -f fastcached 2>/dev/null || true
FASTCACHED_STORAGE_DIR="${FASTCACHED_STORAGE_DIR:-/var/lib/fastcached}"
$SUDO mkdir -p "$FASTCACHED_STORAGE_DIR"

$SUDO docker run -d \
    --name fastcached \
    --restart unless-stopped \
    --cpus="${FASTCACHED_CPUS}" \
    --memory="${FASTCACHED_MEMORY_GB}g" \
    -p 6674:6674 \
    -v "${FASTCACHED_STORAGE_DIR}:/data" \
    fastcached:latest \
    --bind=0.0.0.0 \
    --max-memory="${FASTCACHED_MEMORY_GB}g" \
    --storage=/data \
    --storage-max-disk="${FASTCACHED_DISK_GB}g" \
    --storage-max-value=256m

echo "fastcached started: ${FASTCACHED_CPUS} CPU, ${FASTCACHED_MEMORY_GB}GiB RAM, ${FASTCACHED_DISK_GB}GiB disk cap, storage at ${FASTCACHED_STORAGE_DIR}"

# ── 6. Build the runner image ───────────────────────────────────────────
echo ""
echo "=== Building the morph self-hosted runner image ==="
$SUDO docker build -t morph-runner:latest "$MORPH_ROOT/.github/self-hosted-runner"

# ── 7. Start the workers ────────────────────────────────────────────────
# FASTCACHE_ADDR here is the loopback address, not host.docker.internal:
# fastcached and the runner containers are on the same Docker host (this
# machine), on the default bridge network, so each runner container reaches
# fastcached the same way it would reach any other service on this box's own
# network stack -- via the host's IP, which Docker's bridge networking makes
# reachable as host.docker.internal on Linux too (Docker Engine >= 20.10
# adds this automatically via --add-host on Linux; belt-and-suspenders
# added explicitly below in case an older Engine is on this image).
echo ""
echo "=== Starting ${WORKER_COUNT} runner worker(s) ==="
for i in $(seq 1 "$WORKER_COUNT"); do
    WORKER_TOKEN="$RUNNER_TOKEN"
    if [ "$i" -gt 1 ]; then
        echo "Minting a fresh registration token for worker $i (the one" \
             "passed in on RUNNER_TOKEN is single-use)..."
        if command -v gh >/dev/null 2>&1; then
            WORKER_TOKEN="$(gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token')"
        else
            echo "ERROR: gh CLI not available to mint additional tokens for worker $i." >&2
            echo "       Re-run with RUNNER_TOKEN set to a fresh token and adjust" >&2
            echo "       WORKER_CPUS/FASTCACHED_CPUS to get WORKER_COUNT=1, or" >&2
            echo "       install/auth gh on this box first." >&2
            exit 1
        fi
    fi

    NAME="${RUNNER_NAME_PREFIX}-${i}"
    $SUDO docker rm -f "$NAME" 2>/dev/null || true
    $SUDO docker run -d \
        --name "$NAME" \
        --restart unless-stopped \
        --cpus="${WORKER_CPUS}" \
        --memory="${WORKER_MEM_GB}g" \
        --add-host=host.docker.internal:host-gateway \
        -e RUNNER_TOKEN="$WORKER_TOKEN" \
        -e RUNNER_NAME="$NAME" \
        -e FASTCACHE_ADDR="host.docker.internal:6674" \
        morph-runner:latest
    echo "started $NAME (${WORKER_CPUS} CPU, ${WORKER_MEM_GB}GiB RAM)"
done

echo ""
echo "=== Done ==="
echo "fastcached:      docker logs fastcached"
echo "workers:         docker ps --filter name=${RUNNER_NAME_PREFIX}"
echo "verify runners:  gh api repos/LASTRADA-Software/morph/actions/runners --jq '.runners[] | {name,status,busy}'"
