# Org-wide Runner Fleet Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the self-hosted runner fleet survive host reboots and serve three organisation repositories instead of one, by handing its lifecycle to systemd and minting a registration token on every start.

**Architecture:** A systemd template unit runs one wrapper process per runner. The wrapper mints a fresh GitHub registration token, then `exec`s `docker run --rm` in the foreground so systemd tracks the container directly. Docker's own restart policy is dropped entirely, so exactly one supervisor exists. Registration moves from the `morph` repository to the `LASTRADA-Software` organisation, behind a runner group scoped to `morph`, `fastcached` and `Lightweight`.

**Tech Stack:** bash, systemd (template units, user and system scope), Docker CLI, GitHub Actions runner 2.336.0, `gh` CLI, GitHub Actions REST API.

**Spec:** `docs/superpowers/specs/2026-09-14-runner-fleet-org-wide-design.md`

## Global Constraints

- Every shell script starts `#!/usr/bin/env bash` and `set -euo pipefail`, matching every existing script in `scripts/`.
- A registration token must never be passed via `-e` to `docker run`, and must never appear in a container's environment. Jobs on these containers are contributor-authored code from public repositories.
- The credential `gh` is authenticated with never enters a container.
- Containers run `--rm` with **no** `--restart` flag. systemd is the only supervisor.
- Runner labels are exactly `self-hosted,Linux,X64,lastrada-docker`. The label `morph-docker` is removed everywhere.
- Runner names are `lastrada-docker-<index>`.
- Registration is organisation-level: `orgs/LASTRADA-Software/actions/runners/registration-token`, `--url https://github.com/LASTRADA-Software`, `--runnergroup linux-docker`.
- `RUNNER_CPUS=2`, `RUNNER_MEMORY=6g`, `RUNNER_COUNT=5` on this host.
- Self-tests live in `scripts/test_*.sh` and are gated by the `deprecation-lint` job in `.github/workflows/ci.yml`, following the existing `test_check_*.sh` pattern.
- No existing CI job in any repository is deleted or restructured.
- `fastcache-cc` is never removed by the job hook — only `sccache` and `ccache`.

---

## File Structure

| File | Responsibility |
|---|---|
| `.github/self-hosted-runner/run-runner.sh` | **Create.** Mint a token, build the `docker run` argv, exec it. One runner, by index. |
| `.github/self-hosted-runner/lastrada-runner@.service.in` | **Create.** systemd template, with `@PLACEHOLDER@` paths substituted at install. |
| `.github/self-hosted-runner/install-runner-units.sh` | **Create.** Install unit + wrapper + config; user mode or system mode. |
| `.github/self-hosted-runner/config.example` | **Create.** Documented configuration defaults. |
| `.github/self-hosted-runner/job-started-hook.sh` | **Create.** Clear `sccache`/`ccache` before each job. |
| `.github/self-hosted-runner/entrypoint.sh` | **Modify.** Token from stdin; org scope URL; runner group; drop the sccache wipe. |
| `.github/self-hosted-runner/Dockerfile` | **Modify.** Five packages, `en_US.UTF-8`, ship and wire the job hook. |
| `.github/self-hosted-runner/bootstrap-cloud-node.sh` | **Modify.** Write config and install units instead of running containers. |
| `.github/self-hosted-runner/README.md` | **Modify.** Correct the wrong restart section; document org-wide operation. |
| `.github/workflows/ci.yml` | **Modify.** Org probe endpoint, new label, self-test gates. |
| `scripts/test_run_runner.sh` | **Create.** Self-test for `run-runner.sh` launch contract. |
| `scripts/test_runner_entrypoint.sh` | **Create.** Self-test for entrypoint token resolution. |
| `scripts/test_runner_job_hook.sh` | **Create.** Self-test for the job hook. |
| `scripts/test_runner_units.sh` | **Create.** Self-test for unit generation + `systemd-analyze verify`. |

---

## Task 1: `run-runner.sh` and its launch contract

**Files:**
- Create: `.github/self-hosted-runner/run-runner.sh`
- Create: `scripts/test_run_runner.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `run-runner.sh <index>`. Reads config from `$LASTRADA_RUNNER_CONFIG`. Honours `LASTRADA_RUNNER_DRY_RUN=1` (print argv, then `STDIN:<token>`, exit 0), `LASTRADA_RUNNER_GH` (gh binary, default `gh`), `LASTRADA_RUNNER_DOCKER` (docker binary, default `docker`). Config keys it requires: `GITHUB_ORG`, `RUNNER_NAME_PREFIX`, `RUNNER_GROUP`, `RUNNER_LABELS`, `RUNNER_IMAGE`, `RUNNER_CPUS`, `RUNNER_MEMORY`; optional `FASTCACHE_ADDR`, `CMAKE_BUILD_PARALLEL_LEVEL`.

- [ ] **Step 1: Write the failing self-test**

Create `scripts/test_run_runner.sh`:

```bash
#!/usr/bin/env bash
# Usage: bash scripts/test_run_runner.sh
#
# Self-test for .github/self-hosted-runner/run-runner.sh, which is what
# systemd runs to start one self-hosted runner container.
#
# The launch contract is the whole point of the script and every clause of it
# is a past or potential outage:
#
#   * the token is minted per start, from the ORG endpoint -- a token baked
#     into a container at creation time expires in ~1 hour and left five
#     containers restart-looping for six days;
#   * the token travels on stdin, never in `-e` -- every job on the container
#     can read /proc/1/environ, and these are public repositories running
#     contributor-authored build code;
#   * `--rm` and no `--restart` -- Docker must not supervise a container
#     systemd is already supervising.
#
# None of that is observable from the outside once the script execs docker, so
# it is asserted through LASTRADA_RUNNER_DRY_RUN with stub gh/docker binaries.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly launcher="${repo_root}/.github/self-hosted-runner/run-runner.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# ── stubs ────────────────────────────────────────────────────────────────────
cat >"${scratch}/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${STUB_GH_LOG}"
printf 'tok-ABC123\n'
STUB
chmod +x "${scratch}/gh"

cat >"${scratch}/docker" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
chmod +x "${scratch}/docker"

cat >"${scratch}/config" <<'CONF'
GITHUB_ORG=LASTRADA-Software
RUNNER_NAME_PREFIX=lastrada-docker
RUNNER_GROUP=linux-docker
RUNNER_LABELS=self-hosted,Linux,X64,lastrada-docker
RUNNER_CPUS=2
RUNNER_MEMORY=6g
RUNNER_IMAGE=lastrada-runner:latest
FASTCACHE_ADDR=host.docker.internal:6674
CMAKE_BUILD_PARALLEL_LEVEL=2
CONF

export STUB_GH_LOG="${scratch}/gh.log"
: >"${STUB_GH_LOG}"

out="$(LASTRADA_RUNNER_DRY_RUN=1 \
      LASTRADA_RUNNER_CONFIG="${scratch}/config" \
      LASTRADA_RUNNER_GH="${scratch}/gh" \
      LASTRADA_RUNNER_DOCKER="${scratch}/docker" \
      bash "$launcher" 3 2>&1)" || {
    fail "run-runner.sh exited non-zero in dry-run mode:"
    printf '%s\n' "$out" >&2
    exit 1
}

has() { printf '%s\n' "$out" | grep -qxF "$1"; }

# ── the token is minted per start, against the org endpoint ──────────────────
if grep -q 'orgs/LASTRADA-Software/actions/runners/registration-token' "${STUB_GH_LOG}"; then
    note "mints from the organisation registration-token endpoint"
else
    fail "did not call the org registration-token endpoint; gh saw: $(cat "${STUB_GH_LOG}")"
fi

