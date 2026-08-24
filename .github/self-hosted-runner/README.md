# Self-hosted Linux runner (Docker)

Runs a repo-level GitHub Actions runner for `LASTRADA-Software/morph` inside
a Docker container. Used by `.github/workflows/self-hosted-smoke.yml`
(`runs-on: [self-hosted, Linux, X64, morph-docker]`).

The image is plain Ubuntu 24.04 with just the runner binary and enough
packages (`sudo`, `curl`, `git`, build-essential-adjacent tooling) to
bootstrap a toolchain — it does **not** prebake gcc/clang/sccache. Jobs
install those themselves the same way `ci.yml`'s GitHub-hosted jobs do, so
there is one place, not two, to keep compiler versions in sync.

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

## Environment variables (`entrypoint.sh`)

| Variable        | Required | Default                                | Notes |
|-----------------|----------|-----------------------------------------|-------|
| `RUNNER_TOKEN`  | yes      | —                                        | Registration token, ~1 hour TTL. Mint a fresh one for each `docker run` / re-registration — it is not reusable after the runner has registered once, and a stale token just fails `config.sh`. |
| `RUNNER_NAME`   | no       | `morph-docker-<container hostname>`     | Set this explicitly (e.g. `morph-hetzner-1`) when running more than one runner, so they're distinguishable in the repo's runner list. |
| `RUNNER_LABELS` | no       | `self-hosted,Linux,X64,morph-docker`    | Only change this if you also update the `runs-on:` label list in the workflow(s) that should target it. |

## Trust boundary

A self-hosted runner executes arbitrary job code on whatever host runs the
container — a cloud VM here, or your own machine. Never point
`pull_request:` at this runner without restricting it to same-repo
branches; forked-repo PRs must not be able to run jobs here. See the
`if:` condition in `self-hosted-smoke.yml` for the pattern
(`pull_request.head.repo.full_name == github.repository`) and mirror it
in any new workflow that uses this runner.

## Running more than one runner

Each container is one runner process. To add capacity (e.g. one runner on
this machine, another on a Hetzner box), repeat the Quick start on each
host with a distinct `RUNNER_NAME` — no coordination between them is
needed, they just both poll the same repo's job queue.
