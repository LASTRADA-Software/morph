# Self-hosted Linux runner (Docker)

Runs repo-level GitHub Actions runners for `LASTRADA-Software/morph` inside
Docker containers. Used by `ci.yml`'s `linux-compilers`, `linux-sanitizers`,
and `linux-all-features` jobs whenever a runner is online and idle (see
**ci.yml integration** below).

Any number of hosts can register runners to this repo; they are
indistinguishable to `ci.yml` — a job lands on whichever is online and
idle. Which machines are currently registered is not recorded here (it
changes): read it off **Settings → Actions → Runners**, or
`gh api repos/LASTRADA-Software/morph/actions/runners`. Registrations for
hosts that no longer exist stay listed there as `offline` and are harmless
— the probe counts only online, non-busy ones — but are worth deleting so
the list reflects what actually runs.

Multiple runners exist so a multi-leg matrix (`linux-compilers` has 4,
`linux-sanitizers` has 3) actually runs its legs in parallel instead of
queueing behind each other on one runner process — each container is a
single runner that executes exactly one job at a time. The CPU cap exists
so 4 concurrent compiles cannot each try to claim the whole machine at
once (oversubscription would make every leg slower, not just share what's
already scarce).

The image is plain Ubuntu 24.04 with just the runner binary and enough
packages (`sudo`, `curl`, `git`, build-essential-adjacent tooling) to
bootstrap a toolchain — it does **not** prebake gcc/clang/sccache. Jobs
install those themselves the same way `ci.yml`'s GitHub-hosted jobs do, so
there is one place, not two, to keep compiler versions in sync. It does pin
a specific CMake (Kitware release, not the Ubuntu 24.04 apt package — see
the Dockerfile's own comment on a `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` false
positive that apt's 3.28.3 hits and GitHub-hosted's newer CMake doesn't) and
a modern `libstdc++-15-dev` (clang's default standard-library headers,
needed for C++23 `<print>` regardless of which matrix leg runs first).

## Requirements

- Docker, on any Linux x86_64 host (a cloud VM, a spare machine, WSL2/Docker
  Desktop on Windows). The container itself is Linux regardless of host OS.
- `gh` CLI authenticated with **admin** access to `LASTRADA-Software/morph`
  (needed only to mint the one-hour registration token below — not stored
  in the container or the image).

## Quick start (any host, including a fresh cloud VM)

```bash
# 1. Get the code onto the box (only these two files are needed, or clone
#    the whole repo — either works, since the image build doesn't touch
#    anything else in the tree).
git clone https://github.com/LASTRADA-Software/morph.git
cd morph/.github/self-hosted-runner

# 2. Build the image.
docker build -t morph-runner:latest .

# 3. Mint a short-lived registration token (expires ~1 hour; run this
#    right before step 4, not ahead of time). Requires gh auth with repo
#    admin — run this on a machine where you're logged in, e.g. your own
#    laptop, then copy just the token to the cloud box if `gh` isn't
#    authenticated there.
gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token'

# 4. Start the runner as a long-lived, auto-restarting container.
docker run -d \
  --name morph-runner \
  --restart unless-stopped \
  -e RUNNER_TOKEN="<token from step 3>" \
  morph-runner:latest
```

Check it registered:

```bash
docker logs morph-runner          # should end with "Listening for Jobs"
gh api repos/LASTRADA-Software/morph/actions/runners \
  --jq '.runners[] | {name,status,busy,labels:[.labels[].name]}'
```

It will show up in the repo under **Settings → Actions → Runners**, and
any workflow with `runs-on: [self-hosted, ...]` matching its labels can
pick up jobs on it — see the **Trust boundary** note below before wiring
one up.

## On a fresh cloud instance (e.g. Hetzner)

Same four steps as above, just from scratch on a bare VM:

```bash
# Docker isn't preinstalled on a plain Ubuntu/Debian Hetzner image.
curl -fsSL https://get.docker.com | sh

git clone https://github.com/LASTRADA-Software/morph.git
cd morph/.github/self-hosted-runner
docker build -t morph-runner:latest .

# Token: mint it from wherever you have gh admin auth (your laptop is
# fine) and paste it in here — it's short-lived and only used once.
docker run -d \
  --name morph-runner \
  --restart unless-stopped \
  -e RUNNER_TOKEN="<token>" \
  -e RUNNER_NAME="morph-hetzner-$(hostname)" \
  morph-runner:latest
```

`--restart unless-stopped` means the container comes back after a reboot
of the VM (Docker's own daemon is enabled by `get.docker.com` by default)
without anything else to configure.

That's the minimal path for one runner with no local cache daemon (it still
gets `fastcache-cc` if `FASTCACHE_ADDR` reaches some *other* machine's
`fastcached`, or falls back to `sccache`). To also run a `fastcached`
daemon on the same box, sized dynamically for however many CPUs/RAM it
actually has, use `bootstrap-cloud-node.sh` instead — see **Bootstrapping
a cloud node with its own fastcached** below.

## Bootstrapping a cloud node with its own fastcached

`bootstrap-cloud-node.sh` sets up a fresh Linux VM (bare Ubuntu/Debian —
this is what a Hetzner box ships) with both a `fastcached` daemon and N
self-hosted runner containers, all sized from the machine's actual
`nproc`/`/proc/meminfo` at run time — it does not hardcode a worker count
or per-worker CPU/RAM the way earlier revisions of this doc did. Run as
root (or with `sudo` available):

```bash
curl -fsSLo bootstrap-cloud-node.sh \
  https://raw.githubusercontent.com/LASTRADA-Software/morph/master/.github/self-hosted-runner/bootstrap-cloud-node.sh
chmod +x bootstrap-cloud-node.sh

RUNNER_TOKEN="$(gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token')" \
  ./bootstrap-cloud-node.sh
```

(`gh api ...` needs to run wherever you have `gh` authenticated with repo
admin — your laptop is fine; paste just the resulting token into
`RUNNER_TOKEN` on the cloud box if `gh` isn't set up there too.)

### The sizing rule

- `fastcached` gets a **fixed** 2 GiB RAM / 10 GiB on-disk cap and 1 CPU,
  regardless of machine size (`FASTCACHED_MEMORY_GB`/`FASTCACHED_DISK_GB`/
  `FASTCACHED_CPUS` env vars override this) — deliberately not scaled with
  the box, unlike the workers below.
- A small OS/Docker-daemon reserve is held back too: `max(1 GiB, 10% of
  total RAM)`, never handed to any container (`OS_RESERVE_MEM_GB`
  overrides).
- Whatever CPUs remain after fastcached's 1-CPU reservation are divided
  into workers of **2 CPUs each** (`WORKER_CPUS` overrides) —
  `worker_count = floor(remaining_cpus / 2)`. Each worker's RAM is the
  remaining RAM (after fastcached + the OS reserve) split evenly across
  that many workers.
- The script refuses to proceed rather than start an undersized setup: a
  box with fewer than 3 CPUs total (1 for fastcached + 2 for one worker)
  or where the RAM split would leave a worker under 1 GiB exits with an
  error instead of silently running something too small to compile C++
  in. Confirmed by hand: a 2-CPU/4GiB box is genuinely too small under
  this scheme (1 fastcached + one 2-CPU worker needs 3 CPUs minimum) —
  provision at least 4 CPUs to get one real worker with headroom.

### What it does, step by step

1. Installs Docker if not already present (`get.docker.com`).
2. Clones (or reuses) `morph` and `fastcached` checkouts.
3. Builds `fastcached`'s own image from its Dockerfile and starts it,
   capped per the sizing rule above, with persistent storage under
   `/var/lib/fastcached` (`FASTCACHED_STORAGE_DIR` overrides).
4. Builds this directory's runner image.
5. Starts each worker with `FASTCACHE_ADDR=host.docker.internal:6674` and
   `--add-host=host.docker.internal:host-gateway` — the latter is what
   makes `host.docker.internal` resolve to the Docker host's own IP on
   plain Linux Docker Engine (Docker Desktop provides this automatically;
   a bare Linux Engine needs the explicit `--add-host`). Note this env var
   only matters if `ci.yml`'s own job-level `FASTCACHE_ADDR` (see **ci.yml
   integration** below) is ever changed to read from the runner's
   environment instead of hardcoding the address — right now `ci.yml`
   always sends the literal `host.docker.internal:6674` itself, so this is
   what actually has to resolve on every runner host, cloud or otherwise.
6. Each worker after the first mints its own fresh registration token via
   `gh` (the one you passed in is single-use) — `gh` needs to be
   installed and authenticated on the cloud box itself for `WORKER_COUNT
   > 1`, or re-run per worker by hand with `WORKER_CPUS`/`FASTCACHED_CPUS`
   adjusted so the computed count is 1.

## Stopping / deregistering

```bash
docker stop morph-runner && docker rm morph-runner
```

`entrypoint.sh` traps `EXIT` and calls `./config.sh remove` before the
process exits, so a normal `docker stop` (which sends `SIGTERM`, not
`SIGKILL`) deregisters the runner from the repo automatically — you
should *not* see a stale offline runner left behind in the repo's runner
list. If the container is killed harder than that (host crash, `docker
kill`, out-of-memory), the cleanup trap doesn't run and the runner is
left registered but shows as offline; remove it manually via **Settings →
Actions → Runners** or `gh api -X DELETE
repos/LASTRADA-Software/morph/actions/runners/<id>`.

### Docker Desktop / host restarts

These are long-lived containers (`--restart unless-stopped`), not
recreated from scratch on every start — a Docker Desktop restart (or a
host reboot) re-runs `entrypoint.sh` against the **same** container
filesystem, with `.runner`/`.credentials` from the previous registration
still on disk. `entrypoint.sh` removes those files unconditionally before
calling `config.sh`, so the container always re-registers cleanly on
restart rather than restart-looping — confirmed live: before this fix, a
Docker Desktop restart left all 4 containers stuck in `Restarting (1)`,
each printing "Cannot configure the runner because it is already
configured" followed by a failed cleanup attempt (the old registration
was already gone server-side, so removing it 404's), forever, because
`--replace` alone did not get past that stale local state. If a container
is ever seen restart-looping despite this, `docker logs <name>` is the
first thing to check.

## Environment variables (`entrypoint.sh`)

| Variable        | Required | Default                                | Notes |
|-----------------|----------|-----------------------------------------|-------|
| `RUNNER_TOKEN`  | yes      | —                                        | Registration token, ~1 hour TTL. Mint a fresh one for each `docker run` / re-registration — it is not reusable after the runner has registered once, and a stale token just fails `config.sh`. |
| `RUNNER_NAME`   | no       | `morph-docker-<container hostname>`     | Set this explicitly (e.g. `morph-hetzner-1`) when running more than one runner, so they're distinguishable in the repo's runner list. |
| `RUNNER_LABELS` | no       | `self-hosted,Linux,X64,morph-docker`    | Only change this if you also update the `runs-on:` label list in the workflow(s) that should target it. |

## Trust boundary

A self-hosted runner executes arbitrary job code on whatever host runs the
container — a cloud VM here, or your own machine. `linux-compilers`,
`linux-sanitizers`, and `linux-all-features` inherit `ci.yml`'s top-level
`on: pull_request:` trigger with no fork restriction of their own, but
they cannot actually run on this runner from a forked PR: `probe-self-hosted`
picks the runner by reading the `RUNNER_STATUS_TOKEN` repo secret (see
**ci.yml integration** below), and a `pull_request` (not
`pull_request_target`) event triggered from a fork never receives repo
secrets at all — a GitHub platform guarantee, not something this
workflow implements itself. `RUNNER_STATUS_TOKEN` therefore comes through
empty for any forked PR, which the probe's own script already treats as
"no runner available" and falls back to `ubuntu-24.04` — the fork's build
still runs, just never on self-hosted hardware. No separate fork check is
needed as long as every self-hosted job keeps going through
`probe-self-hosted` rather than hardcoding `runs-on: [self-hosted, ...]`
directly.

## Running more than one runner

Each container is one runner process. To add capacity (another container
here, or a registration on a second machine), repeat the Quick start with
a distinct `RUNNER_NAME` per container/host — no coordination between them
is needed, they all just poll the same repo's job queue. On a host where
`host.docker.internal` resolves by itself (Docker Desktop), that is just
the Quick start in a loop:

```bash
for i in 1 2 3 4; do
  RUNNER_TOKEN=$(gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token')
  docker run -d \
    --name "morph-runner-$i" \
    --restart unless-stopped \
    --cpus=6 \
    -e RUNNER_TOKEN="$RUNNER_TOKEN" \
    -e RUNNER_NAME="morph-docker-$i" \
    morph-runner:latest
done
```

`--cpus=N` is a plain `docker run` flag, not anything `entrypoint.sh` or
the image needs to know about — size it to (host logical processors) ÷
(number of runner containers you want), leaving some headroom for the host
itself, and adjust down if the containers are still oversubscribing the
box under load.

On a **plain Linux Docker Engine** host two more flags are needed. A
12-processor / 61 GiB box running five containers, for example:

```bash
for i in 1 2 3 4 5; do
  lo=$(( (i-1)*2 )); hi=$(( lo+1 ))
  RUNNER_TOKEN=$(gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token')
  docker run -d \
    --name "morph-runner-$i" \
    --restart unless-stopped \
    --cpuset-cpus="${lo}-${hi}" \
    --memory=6g \
    --add-host=host.docker.internal:host-gateway \
    -e RUNNER_TOKEN="$RUNNER_TOKEN" \
    -e RUNNER_NAME="morph-docker-$i" \
    -e FASTCACHE_ADDR="host.docker.internal:6674" \
    -e CMAKE_BUILD_PARALLEL_LEVEL=2 \
    morph-runner:latest
done
```

- `--cpuset-cpus`, **not `--cpus`** — and this one is load bearing. `--cpus=N`
  is a CFS quota: it throttles the container without changing what it *sees*,
  so `nproc` inside a 2-CPU container on this 12-processor box still answers
  **12**. Ninja then starts ~14 parallel compiles per container, five
  containers make ~70 on 12 processors, and every one of them wants a
  gigabyte or so of C++23 template instantiation inside a 6 GiB cap.
  Measured, on the first run configured that way: **32 OOM kills** across the
  five containers (`memory.events`), five red jobs, and `g++: fatal error:
  Killed signal terminated program cc1plus` in every one. `--cpuset-cpus`
  pins actual processors, so `sched_getaffinity` — and therefore `nproc`,
  ninja, `ctest -j` and `clang-tidy-diff`'s own `-j "$(nproc)"` — all agree
  with reality. Disjoint sets per container, leaving the top two processors
  for the host and `fastcached`.
- `-e CMAKE_BUILD_PARALLEL_LEVEL=2` — belt and braces over the above, since
  every build step in `ci.yml` goes through `cmake --build --preset`.
- `--add-host=host.docker.internal:host-gateway` — Docker Desktop provides
  that name automatically, a bare Linux Engine does not, and `ci.yml` sends
  the literal `host.docker.internal:6674` to every self-hosted job. Without
  it the address does not resolve and every job silently compiles uncached.
- `--memory=Ng` — a cap, not a reservation, and worth setting on a machine
  that is also somebody's desktop: it bounds what a runaway link step can
  take from the host rather than letting the OOM killer choose.

The registration token is single-use, which is why it is minted inside the
loop rather than once before it.

Any host that starts its containers with `--cpus=N` instead carries the
same latent hazard — the box's full logical-processor count is reported to
every build inside an N-CPU container. It only bites where a memory cap is
also set: without one the oversubscription costs wall-clock rather than
killed compiles.

## ci.yml integration

None of `linux-compilers`, `linux-sanitizers`, or `linux-all-features`
hardcode `runs-on:`. A `probe-self-hosted` job that runs first checks the
runners API for an online, non-busy runner labeled `morph-docker` and
outputs the label set each of them should use — self-hosted if one is
free, otherwise the plain `ubuntu-24.04` GitHub-hosted label. Nothing
needs to be started or stopped by hand for this fallback to work; it's
just naturally in effect whenever no morph-docker runner happens to be
online or all of them are busy on another job.

The one piece that doesn't come for free: `GITHUB_TOKEN` cannot call the
runners API — `GET /repos/.../actions/runners` is a repo-admin operation
regardless of what the workflow's `permissions:` block grants. The probe
job instead reads a repo secret named `RUNNER_STATUS_TOKEN`, which must
be a **fine-grained PAT scoped to this repo only, with the
"Administration: Read-only" permission** (nothing else — it cannot
register, delete, or otherwise manage runners, and has no code access).
Set it up once at **Settings → Secrets and variables → Actions → New
repository secret**. Until that secret exists, the probe always falls
back to `ubuntu-24.04` — nothing breaks, the jobs above just never pick up
the self-hosted path.

Forked-repo pull requests never receive repo secrets at all (GitHub
withholds them for security), so `RUNNER_STATUS_TOKEN` reads as empty
there and the probe falls back the same way — no separate handling
needed for that case.

`linux-coverage` (split out of `linux-sanitizers`'s old 4th matrix leg) and
the remaining Linux jobs (valgrind, Qt, ladder tests, clang-tidy) are
intentionally left on `ubuntu-24.04` for now.

## Dependency clones and HTTP/2

`CMakeLists.txt` falls back to `FetchContent` for glaze when no installed
copy is found, so every Linux configure step does one anonymous
`git clone https://github.com/stephenberry/glaze.git` — the only
unauthenticated clone in the build. Inside this image that clone fails
most of the time: GitHub answers the `info/refs` GET with 200 and then
the `git-upload-pack` POST on the same reused HTTP/2 connection with a
spurious `401` and `www-authenticate: Basic realm="GitHub"`, which
surfaces as

```
fatal: could not read Username for 'https://github.com': No such device or address
fatal: expected flush after ref listing
Had to git clone more than once: 3 times.
CMake Error ... Failed to clone repository: 'https://github.com/stephenberry/glaze.git'
```

and fails Configure. It looks like a credentials or rate-limit problem and
is neither: it is Ubuntu 24.04's libcurl 8.5.0 / nghttp2 1.59 speaking
HTTP/2. Measured from a running runner container, ~7 of 10 `ls-remote`s
fail; with `-c http.version=HTTP/1.1` or `-c protocol.version=0`, 10 of
10 succeed. The same clone from the Docker host (same public address,
libcurl 8.21) is 10 of 10, and upgrading git inside the container to 2.55
from `ppa:git-core/ppa` changes nothing — the libcurl underneath is the
same, and 24.04 has no newer one to install. Only *authenticated*
requests escape the 401, because git retries them with credentials, which
is why `actions/checkout` has always worked here and only the dependency
clone breaks.

The Dockerfile therefore pins `git config --system http.version HTTP/1.1`.
Containers built from an older image keep failing until they are
recreated; to fix a running one in place, without disturbing the job it
may be executing:

```bash
docker exec -u root morph-runner-1 git config --system http.version HTTP/1.1
```

## Compiler cache: fastcache-cc

`linux-compilers`, `linux-sanitizers`, and `linux-all-features` each set
`FASTCACHE_ADDR=host.docker.internal:6674` and `FASTCACHE_AUTO_INSTALL=ON`
as job-level env — but **only** when `probe-self-hosted` chose the
self-hosted path; both are left empty/OFF on the GitHub-hosted fallback,
where `host.docker.internal` does not resolve (it isn't a Docker
container) and morph's `cmake/CompileCache.cmake` has no daemon to reach
anyway.

`host.docker.internal:6674` is the same literal address on every host --
`ci.yml` hardcodes it, it does not vary by which runner picks up the job --
which means **every host running these runners needs its own `fastcached`
daemon reachable at that address from inside its containers**. There is
no single shared cache across hosts; each host caches its own compiles.
How `host.docker.internal` resolves differs by platform:

- **Docker Desktop (Windows/macOS hosts)**: resolves automatically to
  whatever the host's `127.0.0.1` means — i.e. that machine's own
  `fastcached` service (kept outside this repository). That service must be **running** and its
  `fastcached.yaml` must **bind `0.0.0.0`**, not the default `127.0.0.1`,
  or a container cannot reach it at all (`127.0.0.1` inside a container
  means the container itself).
- **Plain Linux Docker Engine (a workstation or a cloud VM)**: does
  **not** provide `host.docker.internal` automatically the way Docker
  Desktop does. `bootstrap-cloud-node.sh` adds it explicitly via
  `--add-host=host.docker.internal:host-gateway` on each worker
  container — confirmed live (`REACHABLE` from inside a worker to
  `fastcached` on the same box). A worker started without that flag on a
  Linux host would fail to resolve the address and fall through to
  `sccache` instead (never a hard failure, just a slower cache).
- **A Linux workstation that already runs `fastcached` for its own
  builds**: the daemon there is typically bound to `127.0.0.1` and built
  from source, and neither is usable by a container as-is. See the two
  subsections below.

### A workstation that already runs its own fastcached

A developer machine's `fastcached` listens on loopback, which inside a
container means the container itself. Two ways to give the runners a cache
on such a host; the second is what these runners are set up for:

1. **Add a listener on the Docker bridge gateway** to the existing daemon —
   `listeners:` in `fastcached.yaml` supersedes `bind`/`port`, so both
   endpoints must be spelled out:

   ```yaml
   listeners:
     - address: 127.0.0.1
       port: 6674
     - address: 172.17.0.1     # what host-gateway resolves to
       port: 6674
   ```

   One cache serving both the developer's builds and the runners. Requires
   the version match described below, which a from-source daemon does not
   have.

2. **Run a second daemon, as a host process**, bound to the bridge address
   only and left on a released version, with the personal one untouched on
   loopback — e.g. as a `systemd --user` unit (`fastcached-ci.service`)
   running the 0.1.1 release binary:

   ```yaml
   # ~/.local/opt/fastcached-0.1.1/fastcached-ci.yaml
   listeners:
     - address: 172.17.0.1     # the bridge gateway, not the LAN
       port: 6674
   storage_path: /home/<user>/.local/state/fastcached-ci/cache
   storage_max_disk: 30g
   storage_max_value: 256m
   max_memory: 8g
   metrics: true               # loopback-only, on a port the personal
   metrics_bind: 127.0.0.1     # daemon does not use, so cache behaviour
   metrics_port: 9260          # on the runners can actually be measured
   ```

   A daemon rather than a container, deliberately: the runners reach it at
   `host.docker.internal:6674` either way, and a host process is one less
   moving part than a container publishing a port back to its own host. Give
   it **its own `storage_path`** — started without `--config` it would read
   the personal `fastcached.yaml` and two daemons would write one cache file.

   The Ubuntu release binary links `libyaml-cpp 0.8`; a distribution that
   ships 0.9 (Arch, for one) can vendor just that library beside the binary
   and point `LD_LIBRARY_PATH` at it from the unit, rather than installing
   anything system-wide or building from source.

   Two caches on one host, which costs nothing that matters: the runners
   compile in their own checkouts and share almost no entries with the
   developer's tree anyway. The two daemons do not collide — one binds
   `127.0.0.1:6674`, the other `172.17.0.1:6674`.

### The daemon and the launcher have to be version-compatible

`FASTCACHE_AUTO_INSTALL=ON` fetches the newest **released** `fastcache-cc`
from the `fastcached` project's GitHub Releases. A daemon built from that
project's `master` can be far ahead of its last release and **refuses that
client**, which is why the daemon serving the runners has to be a release
build, not whatever from-source one is already on the box.

Measured here: `fastcached 0.1.1-599-gea414a2` (a from-source master build)
against the auto-installed `fastcache-cc` 0.1.1, configuring from inside a
runner container —

```
-- [cache] Auto-installed fastcache-cc (/home/runner/.cache/fastcache-cc/0.1.1/Linux-x86_64/fastcache-cc)
-- [cache] Not using fastcache-cc at host.docker.internal:6674: rejected (unsupported-version
-- [cache] No other compiler-cache launcher found (sccache, ccache); caching disabled
```

The same container against a `fastcached` 0.1.1 release build selects it,
and a second build from a wiped build directory hits the cache
(`fastcached_get_hits_total` 0 → 3). Note the last line of the failing run:
on the self-hosted path `ci.yml` deliberately does not install `sccache`,
so a rejected `fastcache-cc` is not a slower cache, it is **no cache at
all** — and it says so in the configure log rather than failing, which is
what makes it easy to miss.

This applies to `bootstrap-cloud-node.sh` too, which builds `fastcached`
from a `master` checkout.

If a host has no `fastcached` reachable at all (or `FASTCACHE_ADDR` is
left unset there), the module falls back to `sccache` (already installed
by every job) with zero other changes needed; see
`cmake/CompileCache.cmake`'s own header comment for the full
fastcache-cc → sccache → ccache → none preference order.

`FASTCACHE_AUTO_INSTALL=ON` is what lets this work without prebaking
`fastcache-cc` into the runner image: on first configure, CMake downloads
a prebuilt `fastcache-cc` binary from the `fastcached` project's own
GitHub Releases (cached per-user, per-version, so this costs one download
per container, not per build) — see
`cmake/CompileCache.cmake`'s "Optional auto-install" section for exactly
how, including its own SHA256 verification and total inability to fail a
configure (a fetch that fails just falls through to sccache).