if grep -q 'repos/' "${STUB_GH_LOG}"; then
    fail "still calling a repo-level endpoint: $(cat "${STUB_GH_LOG}")"
else
    note "no repo-level endpoint call"
fi

# ── the token goes in on stdin and nowhere else ──────────────────────────────
if has 'STDIN:tok-ABC123'; then
    note "token delivered on stdin"
else
    fail "token was not delivered on stdin"
fi

# Each argv element prints on its own line, so the -e and its value are
# separate lines -- match the value line, not "-e RUNNER_TOKEN", which
# would never match and would make this assertion vacuous.
if printf '%s\n' "$out" | grep -q '^RUNNER_TOKEN='; then
    fail "token passed via -e RUNNER_TOKEN; jobs on the container could read it"
else
    note "no RUNNER_TOKEN environment argument"
fi

if printf '%s\n' "$out" | grep -v '^STDIN:' | grep -q 'tok-ABC123'; then
    fail "the token leaked into the docker argv"
else
    note "token absent from the docker argv"
fi

# ── systemd owns the lifecycle, not Docker ───────────────────────────────────
if has '--rm'; then
    note "--rm present"
else
    fail "--rm missing; containers would accumulate"
fi

if printf '%s\n' "$out" | grep -q -- '--restart'; then
    fail "--restart present; Docker would fight systemd for the lifecycle"
else
    note "no --restart flag"
fi

# ── resource caps and identity ───────────────────────────────────────────────
for expected in '--cpus=2' '--memory=6g' 'lastrada-docker-3'; do
    if has "$expected"; then
        note "${expected} present"
    else
        fail "${expected} missing from the docker argv"
    fi
done

if printf '%s\n' "$out" | grep -q 'morph-docker'; then
    fail "the retired morph-docker name/label is still emitted"
else
    note "no morph-docker name or label"
fi

# ── a missing config must fail loudly, not launch an unconfigured runner ─────
if LASTRADA_RUNNER_DRY_RUN=1 LASTRADA_RUNNER_CONFIG="${scratch}/absent" \
   bash "$launcher" 1 >/dev/null 2>&1; then
    fail "a missing config file was accepted"
else
    note "a missing config file is rejected"
fi

# ── a missing index must fail loudly ─────────────────────────────────────────
if LASTRADA_RUNNER_DRY_RUN=1 LASTRADA_RUNNER_CONFIG="${scratch}/config" \
   LASTRADA_RUNNER_GH="${scratch}/gh" LASTRADA_RUNNER_DOCKER="${scratch}/docker" \
   bash "$launcher" >/dev/null 2>&1; then
    fail "a missing runner index was accepted"
else
    note "a missing runner index is rejected"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall run-runner.sh launch-contract checks passed\n'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/test_run_runner.sh`
Expected: FAIL — `bash: .../run-runner.sh: No such file or directory`, non-zero exit.

- [ ] **Step 3: Write `run-runner.sh`**

Create `.github/self-hosted-runner/run-runner.sh`:

```bash
#!/usr/bin/env bash
# Starts one self-hosted runner container in the foreground, with a
# registration token minted fresh for this start.
#
# Usage: run-runner.sh <index>
#
# systemd's lastrada-runner@<index>.service is the only intended caller.
#
# The token is minted here rather than baked into the container by whoever
# created it. A GitHub registration token expires after ~1 hour, so a token
# captured at `docker run` time is dead on every restart after that hour --
# which is exactly what left five containers restart-looping for six days
# (5,972 failed registrations) until this script existed. Minting per start
# means there is no stored credential that can go stale.
#
# LASTRADA_RUNNER_DRY_RUN=1 prints the docker argv and the stdin payload instead
# of executing, so scripts/test_run_runner.sh can assert the launch contract
# without Docker or GitHub.
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: run-runner.sh <index>" >&2
    exit 1
fi
readonly index="$1"

config="${LASTRADA_RUNNER_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/lastrada-runner/config}"
if [ ! -r "$config" ]; then
    echo "run-runner.sh: no readable config at ${config}" >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$config"

: "${GITHUB_ORG:?config must set GITHUB_ORG}"
: "${RUNNER_NAME_PREFIX:?config must set RUNNER_NAME_PREFIX}"
: "${RUNNER_GROUP:?config must set RUNNER_GROUP}"
: "${RUNNER_LABELS:?config must set RUNNER_LABELS}"
: "${RUNNER_IMAGE:?config must set RUNNER_IMAGE}"
: "${RUNNER_CPUS:?config must set RUNNER_CPUS}"
: "${RUNNER_MEMORY:?config must set RUNNER_MEMORY}"

readonly name="${RUNNER_NAME_PREFIX}-${index}"
readonly gh="${LASTRADA_RUNNER_GH:-gh}"
readonly docker="${LASTRADA_RUNNER_DOCKER:-docker}"

# Organisation-level, not repo-level: one fleet serves morph, fastcached and
# Lightweight. Needs admin:org on whatever credential gh holds -- and that
# credential stays on the host, never entering a container.
token="$("$gh" api -X POST \
    "orgs/${GITHUB_ORG}/actions/runners/registration-token" --jq '.token')"

if [ -z "$token" ]; then
    echo "run-runner.sh: minted an empty registration token" >&2
    exit 1
fi

# A container from a previous start can outlive its unit (SIGKILL, daemon
# crash) and still own the name; --rm only covers a clean exit.
"$docker" rm -f "$name" >/dev/null 2>&1 || true

argv=(
    "$docker" run --rm -i
    --name "$name"
    "--cpus=${RUNNER_CPUS}"
    "--memory=${RUNNER_MEMORY}"
    --add-host=host.docker.internal:host-gateway
    -e "RUNNER_NAME=${name}"
    -e "RUNNER_SCOPE_URL=https://github.com/${GITHUB_ORG}"
    -e "RUNNER_GROUP=${RUNNER_GROUP}"
    -e "RUNNER_LABELS=${RUNNER_LABELS}"
    -e "FASTCACHE_ADDR=${FASTCACHE_ADDR:-}"
    -e "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
    "${RUNNER_IMAGE}"
)

if [ -n "${LASTRADA_RUNNER_DRY_RUN:-}" ]; then
    printf '%s\n' "${argv[@]}"
    printf 'STDIN:%s\n' "$token"
    exit 0
fi

# The token goes in on stdin, never in the environment. Every job on this
# container can read `env` and /proc/1/environ, and a live registration token
# would let contributor-authored code enrol its own machine into the runner
# group. `exec` so systemd tracks the docker client directly and its SIGTERM
# reaches the container rather than an intermediate shell.
exec "${argv[@]}" <<<"$token"
```

- [ ] **Step 4: Make it executable and run the test**

```bash
chmod +x .github/self-hosted-runner/run-runner.sh
bash scripts/test_run_runner.sh
```
Expected: PASS — every `ok:` line, ending `all run-runner.sh launch-contract checks passed`.

- [ ] **Step 5: Commit**

```bash
git add .github/self-hosted-runner/run-runner.sh scripts/test_run_runner.sh
git commit -m "Mint a registration token per runner start

A GitHub registration token lives ~1 hour, so one captured at docker-run
time is dead on every restart after that -- the cause of a six-day
restart loop. run-runner.sh mints one per start instead, from the org
endpoint, and hands it over on stdin so no job on the container can read
it out of the environment."
```

---

## Task 2: Entrypoint reads the token from stdin

**Files:**
- Modify: `.github/self-hosted-runner/entrypoint.sh`
- Create: `scripts/test_runner_entrypoint.sh`

