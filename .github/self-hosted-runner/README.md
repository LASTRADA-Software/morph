# Self-hosted Linux runner (Docker)

Runs **organisation-level** GitHub Actions runners for `LASTRADA-Software`
inside Docker containers, supervised by systemd. Used by `ci.yml`'s
`linux-compilers`, `linux-sanitizers`, `linux-all-features`, `ladder-tests`,
and `ladder-sanitizers` jobs whenever a runner is online and idle (see
**ci.yml integration** below).

## Which repositories use this

The runners register against the **`LASTRADA-Software` organisation**, not a
single repository, and belong to the `linux-docker` runner group. The group
is scoped to `morph`, `fastcached` and `Lightweight` and has
`allows_public_repositories: true` — which it must, because all three are
public and the organisation's `Default` group does not allow public
repositories at all.

Adding a fourth repository is one call against the group; no
re-registration and no restart:

```bash
gh api -X PUT \
  "orgs/LASTRADA-Software/actions/runner-groups/<group-id>/repositories/<repo-id>"
```

A consuming repository needs a `RUNNER_STATUS_TOKEN` secret with
organisation self-hosted-runner read access (`admin:org`, or a fine-grained
PAT with "Self-hosted runners: Read-only"), and a `probe-self-hosted` job
like the one in this repository's `ci.yml`. Without the secret the probe
falls back to GitHub-hosted, which is also what forked-repo PRs get.

Any number of hosts can register runners to the organisation; they are
indistinguishable to `ci.yml` — a job lands on whichever is online and
idle. Which machines are currently registered is not recorded here (it
changes): read it off the organisation's **Settings → Actions → Runners**,
or `gh api orgs/LASTRADA-Software/actions/runners`. Registrations for hosts
that no longer exist stay listed there as `offline` and are harmless — the
probe counts only online, non-busy ones — but are worth deleting so the
list reflects what actually runs.

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
- systemd, to supervise the runner containers via
  `lastrada-runner@N.service` (see **Quick start** below). A host without
  it (Docker Desktop, WSL2) falls back to the plain `docker run` path
  documented under **Docker Desktop / WSL2 (no systemd)**.
- `gh` CLI authenticated with **`admin:org`** access on `LASTRADA-Software`
  (needed because the runners are organisation-level, not repo-level —
  each start mints its own one-hour registration token via
  `run-runner.sh`; the credential stays on the host and never enters a
  container).

## Quick start (any host, including a fresh cloud VM)

```bash
# 1. Get the code onto the box. Cloning the whole repo is simplest; step 2
#    needs Dockerfile, entrypoint.sh and job-started-hook.sh, and step 3
#    needs install-runner-units.sh plus config.example,
#    lastrada-runner@.service.in, run-runner.sh and deregister-runner.sh
#    alongside it -- all from this same directory, so a partial copy has
#    to bring all eight.
git clone https://github.com/LASTRADA-Software/morph.git
cd morph/.github/self-hosted-runner

# 2. Build the image.
docker build -t lastrada-runner:latest .

# 3. Install the systemd units (already in this directory from step 1). No
#    token needed -- each runner mints its own on every start. Needs `gh`
#    authenticated with admin:org on LASTRADA-Software; that credential
#    stays on this host and never enters a container.
./install-runner-units.sh

# 4. Check it registered.
systemctl --user status 'lastrada-runner@*'
gh api orgs/LASTRADA-Software/actions/runners \
  --jq '.runners[] | {name,status,busy,labels:[.labels[].name]}'
```

It will show up in the organisation under **Settings → Actions →
Runners**, in the `linux-docker` group, and any workflow (in `morph`,
`fastcached`, or `Lightweight`) with `runs-on: [self-hosted, ...]` matching
its labels can pick up jobs on it — see the **Trust boundary** note below
before wiring one up.

## On a fresh cloud instance (e.g. Hetzner)

Same steps as **Quick start** above, from scratch on a bare VM, plus
installing Docker itself:

```bash
# Docker isn't preinstalled on a plain Ubuntu/Debian Hetzner image.
curl -fsSL https://get.docker.com | sh

git clone https://github.com/LASTRADA-Software/morph.git
cd morph/.github/self-hosted-runner
docker build -t lastrada-runner:latest .

# gh needs to be authenticated with admin:org on LASTRADA-Software on this
# box -- run-runner.sh (invoked by systemd on every start, including every
# restart) mints its own registration token locally each time.
sudo ./install-runner-units.sh
```

