# Self-hosted Linux runner (Docker)

Runs repo-level GitHub Actions runners for `LASTRADA-Software/morph` inside
Docker containers. Used by `ci.yml`'s `linux-compilers`, `linux-sanitizers`,
and `linux-all-features` jobs whenever a runner is online and idle (see
**ci.yml integration** below).

Currently running as 4 containers on one host (the maintainer's Windows
machine, Docker Desktop, Linux containers), each capped at 6 CPUs
(`docker run --cpus=6`) on a 32-logical-processor box — 24 cores committed,
8 left as headroom for the host OS and Docker Desktop itself. A Hetzner
Cloud VM briefly ran a fifth registration but was torn down.

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
is needed, they all just poll the same repo's job queue. This is exactly
how the current 4 containers are set up:

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

## Compiler cache: fastcache-cc

`linux-compilers`, `linux-sanitizers`, and `linux-all-features` each set
`FASTCACHE_ADDR=host.docker.internal:6674` and `FASTCACHE_AUTO_INSTALL=ON`
as job-level env — but **only** when `probe-self-hosted` chose the
self-hosted path; both are left empty/OFF on the GitHub-hosted fallback,
where `host.docker.internal` does not resolve (it isn't a Docker
container) and morph's `cmake/CompileCache.cmake` has no daemon to reach
anyway.

`host.docker.internal:6674` is a Windows/Docker-Desktop-specific address:
it resolves, from inside a Linux container, to whatever the Docker Desktop
host's `127.0.0.1` would mean — i.e. this same Windows machine's own
`fastcached` service (see `D:\caching` on that machine; **not** part of
this repository). That service must be **running** and its
`fastcached.yaml` must **bind `0.0.0.0`**, not the default `127.0.0.1`,
or a container cannot reach it at all (`127.0.0.1` inside a container
means the container itself). Moving this runner setup to a different host
means either running a `fastcached` daemon reachable from that host's
containers the same way, or leaving `FASTCACHE_ADDR` unset there — the
module falls back to `sccache` (already installed by every job) with zero
other changes needed; see `cmake/CompileCache.cmake`'s own header comment
for the full fastcache-cc → sccache → ccache → none preference order.

`FASTCACHE_AUTO_INSTALL=ON` is what lets this work without prebaking
`fastcache-cc` into the runner image: on first configure, CMake downloads
a prebuilt `fastcache-cc` binary from the `fastcached` project's own
GitHub Releases (cached per-user, per-version, so this costs one download
per container, not per build) — see
`cmake/CompileCache.cmake`'s "Optional auto-install" section for exactly
how, including its own SHA256 verification and total inability to fail a
configure (a fetch that fails just falls through to sccache).