**Interfaces:**
- Consumes: `run-runner.sh` from Task 1 passes the token on stdin and sets `RUNNER_SCOPE_URL`, `RUNNER_GROUP`, `RUNNER_LABELS`, `RUNNER_NAME`.
- Produces: `entrypoint.sh` defines `read_runner_token()` (echoes the token: first line of stdin when stdin is not a TTY and is non-empty, else `$RUNNER_TOKEN`). Sourcing with `LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY=1` defines functions and returns without side effects.

- [ ] **Step 1: Write the failing self-test**

Create `scripts/test_runner_entrypoint.sh`:

```bash
#!/usr/bin/env bash
# Usage: bash scripts/test_runner_entrypoint.sh
#
# Self-test for read_runner_token() in
# .github/self-hosted-runner/entrypoint.sh.
#
# Two callers deliver the registration token two different ways and both must
# keep working: systemd's run-runner.sh pipes it on stdin (so it never enters
# the container environment, where any job could read it), while the plain
# `docker run -e RUNNER_TOKEN=...` path documented for Docker Desktop hosts
# has no stdin to pipe. Getting the precedence wrong fails closed in the worst
# way -- the runner registers with an empty token and the container
# restart-loops, which is the exact failure this whole change exists to end.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly entrypoint="${repo_root}/.github/self-hosted-runner/entrypoint.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

# Sourcing must not run the registration; the guard makes that safe.
resolve() {
    # shellcheck disable=SC1090
    LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY=1 . "$entrypoint"
    read_runner_token
}

got="$(printf 'tok-from-stdin\n' | (RUNNER_TOKEN= resolve))"
if [ "$got" = "tok-from-stdin" ]; then
    note "stdin token is used"
else
    fail "stdin token not used; got '${got}'"
fi

got="$(printf 'tok-from-stdin\n' | (RUNNER_TOKEN=tok-from-env resolve))"
if [ "$got" = "tok-from-stdin" ]; then
    note "stdin wins over RUNNER_TOKEN"
else
    fail "stdin did not win over the environment; got '${got}'"
fi

got="$(printf '' | (RUNNER_TOKEN=tok-from-env resolve))"
if [ "$got" = "tok-from-env" ]; then
    note "falls back to RUNNER_TOKEN when stdin is empty"
else
    fail "no fallback to RUNNER_TOKEN; got '${got}'"
fi

got="$(printf '' | (RUNNER_TOKEN= resolve))"
if [ -z "$got" ]; then
    note "yields empty when neither source has a token"
else
    fail "invented a token from nowhere: '${got}'"
fi

# ── the registration itself must be org-scoped and group-aware ───────────────
if grep -q 'runnergroup' "$entrypoint"; then
    note "config.sh is passed --runnergroup"
else
    fail "entrypoint.sh does not pass --runnergroup"
fi

if grep -q 'RUNNER_SCOPE_URL' "$entrypoint"; then
    note "registration URL comes from RUNNER_SCOPE_URL"
else
    fail "entrypoint.sh still hardcodes a repository URL"
fi

if grep -q 'rm -f /usr/local/bin/sccache' "$entrypoint"; then
    fail "the start-time sccache wipe is still here; it belongs in the job hook"
else
    note "start-time sccache wipe removed"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall entrypoint token-resolution checks passed\n'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/test_runner_entrypoint.sh`
Expected: FAIL — `read_runner_token: command not found`, and the `--runnergroup` / `RUNNER_SCOPE_URL` checks report missing.

- [ ] **Step 3: Rewrite `entrypoint.sh`**

Replace the whole of `.github/self-hosted-runner/entrypoint.sh` with:

```bash
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
```

- [ ] **Step 4: Run the test**

Run: `bash scripts/test_runner_entrypoint.sh`
Expected: PASS — ending `all entrypoint token-resolution checks passed`.

- [ ] **Step 5: Commit**

```bash
git add .github/self-hosted-runner/entrypoint.sh scripts/test_runner_entrypoint.sh
git commit -m "Take the registration token off the container environment

Jobs on these containers are contributor-authored code from public
repositories and can read /proc/1/environ, so a live registration token
in the environment is an enrolment credential handed to anyone opening a
PR. The token now arrives on stdin, with RUNNER_TOKEN kept as a fallback
for the plain docker-run path. Registration is org-scoped and joins the
linux-docker runner group."
```

---

## Task 3: Job-started hook for compile-cache hygiene

**Files:**
- Create: `.github/self-hosted-runner/job-started-hook.sh`
- Create: `scripts/test_runner_job_hook.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `job-started-hook.sh`, wired in Task 4 via `ACTIONS_RUNNER_HOOK_JOB_STARTED`. Honours `LASTRADA_RUNNER_HOOK_DIRS` (space-separated directory list) so the self-test can point it at a scratch tree.

- [ ] **Step 1: Write the failing self-test**

Create `scripts/test_runner_job_hook.sh`:

```bash
#!/usr/bin/env bash
# Usage: bash scripts/test_runner_job_hook.sh
#
# Self-test for .github/self-hosted-runner/job-started-hook.sh.
#
# CompileCache.cmake auto-installs fastcache-cc only when NONE of
# fastcache-cc/sccache/ccache is already on PATH. Now that one container
# serves morph, fastcached and Lightweight, a previous job can leave a
# compile-cache binary behind -- hendrikmuhs/ccache-action apt-installs
# ccache; fastcached's own jobs install sccache -- and silently cost the next
# job its connection to the host fastcached daemon. The loss is invisible:
# the build still succeeds, just uncached.
#
# The asymmetry is the subtle part and is asserted in both directions:
# sccache and ccache must go, fastcache-cc must STAY. Removing fastcache-cc
# would force a fresh download on every single job.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly hook="${repo_root}/.github/self-hosted-runner/job-started-hook.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "${scratch}/bin_a" "${scratch}/bin_b"

for f in "${scratch}/bin_a/sccache" "${scratch}/bin_a/ccache" \
         "${scratch}/bin_a/fastcache-cc" "${scratch}/bin_b/ccache"; do
    printf '#!/bin/sh\nexit 0\n' >"$f"
    chmod +x "$f"
done

LASTRADA_RUNNER_HOOK_DIRS="${scratch}/bin_a ${scratch}/bin_b" bash "$hook" >/dev/null

for gone in "${scratch}/bin_a/sccache" "${scratch}/bin_a/ccache" \
            "${scratch}/bin_b/ccache"; do
    if [ -e "$gone" ]; then
        fail "$(basename "$gone") survived in $(dirname "$gone")"
    else
        note "removed $(basename "$gone") from $(basename "$(dirname "$gone")")"
    fi
done

if [ -e "${scratch}/bin_a/fastcache-cc" ]; then
    note "fastcache-cc left in place"
else
    fail "fastcache-cc was removed; every job would re-download it"
fi

# A directory that does not exist must not fail the job before it starts.
if LASTRADA_RUNNER_HOOK_DIRS="${scratch}/absent" bash "$hook" >/dev/null 2>&1; then
    note "a missing directory is tolerated"
else
    fail "a missing directory made the hook fail, which would fail the job"
fi

# Running twice must be a no-op, not an error.
if LASTRADA_RUNNER_HOOK_DIRS="${scratch}/bin_a ${scratch}/bin_b" \
   bash "$hook" >/dev/null 2>&1; then
    note "re-running on an already-clean tree succeeds"
else
    fail "the hook is not idempotent"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall job-hook checks passed\n'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/test_runner_job_hook.sh`
Expected: FAIL — `bash: .../job-started-hook.sh: No such file or directory`.

- [ ] **Step 3: Write the hook**

Create `.github/self-hosted-runner/job-started-hook.sh`:

```bash
#!/usr/bin/env bash
# ACTIONS_RUNNER_HOOK_JOB_STARTED -- runs before every job on this container.
#
# Removes sccache and ccache from the PATH directories a job can install into.
# CompileCache.cmake's FASTCACHE_AUTO_INSTALL path fires only when none of
# fastcache-cc/sccache/ccache is already on PATH, so a compile-cache binary
# left behind by a previous job silently costs the next one its connection to
# the host fastcached daemon -- the build still succeeds, just uncached.
#
# This has to run between jobs, not at container start: one fleet now serves
# morph, fastcached and Lightweight, and the contamination happens when a
# Lightweight job (hendrikmuhs/ccache-action apt-installs ccache) is followed
# by a morph job on the same container.
#
# fastcache-cc is deliberately NOT removed. When it is present, auto-install
# is unnecessary and CompileCache.cmake simply uses it, which is the outcome
# we want; removing it would force a re-download on every job.
#
# Never fails the job: a hook that exits non-zero aborts the run it was meant
# to help.
set -euo pipefail

dirs="${LASTRADA_RUNNER_HOOK_DIRS:-/usr/local/bin /usr/bin /home/runner/.cargo/bin}"

for dir in $dirs; do
    [ -d "$dir" ] || continue
    for tool in sccache ccache; do
        target="${dir}/${tool}"
        [ -e "$target" ] || continue
        if rm -f "$target" 2>/dev/null || sudo rm -f "$target" 2>/dev/null; then
            echo "job-started-hook: removed ${target}"
        else
            echo "job-started-hook: could not remove ${target}, continuing" >&2
        fi
    done
done

exit 0
```

- [ ] **Step 4: Make it executable and run the test**

```bash
chmod +x .github/self-hosted-runner/job-started-hook.sh
bash scripts/test_runner_job_hook.sh
```
Expected: PASS — ending `all job-hook checks passed`.

- [ ] **Step 5: Commit**

```bash
git add .github/self-hosted-runner/job-started-hook.sh scripts/test_runner_job_hook.sh
git commit -m "Clear leftover compile-cache binaries before every job

With one fleet serving three repositories, a ccache installed by a
Lightweight job silently disables morph's fastcache auto-install on the
next job -- an uncached build that still reports success. The runner's
own job-started hook clears sccache and ccache between jobs, and leaves
fastcache-cc alone so it is not re-downloaded every time."
```

---

## Task 4: Image gains the packages the other two repositories need

**Files:**
- Modify: `.github/self-hosted-runner/Dockerfile`

**Interfaces:**
- Consumes: `job-started-hook.sh` from Task 3.
- Produces: an image with `pkg-config`, `zstd`, `unzip`, `file`, `rsync`, `en_US.UTF-8`, and `ACTIONS_RUNNER_HOOK_JOB_STARTED` set.

- [ ] **Step 1: Add the package layer**

In `.github/self-hosted-runner/Dockerfile`, immediately **after** the `RUN add-apt-repository -y ppa:ubuntu-toolchain-r/test ...` block and **before** `RUN useradd -m -s /bin/bash runner`, insert:

```dockerfile
# Packages GitHub-hosted ubuntu-24.04 ships that morph's own jobs never
# needed, but fastcached's and Lightweight's do now that one fleet serves all
# three repositories. Each was found absent by auditing those two repos'
# Linux jobs against this image, not guessed:
#
#   pkg-config  Lightweight resolves unixodbc, libzip and yaml-cpp through
#               find_package(PkgConfig). The one most likely to be a hard
#               configure failure rather than a slowdown.
#   zstd        what actions/cache compresses with when it is present; both
#               repos use actions/cache, and without it they fall back to gzip.
#   unzip       assumed present by several actions and archive steps.
#   file/rsync  shelled out to by some actions.
#   locales     the image otherwise has only C/C.utf8, so tests asserting
#               formatted output diverge from GitHub-hosted.
#
# LANG is deliberately NOT set here: generating the locale lets a job opt in,
# while forcing it would change the behaviour of morph's existing jobs as a
# side effect of a change made for two other repositories.
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        pkg-config \
        zstd \
        unzip \
        file \
        rsync \
        locales \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*
```

- [ ] **Step 2: Ship and wire the job hook**

In the same file, find:

```dockerfile
COPY --chown=runner:runner entrypoint.sh /home/runner/entrypoint.sh
RUN sudo chmod +x /home/runner/entrypoint.sh
```

Replace with:

```dockerfile
COPY --chown=runner:runner entrypoint.sh /home/runner/entrypoint.sh
COPY --chown=runner:runner job-started-hook.sh /home/runner/job-started-hook.sh
RUN sudo chmod +x /home/runner/entrypoint.sh /home/runner/job-started-hook.sh

# The runner reads this from its own process environment and runs the hook
# before every job. See job-started-hook.sh for why it must be per-job rather
# than per-container-start.
ENV ACTIONS_RUNNER_HOOK_JOB_STARTED=/home/runner/job-started-hook.sh
```

- [ ] **Step 3: Build the image**

```bash
docker build -t lastrada-runner:latest .github/self-hosted-runner
```
Expected: build succeeds.

- [ ] **Step 4: Verify the image contents**

```bash
docker run --rm --entrypoint bash lastrada-runner:latest -c '
  set -e
  for p in pkg-config zstd unzip file rsync; do
    command -v $p >/dev/null || { echo "MISSING: $p"; exit 1; }
  done
  locale -a | grep -qi "en_US.utf8" || { echo "MISSING: en_US.UTF-8"; exit 1; }
  [ -x /home/runner/job-started-hook.sh ] || { echo "MISSING: hook"; exit 1; }
  [ "$ACTIONS_RUNNER_HOOK_JOB_STARTED" = /home/runner/job-started-hook.sh ] \
    || { echo "hook not wired"; exit 1; }
  echo "image contents OK"
'
```
Expected: `image contents OK`.

- [ ] **Step 5: Commit**

```bash
git add .github/self-hosted-runner/Dockerfile
git commit -m "Add the packages fastcached and Lightweight builds need

Audited both repositories' Linux jobs against this image: pkg-config,
zstd, unzip, file, rsync and an en_US.UTF-8 locale are shipped by
GitHub-hosted ubuntu-24.04 and were absent here. pkg-config is the
load-bearing one -- Lightweight resolves unixodbc, libzip and yaml-cpp
through find_package(PkgConfig). Also ships and wires the job-started
hook."
```

---

## Task 5: systemd units and the installer

**Files:**
- Create: `.github/self-hosted-runner/lastrada-runner@.service.in`
- Create: `.github/self-hosted-runner/install-runner-units.sh`
- Create: `.github/self-hosted-runner/config.example`
- Create: `scripts/test_runner_units.sh`

**Interfaces:**
- Consumes: `run-runner.sh` from Task 1.
- Produces: `install-runner-units.sh`, honouring `LASTRADA_RUNNER_PREFIX_DIR` (install root, for tests) and `LASTRADA_RUNNER_NO_SYSTEMCTL=1` (generate files without calling `systemctl`). Template placeholders: `@LIBEXEC@`, `@CONFIG@`, `@PREFIX@`, `@WANTEDBY@`.

- [ ] **Step 1: Write the failing self-test**

Create `scripts/test_runner_units.sh`:

```bash
#!/usr/bin/env bash
# Usage: bash scripts/test_runner_units.sh
#
# Self-test for .github/self-hosted-runner/install-runner-units.sh and the
# unit template it renders.
#
# The unit is the whole supervision story, and two of its directives are the
# difference between a visible failure and the six-day silent outage this
# change exists to prevent:
#
#   StartLimitBurst/StartLimitIntervalSec must land in [Unit], not [Service].
#   systemd moved them in v229 and silently ignores them in the wrong
#   section -- the unit would then retry forever exactly like the Docker
#   restart policy it replaces, and nothing would ever show as failed.
#
#   Restart=always is what brings a runner back after an OOM kill without
#   waiting for a reboot.
#
# Neither is observable without rendering the unit, so it is rendered into a
# scratch prefix and inspected, then handed to `systemd-analyze verify`.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly installer="${repo_root}/.github/self-hosted-runner/install-runner-units.sh"

failures=0
note() { printf 'ok: %s\n' "$*"; }
fail() {
    printf 'error: %s\n' "$*" >&2
    failures=$((failures + 1))
}

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

LASTRADA_RUNNER_PREFIX_DIR="$scratch" LASTRADA_RUNNER_NO_SYSTEMCTL=1 \
    bash "$installer" >/dev/null

unit="$(find "$scratch" -name 'lastrada-runner@.service' -print -quit)"
if [ -z "$unit" ]; then
    fail "no lastrada-runner@.service was rendered"
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
note "unit rendered at ${unit#"$scratch"/}"

if grep -q '@[A-Z]*@' "$unit"; then
    fail "unsubstituted placeholders remain: $(grep -o '@[A-Z]*@' "$unit" | sort -u | tr '\n' ' ')"
else
    note "all placeholders substituted"
fi

# StartLimit* must be in [Unit]; systemd ignores them in [Service].
section=""
start_limit_section=""
while IFS= read -r line; do
    case "$line" in
        \[*\]) section="$line" ;;
        StartLimitBurst=*) start_limit_section="$section" ;;
    esac
done <"$unit"

if [ "$start_limit_section" = "[Unit]" ]; then
    note "StartLimitBurst is in [Unit]"
else
    fail "StartLimitBurst is in '${start_limit_section:-nowhere}', must be [Unit] or systemd ignores it"
fi

for directive in 'Restart=always' 'StartLimitBurst=5' 'StartLimitIntervalSec=600'; do
    if grep -qxF "$directive" "$unit"; then
        note "${directive} present"
    else
        fail "${directive} missing"
    fi
done

if grep -q 'ExecStart=.*run-runner.sh %i' "$unit"; then
    note "ExecStart calls run-runner.sh with the instance index"
else
    fail "ExecStart does not call run-runner.sh %i"
fi

config="$(find "$scratch" -name config -print -quit)"
if [ -n "$config" ] && grep -q '^RUNNER_CPUS=2$' "$config"; then
    note "config written with RUNNER_CPUS=2"
else
    fail "config missing or RUNNER_CPUS is not 2"
fi

if [ -n "$config" ] && grep -q 'morph-docker' "$config"; then
    fail "the retired morph-docker name/label is in the config"
else
    note "config carries no morph-docker name or label"
fi

wrapper="$(find "$scratch" -name run-runner.sh -print -quit)"
if [ -n "$wrapper" ] && [ -x "$wrapper" ]; then
    note "wrapper installed and executable"
else
    fail "wrapper not installed or not executable"
fi

# systemd's own opinion of the rendered unit.
if command -v systemd-analyze >/dev/null 2>&1; then
    staging="${scratch}/verify"
    mkdir -p "$staging"
    # Instantiate %i rather than verifying the template itself: systemd-analyze
    # cannot resolve %i in a bare foo@.service path, and would fail for that
    # reason rather than for any defect in the unit.
    sed 's/%i/1/g' "$unit" >"${staging}/lastrada-runner-verify.service"
    if out="$(systemd-analyze verify "${staging}/lastrada-runner-verify.service" 2>&1)"; then
        note "systemd-analyze verify accepts the unit"
    else
        # Missing ExecStart targets are expected in a scratch prefix; anything
        # else is a real defect in the unit.
        if printf '%s\n' "$out" | grep -vq 'Command .* is not executable'; then
            fail "systemd-analyze verify rejected the unit:"
            printf '%s\n' "$out" >&2
        else
            note "systemd-analyze verify clean apart from the scratch ExecStart path"
        fi
    fi
else
    note "systemd-analyze absent; skipping unit verification"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall unit-installation checks passed\n'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/test_runner_units.sh`
Expected: FAIL — `bash: .../install-runner-units.sh: No such file or directory`.

- [ ] **Step 3: Write the unit template**

Create `.github/self-hosted-runner/lastrada-runner@.service.in`:

```ini
# Template unit: one instance per runner, lastrada-runner@1 .. lastrada-runner@N.
# Rendered by install-runner-units.sh, which substitutes the @...@ paths for
# either a user install (systemctl --user) or a system one.
[Unit]
Description=LASTRADA self-hosted GitHub Actions runner %i
Documentation=https://github.com/LASTRADA-Software/morph/blob/master/.github/self-hosted-runner/README.md
After=docker.service network-online.target
Wants=network-online.target

# StartLimit* belong in [Unit] -- systemd moved them here in v229 and ignores
# them in [Service]. That is not cosmetic: without a working start limit this
# unit retries forever exactly like the `--restart unless-stopped` policy it
# replaces, and a broken credential produces thousands of silent retries
# instead of a failed unit somebody can see. Five attempts in ten minutes,
# then stop and show up red in `systemctl status`.
StartLimitIntervalSec=600
StartLimitBurst=5

[Service]
Type=exec
Environment=LASTRADA_RUNNER_CONFIG=@CONFIG@
ExecStart=@LIBEXEC@/run-runner.sh %i

# --rm covers a clean exit; this covers a SIGKILL or a Docker daemon restart
# leaving the container behind still holding the name. `-` so a missing
# container is not a stop failure.
ExecStopPost=-/usr/bin/docker rm -f @PREFIX@-%i

# Brings a runner back after an OOM kill mid-link without waiting for a
# reboot. Each restart re-runs run-runner.sh, which mints a fresh token --
# there is no stored credential that can go stale.
Restart=always
RestartSec=30

# The runner drains its current job on SIGTERM; give it room before SIGKILL.
TimeoutStopSec=90

[Install]
WantedBy=@WANTEDBY@
```

- [ ] **Step 4: Write `config.example`**

Create `.github/self-hosted-runner/config.example`:

```sh
# Copied to ~/.config/lastrada-runner/config (user install) or
# /etc/lastrada-runner/config (system install) by install-runner-units.sh.

# Registration is organisation-level so one fleet serves morph, fastcached
# and Lightweight.
GITHUB_ORG=LASTRADA-Software

# The runner group must allow public repositories -- all three consuming
# repositories are public, and the organisation's Default group does not.
RUNNER_GROUP=linux-docker

RUNNER_COUNT=5
RUNNER_NAME_PREFIX=lastrada-docker
RUNNER_LABELS=self-hosted,Linux,X64,lastrada-docker

# Five runners x 2 CPUs uses 10 of this host's 12, leaving two for the OS and
# the natively-running fastcached daemon. Matches CMAKE_BUILD_PARALLEL_LEVEL.
RUNNER_CPUS=2
RUNNER_MEMORY=6g

RUNNER_IMAGE=lastrada-runner:latest

# fastcached runs on the host (bound to the Docker bridge), not in a
# container; host.docker.internal is how a container reaches it.
FASTCACHE_ADDR=host.docker.internal:6674
CMAKE_BUILD_PARALLEL_LEVEL=2
```

- [ ] **Step 5: Write the installer**

Create `.github/self-hosted-runner/install-runner-units.sh`:

```bash
#!/usr/bin/env bash
# Installs the systemd units that supervise the self-hosted runner fleet.
#
# Two modes, chosen by who runs it:
#
#   as a normal user -> user units under ~/.config/systemd/user, controlled
#                       with `systemctl --user`. Needs lingering
#                       (`loginctl enable-linger $USER`) to start at boot
#                       without a login session. This is the mode used on the
#                       maintainer's workstation, where `gh` is already
#                       authenticated for that user.
#   as root          -> system units under /etc/systemd/system. This is what
#                       bootstrap-cloud-node.sh uses on a fresh VM.
#
# LASTRADA_RUNNER_PREFIX_DIR redirects every install path under one directory,
# and LASTRADA_RUNNER_NO_SYSTEMCTL=1 skips the systemctl calls, so
# scripts/test_runner_units.sh can render and inspect the unit without
# touching the real system.
set -euo pipefail

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -eq 0 ]; then
    unit_dir="/etc/systemd/system"
    config_dir="/etc/lastrada-runner"
    libexec_dir="/usr/local/libexec/lastrada-runner"
    wantedby="multi-user.target"
    systemctl=(systemctl)
else
    unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/lastrada-runner"
    libexec_dir="${XDG_DATA_HOME:-$HOME/.local/share}/lastrada-runner"
    wantedby="default.target"
    systemctl=(systemctl --user)
fi

if [ -n "${LASTRADA_RUNNER_PREFIX_DIR:-}" ]; then
    unit_dir="${LASTRADA_RUNNER_PREFIX_DIR}${unit_dir}"
    config_dir="${LASTRADA_RUNNER_PREFIX_DIR}${config_dir}"
    libexec_dir="${LASTRADA_RUNNER_PREFIX_DIR}${libexec_dir}"
fi

mkdir -p "$unit_dir" "$config_dir" "$libexec_dir"

install -m 0755 "${here}/run-runner.sh" "${libexec_dir}/run-runner.sh"

# Never clobber a config an operator has tuned; config.example is the
# reference, the installed file is theirs.
if [ ! -e "${config_dir}/config" ]; then
    install -m 0600 "${here}/config.example" "${config_dir}/config"
    echo "wrote ${config_dir}/config"
else
    echo "kept existing ${config_dir}/config"
fi

# shellcheck disable=SC1091
. "${config_dir}/config"
: "${RUNNER_COUNT:?config must set RUNNER_COUNT}"
: "${RUNNER_NAME_PREFIX:?config must set RUNNER_NAME_PREFIX}"

sed \
    -e "s|@LIBEXEC@|${libexec_dir}|g" \
    -e "s|@CONFIG@|${config_dir}/config|g" \
    -e "s|@PREFIX@|${RUNNER_NAME_PREFIX}|g" \
    -e "s|@WANTEDBY@|${wantedby}|g" \
    "${here}/lastrada-runner@.service.in" >"${unit_dir}/lastrada-runner@.service"
echo "wrote ${unit_dir}/lastrada-runner@.service"

if [ -n "${LASTRADA_RUNNER_NO_SYSTEMCTL:-}" ]; then
    echo "LASTRADA_RUNNER_NO_SYSTEMCTL set; not touching systemd"
    exit 0
fi

"${systemctl[@]}" daemon-reload

# Disable instances left over from a previously larger fleet, so shrinking
# RUNNER_COUNT actually shrinks it.
for existing in $("${systemctl[@]}" list-unit-files 'lastrada-runner@*.service' \
                    --no-legend --plain 2>/dev/null | awk '{print $1}'); do
    idx="${existing#lastrada-runner@}"
    idx="${idx%.service}"
    if [ "$idx" -gt "$RUNNER_COUNT" ] 2>/dev/null; then
        echo "disabling ${existing} (beyond RUNNER_COUNT=${RUNNER_COUNT})"
        "${systemctl[@]}" disable --now "$existing" || true
    fi
done

for i in $(seq 1 "$RUNNER_COUNT"); do
    "${systemctl[@]}" enable --now "lastrada-runner@${i}.service"
    echo "enabled lastrada-runner@${i}"
done

echo
echo "status:  ${systemctl[*]} status 'lastrada-runner@*'"
echo "logs:    journalctl${systemctl[1]:+ --user} -u 'lastrada-runner@1' -f"
```

- [ ] **Step 6: Make executable and run the test**

```bash
chmod +x .github/self-hosted-runner/install-runner-units.sh
bash scripts/test_runner_units.sh
```
Expected: PASS — ending `all unit-installation checks passed`.

- [ ] **Step 7: Commit**

```bash
git add .github/self-hosted-runner/lastrada-runner@.service.in \
        .github/self-hosted-runner/install-runner-units.sh \
        .github/self-hosted-runner/config.example \
        scripts/test_runner_units.sh
git commit -m "Hand the runner lifecycle to systemd

A template unit per runner, with Restart=always to recover an OOM-killed
container and a start limit that turns a broken credential into a failed
unit after five attempts instead of thousands of silent retries. The
start limit is in [Unit], where systemd has read it since v229 -- the
self-test asserts that, because in [Service] it is ignored and the unit
would loop forever."
```

---

## Task 6: `ci.yml` probes the organisation

**Files:**
- Modify: `.github/workflows/ci.yml:115-164` (the `probe-self-hosted` job)
- Modify: `.github/workflows/ci.yml` (`deprecation-lint` job, ~line 2004) to gate the four new self-tests

**Interfaces:**
- Consumes: the `lastrada-docker` label from Task 1/2.
- Produces: `probe-self-hosted.outputs.runs_on` is either `["ubuntu-24.04"]` or `["self-hosted", "Linux", "X64", "lastrada-docker"]`. All downstream `runs-on` expressions are unchanged; only the label inside the JSON changes.

- [ ] **Step 1: Replace the probe job**

In `.github/workflows/ci.yml`, replace the comment block and job from `# Picks the self-hosted morph-docker runner...` through the end of the `probe-self-hosted` job (up to but not including `# ── Linux: GCC and Clang plain builds ──`) with:

```yaml
  # Picks a self-hosted lastrada-docker runner for the Linux legs when one is
  # registered, online, and idle; falls back to the GitHub-hosted label
  # otherwise. The runners are ORGANISATION-level (one fleet serves morph,
  # fastcached and Lightweight), so this queries the org endpoint -- the
  # repo endpoint returns zero for them and every run would silently go
  # GitHub-hosted with no error anywhere.
  #
  # GITHUB_TOKEN can't call the runners API, so this needs
  # RUNNER_STATUS_TOKEN: a PAT with organisation self-hosted-runner read
  # access (admin:org, or fine-grained "Self-hosted runners: Read-only" on
  # LASTRADA-Software), stored as a repo secret. Forked-repo PRs don't get
  # repo secrets at all (the token comes through empty), which this treats
  # the same as "no runner online": fall back to GitHub-hosted. See
  # .github/self-hosted-runner/README.md for the fleet itself.
  probe-self-hosted:
    name: Probe self-hosted runner
    runs-on: ubuntu-24.04
    outputs:
      runs_on: ${{ steps.probe.outputs.runs_on }}
    steps:
      - name: Check for an idle lastrada-docker runner
        id: probe
        env:
          RUNNER_STATUS_TOKEN: ${{ secrets.RUNNER_STATUS_TOKEN }}
        run: |
          set -euo pipefail
          fallback='["ubuntu-24.04"]'

          if [ -z "${RUNNER_STATUS_TOKEN}" ]; then
            echo "No RUNNER_STATUS_TOKEN (e.g. a forked-repo PR) -- using GitHub-hosted."
            echo "runs_on=${fallback}" >> "$GITHUB_OUTPUT"
            exit 0
          fi

          runners=$(curl -fsSL \
            -H "Authorization: Bearer ${RUNNER_STATUS_TOKEN}" \
            -H "Accept: application/vnd.github+json" \
            "https://api.github.com/orgs/LASTRADA-Software/actions/runners") || {
              echo "Runners API call failed -- using GitHub-hosted."
              echo "runs_on=${fallback}" >> "$GITHUB_OUTPUT"
              exit 0
            }

          online=$(echo "${runners}" | jq '[.runners[] |
            select(.status == "online" and .busy == false and
                   (.labels | map(.name) | contains(["lastrada-docker"])))] | length')

          if [ "${online}" -gt 0 ]; then
            echo "Found an idle lastrada-docker runner -- using self-hosted."
            echo 'runs_on=["self-hosted", "Linux", "X64", "lastrada-docker"]' >> "$GITHUB_OUTPUT"
          else
            echo "No idle lastrada-docker runner -- using GitHub-hosted."
            echo "runs_on=${fallback}" >> "$GITHUB_OUTPUT"
          fi
```