`install-runner-units.sh` writes `/etc/lastrada-runner/config` from
`config.example` the first time it runs and never overwrites it again
(it prints `kept existing ...` on a re-run instead).  `config.example`'s
defaults (`RUNNER_COUNT=5`, `RUNNER_CPUS=2`, `RUNNER_MEMORY=6g`) are sized
for the maintainer's own 12-CPU host, not necessarily this VM — edit that
file to match before or after the first install, then re-run
`install-runner-units.sh` to pick up the change (it also disables any
instance left over above the new `RUNNER_COUNT`).

Working out that sizing by hand is what `bootstrap-cloud-node.sh` automates
instead — see **Bootstrapping a cloud node with its own fastcached** below,
which also sets up a local `fastcached` daemon.

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

./bootstrap-cloud-node.sh
```

No registration token to mint or pass in — `gh` just needs to be
installed and authenticated *on this box*, with `admin:org` on
`LASTRADA-Software`, before running the script (it checks `gh auth status`
up front and refuses otherwise). Unlike the token, this can't be minted
elsewhere and copied over: every runner start from here on, including
every restart, calls `gh` locally via `run-runner.sh`.

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
2. Clones (or reuses, `git pull --ff-only`) `morph` and `fastcached`
   checkouts under `$HOME` (`MORPH_ROOT`/`FASTCACHED_ROOT` override).
3. Builds `fastcached`'s own image from its Dockerfile and starts it,
   capped per the sizing rule above, with persistent storage under
   `/var/lib/fastcached` (`FASTCACHED_STORAGE_DIR` overrides).
4. Builds this directory's runner image, tagged `lastrada-runner:latest`.
5. Writes `/etc/lastrada-runner/config` with the computed sizing
   (`GITHUB_ORG=LASTRADA-Software`, `RUNNER_GROUP=linux-docker`,
   `RUNNER_COUNT`, per-worker `RUNNER_CPUS`/`RUNNER_MEMORY`,
   `FASTCACHE_ADDR=host.docker.internal:6674`) and runs
   `install-runner-units.sh`, which installs and enables one
   `lastrada-runner@N.service` per worker under systemd. Every worker's
   `run-runner.sh` always passes `--add-host=host.docker.internal:host-gateway`
   — what makes `host.docker.internal` resolve to the Docker host's own IP
   on plain Linux Docker Engine (Docker Desktop provides this
   automatically). This only matters because `ci.yml`'s own job-level
   `FASTCACHE_ADDR` (see **ci.yml integration** below) always sends the
   literal `host.docker.internal:6674` itself, so that is what actually has
   to resolve on every runner host, cloud or otherwise.
6. Each unit's `run-runner.sh` mints its own fresh registration token from
   `orgs/LASTRADA-Software/actions/runners/registration-token` on every
   start — nothing is passed into the script and no token is stored in a
   container. `gh` needs to stay installed and authenticated with
   `admin:org` on the cloud box itself for as long as the fleet runs there,
   not just for this one invocation.

## Stopping / deregistering

```bash
systemctl --user stop 'lastrada-runner@1'      # or plain `systemctl`, system install
```

Deregistration happens on the **host**, not inside the container.
`run-runner.sh` starts every container with `-e RUNNER_SELF_DEREGISTER=0`,
which tells `entrypoint.sh` to skip installing its own `EXIT` trap; the
unit's `ExecStop=-<libexec>/deregister-runner.sh %i` looks this runner up
by name against the organisation's runners API and `DELETE`s it instead,
using the host's own `gh` login. That split exists because the container's
only credential by stop time is the *registration* token it started with,
and GitHub expires those after about an hour — a container that has been up
longer than that would have its own `config.sh remove` fail, stranding the
registration and its live session (which GitHub then holds in a "Conflict.
Retrying until reconnected" state for a couple of minutes on the next
start). The host's `gh` credential has no such expiry, so it deregisters
instead. `run-runner.sh` also clears any stale registration for this name
before every start, as a second layer against the same failure.

Separately — and unrelated to deregistration — systemd sends `SIGTERM` to
the `docker run -i` process that is this unit's main process; sig-proxy (on
by default without `-t`) relays it straight into the container, and the
runner process there exits, ending whatever job is currently running rather
than letting it finish. There is no drain. `TimeoutStopSec=90` is only the
ceiling before a `SIGKILL` if that hasn't happened by then; in practice a
stop or restart completes in a few seconds — verified from a real restart's
journal, `Stopping ...` to `Stopped` in about 3 seconds — so that budget is
essentially never used. The unit's `ExecStopPost` also force-removes the
container by name as a backstop, since `--rm` alone only covers a clean
exit. If the container is killed harder than a normal stop (host crash,
`docker kill`, out-of-memory), or `deregister-runner.sh` itself fails to
reach the API, the runner can be left registered but shows as offline;
remove it manually via the organisation's **Settings → Actions → Runners**,
or `gh api -X DELETE orgs/LASTRADA-Software/actions/runners/<id>`.

To stop the fleet for good rather than just for now, disable the unit(s)
too (`systemctl --user disable --now 'lastrada-runner@*'`), or lower
`RUNNER_COUNT` in the config and re-run `install-runner-units.sh`, which
disables whatever instances that leaves in excess.

### Host restarts

systemd owns the containers, and each start mints its own registration
token — so a reboot re-registers cleanly no matter how long the machine was
off.

This was not always true, and the failure was expensive. Until 2026-09-14
the containers ran `--restart unless-stopped` with `RUNNER_TOKEN` baked into
the container environment at `docker run` time. A GitHub registration token
expires after about an hour, so every reboot after that hour re-ran
`config.sh` against a dead token: 404, exit 1, Docker restarts it, forever.
Five containers sat in that loop from 2026-09-08 to 2026-09-14 — 5,972
failed registrations on one of them — and nothing reported it.

Two changes make it unreachable rather than unlikely. There is no stored
token to go stale, because `run-runner.sh` mints one per start. And a
failure is now bounded and visible: `StartLimitBurst=12` puts the unit into
`failed` after twelve attempts within a 900-second (15-minute)
`StartLimitIntervalSec` window, so `systemctl --user status
'lastrada-runner@*'` shows red instead of a loop nobody looks at.

That 12/900 budget is wider than a first cut of 5 attempts over 10 minutes,
and deliberately so. `lastrada-runner@.service`'s `After=docker.service
network-online.target` / `Wants=network-online.target` only resolve for a
**system** install (`systemctl`, run as root) — both names are system
units, and under `systemctl --user`, the mode this README's **Quick start**
uses, neither resolves at all, so a user-mode install has *no* boot
ordering against Docker or the network. At boot this unit can start racing
a Docker daemon that is still coming up. With `RestartSec=30`, 5 attempts
gave only 150 seconds of runway before the unit went permanently `failed` —
not enough margin against a slow-booting host's Docker daemon, observed
taking close to two minutes to come up. 12 attempts at the same
`RestartSec=30` gives at least 360 seconds (6 minutes) of runway, and the
900-second window is widened in step so it can't itself lapse mid-burst.
It is still bounded, not a return to the unbounded-retry failure this
`StartLimit` exists to prevent — a genuinely dead credential still ends in
a visible `failed` state, just with a realistic amount of runway first.

Hosts without systemd (Docker Desktop, WSL2) still use the plain `docker
run` path below. That path is reboot-fragile by design: it has the same
one-hour token, and re-running the `docker run` command with a fresh token
is the recovery.

### Docker Desktop / WSL2 (no systemd)

With no supervisor to mint a fresh token per start, `entrypoint.sh` falls
back to reading one from `RUNNER_TOKEN` in the container's own environment
instead of stdin:

```bash
gh api -X POST orgs/LASTRADA-Software/actions/runners/registration-token --jq '.token'

