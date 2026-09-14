#!/usr/bin/env bash
# Bootstraps a fresh Linux cloud VM (e.g. a Hetzner box) to run BOTH a
# fastcached compile-cache daemon and N self-hosted GitHub Actions runner
# containers for LASTRADA-Software/morph, sized dynamically from whatever
# CPU/RAM the machine actually has -- nothing here is a hardcoded "2 workers,
# 2 CPUs, 4 GB" assumption. Run as root (or with sudo) on a bare Ubuntu/Debian
# VM; safe to re-run (idempotent: reuses Docker if already installed, rebuilds
# images, replaces the existing fastcached container by name, reinstalls the
# systemd units that own the runner containers' lifecycle, and then restarts
# those units one at a time so a rebuilt runner image is actually picked up --
# never touching an already-written /etc/lastrada-runner/config, so an
# operator's hand-tuned settings, e.g. a lowered RUNNER_COUNT, survive a
# re-run instead of being reverted to this run's detected sizing).
#
# Usage:
#   ./bootstrap-cloud-node.sh
#
# No registration token is needed up front: each runner mints its own, fresh,
# on every start (see run-runner.sh). This box needs `gh` authenticated with
# admin:org on LASTRADA-Software instead -- a credential that stays on the
# host and never enters a container. The preflight below proves that with a
# real, read-only org-scoped API call (not just `gh auth status`, which only
# proves *some* credential is stored, not that it has admin:org against this
# org) -- and it runs that call through $SUDO, because the systemd units
# install-runner-units.sh installs run as whatever identity $SUDO resolves to
# (root, under sudo), so root's own `gh` credential is the one that actually
# has to be authenticated, not the invoking user's.
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

if ! $SUDO command -v gh >/dev/null 2>&1; then
    echo "ERROR: the gh CLI is required -- each runner mints its own" >&2
    echo "       registration token on every start." >&2
    exit 1
fi

# `gh auth status` is not enough here: it only proves some credential is
# stored, not that it has admin:org against LASTRADA-Software, nor that it
# belongs to the identity the systemd units actually run as. Query an
# endpoint that requires org-level self-hosted-runner read (admin:org) and
# mints/changes nothing, through $SUDO so it exercises the exact credential
# store install-runner-units.sh's units will use.
if ! $SUDO gh api orgs/LASTRADA-Software/actions/runners --jq '.total_count' >/dev/null 2>&1; then
    echo "ERROR: could not list LASTRADA-Software's self-hosted runners via gh." >&2
    echo "       The credential the runner units will use must be authenticated" >&2
    echo "       with admin:org on LASTRADA-Software." >&2
    if [ -n "$SUDO" ]; then
        echo "       This ran as root (via sudo), because that's the identity the" >&2
        echo "       systemd units install as -- so it's root's own gh credential" >&2
        echo "       that needs 'gh auth login', not the invoking user's." >&2
    fi
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

# --metrics: the image's own HEALTHCHECK is `fastcached --healthcheck`,
# which needs the running daemon to answer on its metrics endpoint --
# omitting this here means `docker ps` reports the container "(unhealthy)"
# forever even though the daemon itself is fine (confirmed live: dropping
# --metrics while overriding every other default CMD arg left the daemon
# genuinely serving requests but Docker's own health probe failing with
# exit 1 on every check). --metrics-bind=0.0.0.0 so the healthcheck can
# reach it the same way the daemon's cache port is reachable.
$SUDO docker run -d \
    --name fastcached \
    --restart unless-stopped \
    --cpus="${FASTCACHED_CPUS}" \
    --memory="${FASTCACHED_MEMORY_GB}g" \
    -p 6674:6674 \
    -v "${FASTCACHED_STORAGE_DIR}:/data" \
    fastcached:latest \
    --bind=0.0.0.0 \
    --metrics \
    --metrics-bind=0.0.0.0 \
    --max-memory="${FASTCACHED_MEMORY_GB}g" \
    --storage=/data \
    --storage-max-disk="${FASTCACHED_DISK_GB}g" \
    --storage-max-value=256m

echo "fastcached started: ${FASTCACHED_CPUS} CPU, ${FASTCACHED_MEMORY_GB}GiB RAM, ${FASTCACHED_DISK_GB}GiB disk cap, storage at ${FASTCACHED_STORAGE_DIR}"