- [ ] **Step 2: Verify no `morph-docker` reference survives in the workflow**

```bash
grep -n "morph-docker" .github/workflows/ci.yml || echo "clean"
```
Expected: `clean`.

- [ ] **Step 3: Gate the new self-tests**

In `.github/workflows/ci.yml`, in the `deprecation-lint` job, after the existing `- name: Self-test the deprecation-marker checker` step, add:

```yaml
      # The runner fleet's scripts are not exercised by any build, so their
      # self-tests are the only thing standing between a typo and a fleet
      # that silently stops registering. See each test's own header.
      - name: Self-test the runner launch contract
        run: bash scripts/test_run_runner.sh

      - name: Self-test the runner entrypoint token resolution
        run: bash scripts/test_runner_entrypoint.sh

      - name: Self-test the runner job-started hook
        run: bash scripts/test_runner_job_hook.sh

      - name: Self-test the runner systemd units
        run: bash scripts/test_runner_units.sh
```

- [ ] **Step 4: Validate the workflow parses and run all four self-tests**

```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); print('ci.yml parses')"
for t in run_runner runner_entrypoint runner_job_hook runner_units; do
  echo "== $t =="; bash "scripts/test_$t.sh" >/dev/null && echo PASS || echo FAIL
done
```
Expected: `ci.yml parses`, then `PASS` four times.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "Probe the organisation for idle runners

The fleet registers org-level now, so the repo runners endpoint returns
zero for it -- every run would fall back to GitHub-hosted silently, with
no error to notice. Switches to the org endpoint and the lastrada-docker
label, and gates the four runner self-tests in deprecation-lint.

RUNNER_STATUS_TOKEN must be reissued with org runner read access before
this finds anything."
```

---

## Task 7: `bootstrap-cloud-node.sh` installs units

**Files:**
- Modify: `.github/self-hosted-runner/bootstrap-cloud-node.sh`

**Interfaces:**
- Consumes: `install-runner-units.sh` and `config.example` from Task 5.
- Produces: a cloud VM whose runners are systemd-supervised, matching this host.

- [ ] **Step 1: Drop the `RUNNER_TOKEN` requirement from the header and preflight**

Replace the `# Required env var:` block in the header comment with:

```bash
# No registration token is needed up front: each runner mints its own, fresh,
# on every start (see run-runner.sh). This box needs `gh` authenticated with
# admin:org on LASTRADA-Software instead -- a credential that stays on the
# host and never enters a container.
```

Then delete the preflight block that exits when `RUNNER_TOKEN` is unset:

```bash
if [ -z "${RUNNER_TOKEN:-}" ]; then
    echo "ERROR: RUNNER_TOKEN is not set. Mint one with:" >&2
    echo "  gh api -X POST repos/LASTRADA-Software/morph/actions/runners/registration-token --jq '.token'" >&2
    exit 1
fi
```

and replace it with:

```bash
if ! command -v gh >/dev/null 2>&1; then
    echo "ERROR: the gh CLI is required -- each runner mints its own" >&2
    echo "       registration token on every start." >&2
    exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
    echo "ERROR: gh is not authenticated. Needs admin:org on LASTRADA-Software." >&2
    exit 1
fi
```

- [ ] **Step 2: Replace the worker-start section**

Replace the whole of section `# ── 7. Start the workers ──` (the `for i in $(seq 1 "$WORKER_COUNT")` loop and its preamble comment) with:

```bash
# ── 7. Configure and start the workers ──────────────────────────────────
# systemd supervises the containers, not Docker: one owner, a fresh
# registration token per start, and a bounded start limit so a broken
# credential shows up as a failed unit instead of retrying forever. The
# per-worker token minting that used to live here is gone -- run-runner.sh
# mints one per start, which covers the single-use property by construction.
echo ""
echo "=== Installing systemd units for ${WORKER_COUNT} worker(s) ==="

$SUDO mkdir -p /etc/lastrada-runner
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

$SUDO bash "$MORPH_ROOT/.github/self-hosted-runner/install-runner-units.sh"
```

- [ ] **Step 3: Update the closing summary**

Replace the final `echo "workers: ..."` and `echo "verify runners: ..."` lines with:

```bash
echo "workers:         systemctl status 'lastrada-runner@*'"
echo "worker logs:     journalctl -u 'lastrada-runner@1' -f"
echo "verify runners:  gh api orgs/LASTRADA-Software/actions/runners --jq '.runners[] | {name,status,busy}'"
```

- [ ] **Step 4: Verify it parses and no stale token logic remains**

```bash
bash -n .github/self-hosted-runner/bootstrap-cloud-node.sh && echo "syntax OK"
grep -n "RUNNER_TOKEN\|--restart unless-stopped\|registration-token" \
  .github/self-hosted-runner/bootstrap-cloud-node.sh || echo "no stale token logic"
```
Expected: `syntax OK`, then `no stale token logic`.

- [ ] **Step 5: Commit**

```bash
git add .github/self-hosted-runner/bootstrap-cloud-node.sh
git commit -m "Bootstrap cloud nodes onto systemd units

The script baked one registration token into every container at creation
time, which is the bug this branch exists to fix -- a cloud VM rebooting
an hour later hit the same restart loop. It now writes the fleet config
and installs the units; the per-worker token-minting loop is gone,
because run-runner.sh mints one per start by construction."
```

---

## Task 8: README tells the truth

**Files:**
- Modify: `.github/self-hosted-runner/README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: no code; documentation consistent with the implementation.

- [ ] **Step 1: Replace the wrong restart section**

Replace the entire `### Docker Desktop / host restarts` section (the paragraph beginning "These are long-lived containers (`--restart unless-stopped`)" through "…`docker logs <name>` is the first thing to check.") with:

```markdown
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
failure is now bounded and visible: `StartLimitBurst=5` puts the unit into
`failed` after five attempts in ten minutes, so `systemctl --user status
'lastrada-runner@*'` shows red instead of a loop nobody looks at.

Hosts without systemd (Docker Desktop, WSL2) still use the plain `docker
run` path below. That path is reboot-fragile by design: it has the same
one-hour token, and re-running the `docker run` command with a fresh token
is the recovery.
```

- [ ] **Step 2: Replace the quick start**

Replace steps 3 and 4 of the **Quick start** section with:

```markdown
# 3. Install the systemd units. No token needed -- each runner mints its
#    own on every start. Needs `gh` authenticated with admin:org on
#    LASTRADA-Software; that credential stays on this host and never
#    enters a container.
bash .github/self-hosted-runner/install-runner-units.sh

# 4. Check it registered.
systemctl --user status 'lastrada-runner@*'
gh api orgs/LASTRADA-Software/actions/runners \
  --jq '.runners[] | {name,status,busy,labels:[.labels[].name]}'
```

- [ ] **Step 3: Add a section on which repositories use the fleet**

Insert immediately after the opening paragraph of the README:

```markdown
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
```

- [ ] **Step 4: Correct the environment-variable table**

Replace the `RUNNER_TOKEN` row of the **Environment variables** table with:

