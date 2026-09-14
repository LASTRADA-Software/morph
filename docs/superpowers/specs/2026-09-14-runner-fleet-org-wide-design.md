# Org-wide runner fleet — design

The `morph-runner-*` containers stop being long-lived holders of an expired
credential. systemd owns their lifecycle, each start mints its own
registration token, and the fleet registers against the
`LASTRADA-Software` organisation rather than the `morph` repository, so
`fastcached` and `Lightweight` can use it too.

This is sub-project **A** of three. **B** (fastcached CI) and **C**
(Lightweight CI) depend on it and get their own specs; the job inventory
they start from is recorded in [Follow-up work](#follow-up-work).

## Contents

- [The failure](#the-failure)
- [Lifecycle](#lifecycle)
- [Minting a token per start](#minting-a-token-per-start)
- [Configuration](#configuration)
- [Organisation registration](#organisation-registration)
- [Labels](#labels)
- [Image contents](#image-contents)
- [Compile-cache hygiene between jobs](#compile-cache-hygiene-between-jobs)
- [The ci.yml probe](#the-ciyml-probe)
- [Installation](#installation)
- [Manual steps](#manual-steps)
- [Testing](#testing)
- [Follow-up work](#follow-up-work)
- [Out of scope](#out-of-scope)

## The failure

All five containers restart-looped from 2026-09-08 11:14:30 to the time of
writing — 5,972 failed registrations on `morph-runner-1` alone, and no
self-hosted capacity for six days. Nothing reported it.

`RUNNER_TOKEN` is a GitHub *registration token*, which GitHub expires after
roughly one hour. `docker run -e RUNNER_TOKEN=…` freezes one into the
container's environment at creation time. The containers ran
`--restart unless-stopped`, so every host reboot re-ran `entrypoint.sh`
against the same, now long-dead token: `config.sh` 404s, the process exits
1, Docker restarts it, forever. The containers were created on 2026-09-06
and the host has rebooted six times since.

`README.md`'s "Docker Desktop / host restarts" section claims the opposite:

> `entrypoint.sh` removes those files unconditionally before calling
> `config.sh`, so the container always re-registers cleanly on restart
> rather than restart-looping

That fix addressed a different failure — stale `.runner`/`.credentials`
making `--replace` refuse — and cannot help here, because the token itself
is expired one step later. A container holding a one-hour credential under
`--restart unless-stopped` cannot survive a reboot an hour after creation.
The section is corrected as part of this work.

## Lifecycle

systemd owns the containers. Docker's restart policy is dropped entirely.

A template unit `lastrada-runner@.service` is enabled once per runner
(`lastrada-runner@1` … `lastrada-runner@5`):

```ini
[Unit]
Description=LASTRADA self-hosted GitHub Actions runner %i
After=docker.service network-online.target
Wants=network-online.target

[Service]
ExecStart=<libexec>/run-runner.sh %i
ExecStopPost=-/usr/bin/docker rm -f <prefix>-%i
Restart=always
RestartSec=30
StartLimitIntervalSec=600
StartLimitBurst=5
TimeoutStopSec=90

[Install]
WantedBy=default.target
```

`<libexec>`, `<prefix>` and `WantedBy` are substituted at install time —
`default.target` for a user install, `multi-user.target` for a system one.

`entrypoint.sh` changes with it: `REPO_URL` becomes a `RUNNER_SCOPE_URL`
environment variable defaulting to the organisation, `--runnergroup` is
passed through from `RUNNER_GROUP`, and the token is read from stdin (see
below). Its `rm -f /usr/local/bin/sccache` moves to the job hook.

Three properties follow, and each one is load-bearing:

**Exactly one owner.** Containers run `--rm` with no `--restart` flag.
Today Docker restarts a container that systemd knows nothing about; after
this, nothing competes. It also means each start gets a clean container
filesystem, which retires the stale-`.runner` class of bug on this path
entirely.

**Failure is bounded and visible.** `StartLimitBurst=5` over ten minutes
puts the unit into `failed` instead of retrying forever. The six-day silent
outage becomes a red `systemctl --user status lastrada-runner@3`, with the
reason in `journalctl --user -u lastrada-runner@3` rather than 149,000 lines
of `docker logs`.

**A dead runner comes back.** A container killed mid-life — OOM during a
link, a job that takes the runner down — is restarted by `Restart=always`
with a fresh token, without waiting for a reboot.

The cost is that a restart discards whatever toolchain a previous job
installed in that container, so the next job re-downloads gcc/clang. `ci.yml`
installs its toolchain per run regardless, and restarts are rare, so this is
accepted.

## Minting a token per start

`ExecStartPre` cannot do this. systemd does not propagate environment from
`ExecStartPre` to `ExecStart`, and `ExecStart` is not a shell, so there is
no command substitution to read a token file back. `ExecStart` therefore
points at a wrapper that mints the token *and* `exec`s Docker:

```sh
#!/usr/bin/env bash
# run-runner.sh <index>
set -euo pipefail
index="$1"
. "${LASTRADA_RUNNER_CONFIG:?}"

token="$(gh api -X POST \
    "orgs/${GITHUB_ORG}/actions/runners/registration-token" --jq '.token')"

name="${RUNNER_NAME_PREFIX}-${index}"
docker rm -f "$name" >/dev/null 2>&1 || true

exec docker run --rm -i \
    --name "$name" \
    --cpus="${RUNNER_CPUS}" \
    --memory="${RUNNER_MEMORY}" \
    --add-host=host.docker.internal:host-gateway \
    -e RUNNER_NAME="$name" \
    -e RUNNER_SCOPE_URL="https://github.com/${GITHUB_ORG}" \
    -e RUNNER_GROUP="${RUNNER_GROUP}" \
    -e RUNNER_LABELS="${RUNNER_LABELS}" \
    -e FASTCACHE_ADDR="${FASTCACHE_ADDR}" \
    -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL}" \
    "${RUNNER_IMAGE}" <<<"$token"
```

`exec` matters: systemd then tracks the Docker client directly, so `SIGTERM`
on stop reaches the container instead of an intermediate shell.

**The token arrives on stdin, not in the environment.** `-e RUNNER_TOKEN=…`
would put a live registration token in the container's environment, readable
by every job that runs there via `env` or `/proc/1/environ`. On a public
repository that is contributor-controlled code, and a valid registration
token can enrol an attacker's own machine into the runner group. Reading it
from stdin keeps it out of the environment and out of `/proc`.

`entrypoint.sh` reads the first line of stdin, falling back to
`$RUNNER_TOKEN` when stdin is a TTY or empty — that fallback keeps the plain
`docker run` path in the README working for Docker Desktop hosts, which get
no systemd.

The credential that mints these tokens is the host's existing `gh` CLI
authentication. It never enters a container. Its scopes are broad
(`admin:org`, `delete_repo`, `admin:enterprise`), which is why the unit runs
as the `yaraslau` user rather than root and why the README documents the
scope explicitly. `admin:org` is what the organisation endpoint requires.

## Configuration

An env-style file, `~/.config/lastrada-runner/config` for a user install or
`/etc/lastrada-runner/config` for a system install:

```sh
GITHUB_ORG=LASTRADA-Software
RUNNER_COUNT=5
RUNNER_NAME_PREFIX=lastrada-docker
RUNNER_GROUP=linux-docker
RUNNER_LABELS=self-hosted,Linux,X64,lastrada-docker
RUNNER_CPUS=2
RUNNER_MEMORY=6g
RUNNER_IMAGE=lastrada-runner:latest
FASTCACHE_ADDR=host.docker.internal:6674
CMAKE_BUILD_PARALLEL_LEVEL=2
```

The runners are renamed from `morph-docker-N` to `lastrada-docker-N` for the
same reason the label changes — the old name describes one of the three
repositories they now serve. The rename is free here because the existing
repo-level registrations are being deleted anyway.

`RUNNER_COUNT` is read by the install script to decide which template
instances to enable; `run-runner.sh` reads the rest. `RUNNER_CPUS=2` caps
each container at two CPUs — five runners then use 10 of this host's 12,
leaving two for the OS and the natively-running `fastcached` daemon, and it
matches the `CMAKE_BUILD_PARALLEL_LEVEL=2` already in use.

`fastcached` runs natively on this host (bound to `127.0.0.1` and the Docker
bridge `172.17.0.1`), not in a container, which is why
`host.docker.internal:6674` resolves. `bootstrap-cloud-node.sh` still starts
a containerised daemon on cloud VMs; both reach it the same way.

## Organisation registration

Runners register against the organisation:
`POST /orgs/LASTRADA-Software/actions/runners/registration-token`, and
`config.sh --url https://github.com/LASTRADA-Software --runnergroup linux-docker`.

**A new runner group `linux-docker` is required**, because the organisation's
`Default` group has `allows_public_repositories: false` and all three
consuming repositories are public — org-level runners in `Default` would be
invisible to exactly the repositories that need them. The group is created
as:

- `visibility: selected` → `morph`, `fastcached`, `Lightweight`
- `allows_public_repositories: true`

`Default` is left untouched. Scoping to selected repositories keeps the
organisation's other five repositories away from this hardware; verified
during design that this organisation's Free plan permits creating such a
group (a throwaway group was created with these settings and deleted).

Adding a fourth repository later is one API call against the group — no
re-registration, no restart.

Registration is idempotent across restarts: names are stable, so
`config.sh --replace` overwrites the same-named offline entry each time.
No cleanup job is needed and offline entries do not accumulate.

The five existing repo-level registrations on `LASTRADA-Software/morph` are
deleted as part of the rollout.

## Labels

Runners register as `self-hosted,Linux,X64,lastrada-docker`.

`morph-docker` is removed. It named one repository for a fleet that now
serves three, and keeping it would leave every future consumer targeting a
label that describes none of them.

This is a flag day: the label and `ci.yml`'s `runs-on` must change together,
which they do — both are in this repository and in this change. A mismatch
does not break the build, because every consuming job falls back to
GitHub-hosted when the probe finds nothing; it would silently stop using
self-hosted capacity, which the probe's log line makes visible.

## Image contents

Auditing the image against what `fastcached` and `Lightweight` builds need
found five packages present on GitHub-hosted `ubuntu-24.04` and absent here:

| Package | Why |
|---|---|
| `pkg-config` | Lightweight resolves unixodbc, libzip and yaml-cpp through `find_package(PkgConfig)`. The most likely hard failure of the five. |
| `zstd` | `actions/cache` (used by both repos) otherwise falls back to gzip. |
| `unzip` | Assumed present by several actions and archive steps. |
| `file`, `rsync` | Shelled out to by some actions. |
| `locales` + `en_US.UTF-8` | The image has only `C`/`C.utf8`; tests asserting formatted output can diverge from GitHub-hosted. |

`ninja-build`, `doxygen` and `ccache` are deliberately **not** added — every
job installs those itself, and the Dockerfile's existing rule is not to
prebake what jobs bootstrap. Both repos refresh the apt database before
installing (`sudo apt -q update`, and fastcached's `scripts/ci-apt-update.sh`),
so the image's empty apt cache is not a blocker.

## Compile-cache hygiene between jobs

`entrypoint.sh` currently runs `rm -f /usr/local/bin/sccache` at container
start, because a leftover compile-cache binary satisfies
`CompileCache.cmake`'s `find_program` and disables morph's
`FASTCACHE_AUTO_INSTALL` path — that guard fires only when *none* of
`fastcache-cc`, `sccache` or `ccache` is on `PATH`.

Sharing the fleet makes this reachable in a new way. Both new repositories
use `hendrikmuhs/ccache-action`, which apt-installs `ccache` to
`/usr/bin/ccache`. A Lightweight job followed by a morph job on the same
container silently costs morph its compile cache. Start-time wiping cannot
fix it: the contamination happens *between* jobs.

The runner's own hook mechanism is the right lever. A
`job-started-hook.sh`, baked into the image and wired through
`ACTIONS_RUNNER_HOOK_JOB_STARTED`, removes `sccache` and `ccache` from
`PATH` directories before every job. It replaces the entrypoint's one-shot
`rm -f` as a special case.

It deliberately does **not** remove `fastcache-cc`. When that binary is
present, auto-install is unnecessary and `CompileCache.cmake` simply uses
it — which is the desired outcome. Removing it would force a re-download on
every job.

## The ci.yml probe

`probe-self-hosted` queries
`https://api.github.com/repos/${{ github.repository }}/actions/runners`.
Once registration is organisation-level, that endpoint returns zero runners
and **every run silently falls back to GitHub-hosted** with no error. It
becomes:

```
https://api.github.com/orgs/LASTRADA-Software/actions/runners
```

The `jq` filter's label test changes from `morph-docker` to
`lastrada-docker`, and the emitted `runs_on` becomes
`["self-hosted", "Linux", "X64", "lastrada-docker"]`.

The existing fallback behaviour is preserved exactly: an empty
`RUNNER_STATUS_TOKEN` (forked-repo PRs get no secrets), a failed API call,
or no idle runner all select `ubuntu-24.04`.

`RUNNER_STATUS_TOKEN` must be re-issued — see [Manual steps](#manual-steps).

## Installation

One script, `install-runner-units.sh`, with two modes selected by whether it
runs as root:

| | User install (this host) | System install (cloud VM) |
|---|---|---|
| Unit | `~/.config/systemd/user/lastrada-runner@.service` | `/etc/systemd/system/lastrada-runner@.service` |
| Config | `~/.config/lastrada-runner/config` | `/etc/lastrada-runner/config` |
| Wrapper | `~/.local/libexec/lastrada-runner/` | `/usr/local/libexec/lastrada-runner/` |
| Control | `systemctl --user` | `systemctl` |
| Boot | requires lingering (already enabled here) | ordinary system unit |

The same template body is used for both, with paths substituted at install
time. The script enables `lastrada-runner@1`…`@N` from `RUNNER_COUNT`, and
disables any higher-numbered instances left over from a previous, larger
fleet.

`bootstrap-cloud-node.sh` changes from starting containers directly to
writing the config file and calling this script. Its per-worker
token-minting loop — which exists only because a registration token is
single-use — is deleted; `run-runner.sh` mints per start, which covers it.

## Manual steps

Two things cannot be automated here and belong to the repository owner:

1. **Re-issue `RUNNER_STATUS_TOKEN`.** The current secret holds a PAT with
   `Administration: Read-only` on the `morph` repository. The organisation
   runners endpoint needs an organisation-scoped token (`admin:org`, or a
   fine-grained PAT with organisation `Self-hosted runners: Read-only`).
   Until it is replaced, the probe fails its API call and every job falls
   back to GitHub-hosted — degraded, not broken.
2. **Add `RUNNER_STATUS_TOKEN` to `fastcached` and `Lightweight`** when
   sub-projects B and C land. Those repositories have no such secret today.

## Testing

**Unit-level, no Docker or GitHub required.** `run-runner.sh` gains a
dry-run mode (`LASTRADA_RUNNER_DRY_RUN=1`) that prints the `docker` argv
instead of `exec`ing it, and the tests put a stub `gh` on `PATH` that
returns a known token. Assertions: the org endpoint is called (not the repo
one), `--cpus=2` and `--memory=6g` are present, `--rm` is present, no
`--restart` flag is emitted, the container name is
`${RUNNER_NAME_PREFIX}-${index}`, and the token appears on stdin and in no
`-e` argument.

**Unit file.** `systemd-analyze verify` on the generated unit in both
install modes.

**stdin token delivery.** An explicit test that `run.sh` tolerates EOF on
stdin after the token is read — the runner is normally started with a
terminal, and this is the one assumption in the design that could fail at
runtime rather than at review.

**Image.** A container-level check that each of the five added packages is
present and `en_US.UTF-8` is generated.

**Hook.** Plant `sccache` and `ccache` on `PATH`, run the hook, assert both
are gone and a planted `fastcache-cc` survives.

**Live, on this host.** Five runners online in the organisation with the
`lastrada-docker` label; `systemctl --user restart lastrada-runner@3`
re-registers with a *different* token than its previous start; a full
`systemctl --user stop` of all five leaves no containers behind; and a morph
CI run lands on a self-hosted runner and reports a fastcache hit against the
host daemon.

**End-to-end for B and C readiness.** Build `fastcached` and `Lightweight`
inside the container. This is the only way to establish that the image
package list is complete rather than guessed, and it is a gate on this
sub-project even though the CI wiring lands later.

## Follow-up work

Job inventory for sub-projects B and C, from an audit of all 38 jobs in the
two repositories.

**Move to self-hosted** — compile-heavy, apt-only dependencies:

| Repo | Jobs |
|---|---|
| fastcached | `linux` (×2), `clang-tidy`, `clang-asan-ubsan`, `clang-tsan`, `coverage` |
| Lightweight | `ubuntu_build_cc_matrix` (×2), `check_clang_tidy`, `sanitizers` (×2), `check_docs` |

**Blocked — require a Docker daemon the runner container does not have:**
fastcached `docker`; Lightweight `cpp26-reflection`, `dbms_test_matrix`
(legs where `matrix.docker_db != ''`), `coverage`. Mounting the host's
Docker socket would unblock these and is rejected: on a public repository it
grants any fork-PR author root on the host.

**Not worth a slot** — seconds-long jobs with no compile-cache benefit:
fastcached `check-clang-format`, `check-release-gate`, `changes`, `release`;
Lightweight `check_PR_TODOs`, `check_clang_format`, `ddl2cpp`.

**Excluded deliberately:**

- fastcached `compile-cache-e2e-linux`, `sccache-smoke-*`,
  `fastcache-cc-smoke` — these test compile-cache behaviour and stand up
  their own `fastcached` on fixed ports. An ambient `FASTCACHE_ADDR`
  pointing at the host daemon would leak into the code under test. They
  could move only if the step clears `FASTCACHE_ADDR` explicitly, which is
  a decision for whoever owns those tests.
- Lightweight `ubuntu_dbtool_gui` — pulls Qt 6.8 via `install-qt-action`,
  needing libGL/libxkbcommon/fontconfig that a headless container lacks.
- fastcached `package-linux` and Lightweight `tracy_capture` — excluded at
  the maintainer's request. They remain on GitHub-hosted; the jobs
  themselves are unchanged.

**`CompileCache.cmake` divergence.** The module is designed to be copied
between repositories verbatim, and the three copies have drifted:

| Repo | Lines | `FASTCACHE_ADDR` | `FASTCACHE_AUTO_INSTALL` |
|---|---|---|---|
| morph | 1197 | yes | yes |
| Lightweight | 302 | yes | **no** |
| fastcached | absent | — | — |

`FASTCACHE_AUTO_INSTALL` is what fetches `fastcache-cc` on a machine that has
none — which every one of these containers is. Without it, pointing a build
at the host daemon caches nothing. So B adds the module to `fastcached` and
C syncs Lightweight's copy up; neither is a CI-only change. Lightweight's
older copy also uses `FASTCACHE_SRCROOT`/`FASTCACHE_BUILDTREE` where morph
uses `FASTCACHE_SOURCE_DIR`/`FASTCACHE_BINARY_DIR`, so C must check for
references outside the module before syncing.

**Probe duplication.** B and C would each add a third and fourth copy of
`probe-self-hosted`. Worth consolidating into a `workflow_call` reusable
workflow at that point rather than copy-pasting it twice more.

## Out of scope

- Editing `fastcached`'s or `Lightweight`'s workflows — sub-projects B and C.
- Deleting or restructuring any existing CI job in any repository.
- Mounting a Docker socket into runner containers.
- Growing the image for Qt (`ubuntu_dbtool_gui`).
- Replacing the host `gh` credential with a dedicated or fine-grained PAT.
  Considered during design and declined in favour of reusing the existing
  authentication; the README records the scope breadth this accepts.
- Any change to `fastcached`'s native daemon deployment on this host.