docker run -d \
  --name lastrada-docker-1 \
  --restart unless-stopped \
  -e RUNNER_TOKEN="<token from above, ~1 hour TTL>" \
  lastrada-runner:latest
```

This is the path described just above: the token is only good for about an
hour, so a Docker Desktop restart or a host reboot more than an hour after
this `docker run` leaves `config.sh` registering against a dead token.
Recovery is re-running the command above with a fresh token — there is
nothing else to fix, since `entrypoint.sh` already removes stale
`.runner`/`.credentials` files unconditionally before calling `config.sh`;
that part of the container's local state was never the problem.

## Environment variables (`entrypoint.sh`)

| Variable        | Required | Default                                | Notes |
|-----------------|----------|-----------------------------------------|-------|
| `RUNNER_TOKEN`  | no       | —                                        | Fallback registration token for the plain `docker run` path only, ~1 hour TTL. Under systemd the token arrives on **stdin** from `run-runner.sh` instead, so it never sits in the container environment where a job could read it out of `/proc/1/environ`. |
| `RUNNER_SCOPE_URL` | no    | `https://github.com/LASTRADA-Software`   | What `config.sh --url` registers against. |
| `RUNNER_GROUP`  | no       | `linux-docker`                           | Runner group to join. Must allow public repositories. |
| `RUNNER_NAME`   | no       | `lastrada-docker-<container hostname>`  | Under systemd, `run-runner.sh` always sets this explicitly to `<RUNNER_NAME_PREFIX>-<index>` from the config instead of relying on the default; only the plain `docker run` path needs to set it by hand for multiple runners to stay distinguishable in the organisation's runner list. |
| `RUNNER_LABELS` | no       | `self-hosted,Linux,X64,lastrada-docker` | Only change this if you also update the `runs-on:` label list in the workflow(s) that should target it. |
| `RUNNER_SELF_DEREGISTER` | no | `1` (enabled) | Set to `0` to skip `entrypoint.sh`'s own `EXIT`-trap deregistration. `run-runner.sh` always passes `0` under systemd, because the host-side `ExecStop=deregister-runner.sh` (see **Stopping / deregistering**) does it instead, with a credential that doesn't expire the way the container's registration token does. Only the plain `docker run` path (no systemd) needs the trap, and leaves this unset so it defaults to enabled. |