```markdown
| `RUNNER_TOKEN`  | no       | —                                        | Fallback registration token for the plain `docker run` path only, ~1 hour TTL. Under systemd the token arrives on **stdin** from `run-runner.sh` instead, so it never sits in the container environment where a job could read it out of `/proc/1/environ`. |
| `RUNNER_SCOPE_URL` | no    | `https://github.com/LASTRADA-Software`   | What `config.sh --url` registers against. |
| `RUNNER_GROUP`  | no       | `linux-docker`                           | Runner group to join. Must allow public repositories. |
```

- [ ] **Step 5: Purge stale references**

```bash
grep -n "morph-docker\|unless-stopped\|repos/LASTRADA-Software/morph/actions/runners" \
  .github/self-hosted-runner/README.md
```
Every remaining hit must be inside the historical explanation in "Host restarts". Rewrite any that is not.

- [ ] **Step 6: Commit**

```bash
git add .github/self-hosted-runner/README.md
git commit -m "Correct the README's restart claim and document org-wide use

The 'Docker Desktop / host restarts' section asserted the containers
always re-register cleanly on restart. They did not: that fix addressed
stale .runner files, while the token itself expires an hour after the
container is created, and five containers restart-looped for six days.
Replaces the claim with what actually happens now, and documents the
organisation runner group the three repositories share."
```

---

## Task 9: Roll the fleet over

**Files:** none — this task changes live infrastructure, not the repository.

**Interfaces:**
- Consumes: everything above, committed.
- Produces: five online organisation-level runners.

> Run these in order. Steps 2 and 3 are irreversible-ish: the old registrations are deleted and the containers destroyed. Both are recreated by step 4.

- [ ] **Step 1: Create the runner group**

```bash
cd /tmp && python3 - <<'PY'
import json, subprocess
ids = [int(subprocess.run(["gh","api",f"repos/LASTRADA-Software/{r}","--jq",".id"],
        capture_output=True, text=True, check=True).stdout.strip())
       for r in ("morph","fastcached","Lightweight")]
json.dump({"name":"linux-docker","visibility":"selected",
           "allows_public_repositories":True,"selected_repository_ids":ids},
          open("/tmp/linux-docker-group.json","w"))
print(ids)
PY
gh api -X POST orgs/LASTRADA-Software/actions/runner-groups \
  --input /tmp/linux-docker-group.json --jq '{id,name,visibility,allows_public_repositories}'
```
Expected: a JSON object with `"name": "linux-docker"`, `"visibility": "selected"`, `"allows_public_repositories": true`. Note the `id`.

- [ ] **Step 2: Stop and remove the old containers**

```bash
for i in 1 2 3 4 5; do docker rm -f "morph-runner-$i" 2>/dev/null || true; done
docker ps -a --filter name=morph-runner --format '{{.Names}}' || echo "none left"
```
Expected: no `morph-runner-*` containers remain.

- [ ] **Step 3: Delete the old repo-level registrations**

```bash
for id in $(gh api repos/LASTRADA-Software/morph/actions/runners --jq '.runners[].id'); do
  gh api -X DELETE "repos/LASTRADA-Software/morph/actions/runners/$id" && echo "deleted $id"
done
gh api repos/LASTRADA-Software/morph/actions/runners --jq '.total_count'
```
Expected: `0`.

- [ ] **Step 4: Install and start the fleet**

```bash
bash .github/self-hosted-runner/install-runner-units.sh
systemctl --user status 'lastrada-runner@*' --no-pager | head -40
```
Expected: five units `active (running)`.

- [ ] **Step 5: Verify registration**

```bash
gh api orgs/LASTRADA-Software/actions/runners \
  --jq '.runners[] | "\(.name)\t\(.status)\t\(.busy)\t\([.labels[].name]|join(","))"'
```
Expected: five rows `lastrada-docker-N online false self-hosted,Linux,X64,lastrada-docker`.

- [ ] **Step 6: Prove a restart re-registers with a *different* token**

This is the assertion the whole change rests on — that no stored credential can go stale.

```bash
journalctl --user -u lastrada-runner@3 --since "-2min" | grep -c "Listening for Jobs" || true
systemctl --user restart lastrada-runner@3
sleep 25
gh api orgs/LASTRADA-Software/actions/runners \
  --jq '.runners[] | select(.name=="lastrada-docker-3") | {name,status}'
```
Expected: `lastrada-docker-3` is `online` again. Confirms `config.sh` succeeded with a token minted during the restart, not a stored one.

- [ ] **Step 7: Prove stopping leaves nothing behind**

```bash
systemctl --user stop 'lastrada-runner@*'
sleep 5
docker ps -a --filter name=lastrada-docker --format '{{.Names}}' || true
systemctl --user start 'lastrada-runner@*'
```
Expected: no containers listed while stopped; all five back after `start`.

- [ ] **Step 8: Verify the end-to-end build for B and C readiness**

This gates sub-projects B and C: it is the only way to know the image package list is complete rather than guessed.

```bash
docker run --rm -v /tmp/lw:/src lastrada-runner:latest bash -lc '
  set -e
  sudo apt-get update -q
  sudo apt-get install -y --no-install-recommends \
    ninja-build catch2 unixodbc-dev sqlite3 libsqlite3-dev uuid-dev \
    libyaml-cpp-dev libzip-dev g++-14
  git clone --depth 1 https://github.com/LASTRADA-Software/Lightweight /src/Lightweight
  cd /src/Lightweight
  cmake --preset gcc-release
' 2>&1 | tail -30
```
Expected: CMake configure completes. A `find_package(PkgConfig)` failure here means the Task 4 package list is incomplete — fix it there, rebuild, rerun.

- [ ] **Step 9: Commit nothing; report**

No repository change. Record the group id and the live verification output in the PR description.

---

## Self-Review

**Spec coverage.** Every spec section maps to a task: The failure → Task 8 (README) and the commit messages; Lifecycle → Task 5; Minting a token per start → Tasks 1, 2; Configuration → Task 5; Organisation registration → Tasks 1, 2, 9; Labels → Tasks 1, 2, 6; Image contents → Task 4; Compile-cache hygiene → Task 3; The ci.yml probe → Task 6; Installation → Tasks 5, 7; Manual steps → below; Testing → Tasks 1–6, 9.

**Manual steps are not tasks** and are restated here because no task can do them:

1. Reissue `RUNNER_STATUS_TOKEN` in `LASTRADA-Software/morph` with `admin:org` (or fine-grained "Self-hosted runners: Read-only"). Until then the probe's API call fails and every job falls back to GitHub-hosted — degraded, not broken.
2. Add that secret to `fastcached` and `Lightweight` when B and C land.

**Type consistency.** `LASTRADA_RUNNER_CONFIG`, `LASTRADA_RUNNER_DRY_RUN`, `LASTRADA_RUNNER_GH`, `LASTRADA_RUNNER_DOCKER`, `LASTRADA_RUNNER_HOOK_DIRS`, `LASTRADA_RUNNER_PREFIX_DIR`, `LASTRADA_RUNNER_NO_SYSTEMCTL`, `LASTRADA_RUNNER_ENTRYPOINT_SOURCE_ONLY` are each defined in exactly one task and used consistently. `read_runner_token` is defined in Task 2 and referenced only by its own self-test. Template placeholders `@LIBEXEC@`, `@CONFIG@`, `@PREFIX@`, `@WANTEDBY@` are substituted in Task 5 step 5 and asserted in Task 5 step 1.

**Known risk, deliberately left as a test rather than a design change.** Task 1 delivers the token on stdin. If `run.sh` proves intolerant of EOF on stdin, Task 9 step 5 will show runners failing to come online. The fallback is `-e RUNNER_TOKEN` in `run-runner.sh`'s argv, accepting the exposure documented in the spec — do not silently make that change; it reverses a security decision and belongs in review.