# ── 6. Build the runner image ───────────────────────────────────────────
echo ""
echo "=== Building the lastrada self-hosted runner image ==="
$SUDO docker build -t lastrada-runner:latest "$MORPH_ROOT/.github/self-hosted-runner"

# ── 7. Configure and start the workers ──────────────────────────────────
# systemd supervises the containers, not Docker: one owner, a fresh
# registration token per start, and a bounded start limit so a broken
# credential shows up as a failed unit instead of retrying forever. The
# per-worker token minting that used to live here is gone -- run-runner.sh
# mints one per start, which covers the single-use property by construction.
echo ""
echo "=== Installing systemd units for ${WORKER_COUNT} worker(s) ==="

$SUDO mkdir -p /etc/lastrada-runner

# Never clobber a config an operator has tuned -- same rule and message style
# as install-runner-units.sh's own config.example handling. Without this, a
# re-run (the documented way to pick up an image fix, see section 8 below)
# would silently revert e.g. a hand-lowered RUNNER_COUNT back to whatever this
# run's hardware detection computes.
if [ ! -e /etc/lastrada-runner/config ]; then
    $SUDO tee /etc/lastrada-runner/config >/dev/null <<CONF
GITHUB_ORG=LASTRADA-Software
RUNNER_GROUP=linux-docker
RUNNER_COUNT=${WORKER_COUNT}
RUNNER_NAME_PREFIX=${RUNNER_NAME_PREFIX}
RUNNER_LABELS=self-hosted,Linux,X64,lastrada-docker
RUNNER_CPUS=${WORKER_CPUS}
RUNNER_MEMORY=${WORKER_MEM_GB}g
RUNNER_IMAGE=lastrada-runner:latest
FASTCACHE_ADDR=host.docker.internal:6674
CMAKE_BUILD_PARALLEL_LEVEL=${WORKER_CPUS}
CONF
    $SUDO chmod 0600 /etc/lastrada-runner/config
    echo "wrote /etc/lastrada-runner/config"
else
    echo "kept existing /etc/lastrada-runner/config (not overwriting operator-tuned settings)"
    echo "  This run detected ${WORKER_COUNT} worker(s) x ${WORKER_CPUS} CPU / ${WORKER_MEM_GB} GiB RAM each,"
    echo "  but that is NOT being applied. To change the running config, either edit"
    echo "  /etc/lastrada-runner/config directly, or delete it and re-run this script"
    echo "  to regenerate it from this machine's detected hardware."
fi

$SUDO bash "$MORPH_ROOT/.github/self-hosted-runner/install-runner-units.sh"

# ── 8. Restart the fleet so a rebuilt image is actually used ───────────────
# install-runner-units.sh's `systemctl enable --now` is a no-op on a unit
# that's already active, so on a re-run section 6 above may have just built a
# new lastrada-runner:latest image while every already-running runner keeps
# using the old one until something restarts it. Read RUNNER_COUNT back from
# the config on disk (not WORKER_COUNT computed in section 2) so this counts
# the fleet that's actually installed, including a kept, operator-tuned
# config from the guard above.
ACTUAL_RUNNER_COUNT="$(awk -F= '/^RUNNER_COUNT=/ {print $2}' /etc/lastrada-runner/config)"

echo ""
echo "=== Restarting ${ACTUAL_RUNNER_COUNT} runner unit(s) to pick up the rebuilt image ==="
echo "This interrupts any job currently running on each unit as it restarts."
echo "Restarting one at a time, not all at once, so the whole fleet is never"
echo "down simultaneously."
for i in $(seq 1 "$ACTUAL_RUNNER_COUNT"); do
    echo "restarting lastrada-runner@${i}"
    $SUDO systemctl restart "lastrada-runner@${i}.service"
done

echo ""
echo "=== Done ==="
echo "fastcached:      docker logs fastcached"
echo "workers:         systemctl status 'lastrada-runner@*'"
echo "worker logs:     journalctl -u 'lastrada-runner@1' -f"
echo "verify runners:  gh api orgs/LASTRADA-Software/actions/runners --jq '.runners[] | {name,status,busy}'"