## Trust boundary

A self-hosted runner executes arbitrary job code on whatever host runs the
container — a cloud VM here, or your own machine. `linux-compilers`,
`linux-sanitizers`, `linux-all-features`, `ladder-tests`, and
`ladder-sanitizers` inherit `ci.yml`'s top-level `on: pull_request:`
trigger with no fork restriction of their own, but
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

Each container is one runner process, and capacity is controlled by
`RUNNER_COUNT` in the systemd config (`config.example`, installed to
`~/.config/lastrada-runner/config` for a user install or
`/etc/lastrada-runner/config` for a system one) rather than a manual loop
of `docker run` commands. `install-runner-units.sh` enables one
`lastrada-runner@N.service` per index from `1` to `RUNNER_COUNT`, and each
unit's `run-runner.sh <index>` mints its own token and starts its own
container — no coordination between them is needed, they all just poll
the same organisation's job queue. To add or remove capacity, edit
`RUNNER_COUNT` in the config and re-run `install-runner-units.sh`: it
enables the new instances and disables whatever is left over above the
new count.

Per-worker sizing is `RUNNER_CPUS`/`RUNNER_MEMORY` in that same config,
which `run-runner.sh` passes straight through as `--cpus=`/`--memory=` on
`docker run` — a CFS quota, **not** a pinned `--cpuset-cpus`. That
distinction used to be load-bearing here and still is: `--cpus=N` throttles
a container without changing what it *sees*, so `nproc` inside a 2-CPU
container on a 12-processor host still answers **12**, and anything that
fans out on `nproc` directly — ninja run without a cap, `ctest -j`,
`clang-tidy-diff`'s own `-j "$(nproc)"` — oversubscribes the box regardless
of the quota. Measured here once, before this fleet's config-driven sizing
existed: **32 OOM kills** across five containers on one run, `g++: fatal
error: Killed signal terminated program cc1plus` in every one.
`CMAKE_BUILD_PARALLEL_LEVEL` (also set per worker in `config`, and matched
to `RUNNER_CPUS` by convention — `config.example`'s own comment says so,
nothing enforces it automatically) is what actually bounds the one build
step every self-hosted job runs, `cmake --build --preset`, since that tool
reads the variable instead of `nproc`. The Linux jobs that would otherwise
exercise an uncapped `ctest -j`/`clang-tidy-diff` (`valgrind`, `clang-tidy`)
are kept off this fleet entirely — see **ci.yml integration** above — so
raising `RUNNER_COUNT` or lowering `RUNNER_CPUS` without also updating
`CMAKE_BUILD_PARALLEL_LEVEL` is a real, if currently unexercised, way to
reintroduce this.

