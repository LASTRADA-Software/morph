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
# it is asserted through MORPH_RUNNER_DRY_RUN with stub gh/docker binaries.
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
RUNNER_IMAGE=morph-runner:latest
FASTCACHE_ADDR=host.docker.internal:6674
CMAKE_BUILD_PARALLEL_LEVEL=2
CONF

export STUB_GH_LOG="${scratch}/gh.log"
: >"${STUB_GH_LOG}"

out="$(MORPH_RUNNER_DRY_RUN=1 \
      MORPH_RUNNER_CONFIG="${scratch}/config" \
      MORPH_RUNNER_GH="${scratch}/gh" \
      MORPH_RUNNER_DOCKER="${scratch}/docker" \
      bash "$launcher" 3 2>&1)" || {
    fail "run-runner.sh exited non-zero in dry-run mode:"
    printf '%s\n' "$out" >&2
    exit 1
}

has() { printf '%s\n' "$out" | grep -qxF -- "$1"; }

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
if MORPH_RUNNER_DRY_RUN=1 MORPH_RUNNER_CONFIG="${scratch}/absent" \
   bash "$launcher" 1 >/dev/null 2>&1; then
    fail "a missing config file was accepted"
else
    note "a missing config file is rejected"
fi

# ── a missing index must fail loudly ─────────────────────────────────────────
if MORPH_RUNNER_DRY_RUN=1 MORPH_RUNNER_CONFIG="${scratch}/config" \
   MORPH_RUNNER_GH="${scratch}/gh" MORPH_RUNNER_DOCKER="${scratch}/docker" \
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