`run-runner.sh` always passes `--add-host=host.docker.internal:host-gateway`
regardless of host type — Docker Desktop provides that name automatically,
a bare Linux Engine does not, and `ci.yml` sends the literal
`host.docker.internal:6674` to every self-hosted job. Without it the
address does not resolve and every job silently compiles uncached.

## ci.yml integration

None of `linux-compilers`, `linux-sanitizers`, `linux-all-features`,
`ladder-tests`, or `ladder-sanitizers` hardcode `runs-on:`. A
`probe-self-hosted` job that runs first checks the runners API for an
online, non-busy runner labeled `lastrada-docker` and outputs the label
set each of them should use — self-hosted if one is free, otherwise the
plain `ubuntu-24.04` GitHub-hosted label. Nothing needs to be started or
stopped by hand for this fallback to work; it's just naturally in effect
whenever no `lastrada-docker` runner happens to be online or all of them
are busy on another job. `linux-coverage`, `kanban-tsan`, `linux-qt`,
`valgrind`, and `clang-tidy` are intentionally left on `ubuntu-24.04` for
now, with no `probe-self-hosted` dependency at all.

The one piece that doesn't come for free: `GITHUB_TOKEN` cannot call the
runners API, and — because the fleet is organisation-level, not
repo-level — the *repo* runners endpoint wouldn't answer for it even if
it could: `GET /repos/.../actions/runners` returns zero for these
runners, silently, with no error to notice. The probe therefore calls
`GET https://api.github.com/orgs/LASTRADA-Software/actions/runners`
instead, authenticated with a repo secret named `RUNNER_STATUS_TOKEN`,
which must carry **organisation-level self-hosted-runner read access** —
either `admin:org` on a classic PAT, or a fine-grained PAT with
"Self-hosted runners: Read-only" granted **on the `LASTRADA-Software`
organisation**, not scoped to this repo alone (a repo-scoped grant, even
with "Administration: Read-only", cannot see an org-level runner and the
call comes back empty exactly like a missing secret). It cannot register,
delete, or otherwise manage runners, and has no code access. Set it up
once at **Settings → Secrets and variables → Actions → New repository
secret**. Until that secret exists, the probe always falls back to
`ubuntu-24.04` — nothing breaks, the jobs above just never pick up the
self-hosted path.

Forked-repo pull requests never receive repo secrets at all (GitHub
withholds them for security), so `RUNNER_STATUS_TOKEN` reads as empty
there and the probe falls back the same way — no separate handling
needed for that case.

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
docker exec -u root lastrada-docker-1 git config --system http.version HTTP/1.1
```

## Compiler cache: fastcache-cc

`linux-compilers`, `linux-sanitizers`, `linux-all-features`, `ladder-tests`,
and `ladder-sanitizers` each set `FASTCACHE_ADDR=host.docker.internal:6674`
as job-level env — but **only** when `probe-self-hosted` chose the
self-hosted path; it is left empty on the GitHub-hosted fallback, where
`host.docker.internal` does not resolve (it isn't a Docker container) and
morph's `cmake/CompileCache.cmake` has no daemon to reach anyway.

`FASTCACHE_AUTO_INSTALL` is a plain CMake `option()`, not something
`CompileCache.cmake` reads from the environment the way it does
`FASTCACHE_ADDR` — a job-level env var of that name would be silently
ignored, reaching only `option()`'s CMake-cache default rather than
CompileCache.cmake's auto-install logic. So `ci.yml` never sets it as env
at all. Instead each of those five jobs precomputes a same-named job-level
env var, `MORPH_FASTCACHE_AUTO_INSTALL_FLAG`, whose value on the
self-hosted path is the literal string `-DFASTCACHE_AUTO_INSTALL=ON` (and
empty on the GitHub-hosted fallback), and every Configure step splices
`$MORPH_FASTCACHE_AUTO_INSTALL_FLAG` directly onto the `cmake --preset`
command line, so the flag reaches CMake the only way it can take effect.

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
