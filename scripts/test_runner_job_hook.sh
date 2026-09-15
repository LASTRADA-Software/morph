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
#
# A second asymmetry cuts across the first: sccache is an unpackaged tarball
# download, but ccache-action apt-installs ccache, so `rm -f`-ing it leaves
# dpkg believing the package is still present -- the next job's
# `apt-get install ccache` becomes a no-op, and the binary never comes back.
# The hook checks ownership with `dpkg -S` and removes owned tools with
# `apt-get remove` instead of `rm`. LASTRADA_RUNNER_DPKG/LASTRADA_RUNNER_APT
# swap in stub binaries below so this is exercised without root or a real
# package manager, the same way LASTRADA_RUNNER_GH/DOCKER stub run-runner.sh.
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

# ── package-owned tools go through apt-get, not rm ──────────────────────────
# dpkg stub: ccache is owned by a package, sccache is not (mirroring reality:
# sccache is never apt-installed). apt stub: records its argv instead of
# touching anything, so a surviving file proves rm was never used on it.
pkg_scratch="${scratch}/pkg"
mkdir -p "${pkg_scratch}/bin"
for f in ccache sccache fastcache-cc; do
    printf '#!/bin/sh\nexit 0\n' >"${pkg_scratch}/bin/${f}"
    chmod +x "${pkg_scratch}/bin/${f}"
done

cat >"${pkg_scratch}/dpkg" <<'DPKG'
#!/bin/sh
# args: -S <path>
case "$2" in
    */ccache) echo "ccache-stub-pkg: $2"; exit 0 ;;
    *) exit 1 ;;
esac
DPKG
chmod +x "${pkg_scratch}/dpkg"

apt_log="${pkg_scratch}/apt.log"
cat >"${pkg_scratch}/apt" <<APT
#!/bin/sh
echo "\$@" >>"${apt_log}"
exit 0
APT
chmod +x "${pkg_scratch}/apt"

LASTRADA_RUNNER_HOOK_DIRS="${pkg_scratch}/bin" \
LASTRADA_RUNNER_DPKG="${pkg_scratch}/dpkg" \
LASTRADA_RUNNER_APT="${pkg_scratch}/apt" \
bash "$hook" >/dev/null

if [ -e "${pkg_scratch}/bin/ccache" ]; then
    note "package-owned ccache was left for apt-get, not rm'd"
else
    fail "package-owned ccache was deleted with rm; dpkg would still think it is installed"
fi

if [ -e "$apt_log" ] && grep -q -- '-qq ccache-stub-pkg' "$apt_log"; then
    note "apt-get remove was invoked for the package owning ccache"
else
    fail "apt-get remove was not invoked for the package owning ccache"
fi

if [ -e "${pkg_scratch}/bin/sccache" ]; then
    fail "unowned sccache survived; it should still be rm'd"
else
    note "unowned sccache was removed with rm"
fi

if [ -e "${pkg_scratch}/bin/fastcache-cc" ]; then
    note "fastcache-cc untouched on the package-aware path too"
else
    fail "fastcache-cc was removed on the package-aware path"
fi

# ── a failing dpkg must not fail the job ────────────────────────────────────
# No package information means "treat as unowned", not "abort the job": the
# tool still gets rm'd.
dpkg_err_scratch="${scratch}/dpkg_errors"
mkdir -p "${dpkg_err_scratch}/bin"
printf '#!/bin/sh\nexit 0\n' >"${dpkg_err_scratch}/bin/ccache"
chmod +x "${dpkg_err_scratch}/bin/ccache"
cat >"${dpkg_err_scratch}/dpkg" <<'DPKG'
#!/bin/sh
echo "dpkg: stub failure" >&2
exit 2
DPKG
chmod +x "${dpkg_err_scratch}/dpkg"

if LASTRADA_RUNNER_HOOK_DIRS="${dpkg_err_scratch}/bin" \
   LASTRADA_RUNNER_DPKG="${dpkg_err_scratch}/dpkg" \
   bash "$hook" >/dev/null 2>&1; then
    note "a failing dpkg does not fail the hook"
else
    fail "a failing dpkg made the hook fail, which would fail the job"
fi

# ── a failing apt-get must not fail the job ─────────────────────────────────
apt_err_scratch="${scratch}/apt_errors"
mkdir -p "${apt_err_scratch}/bin"
printf '#!/bin/sh\nexit 0\n' >"${apt_err_scratch}/bin/ccache"
chmod +x "${apt_err_scratch}/bin/ccache"
cat >"${apt_err_scratch}/dpkg" <<'DPKG'
#!/bin/sh
echo "ccache-stub-pkg: $2"
exit 0
DPKG
chmod +x "${apt_err_scratch}/dpkg"
cat >"${apt_err_scratch}/apt" <<'APT'
#!/bin/sh
echo "apt: stub failure" >&2
exit 1
APT
chmod +x "${apt_err_scratch}/apt"

# The hook's apt-get fallback tries sudo, which can't succeed unattended
# here; timeout guards against any environment where that unexpectedly blocks
# instead of failing fast.
if timeout 15 env LASTRADA_RUNNER_HOOK_DIRS="${apt_err_scratch}/bin" \
   LASTRADA_RUNNER_DPKG="${apt_err_scratch}/dpkg" \
   LASTRADA_RUNNER_APT="${apt_err_scratch}/apt" \
   bash "$hook" >/dev/null 2>&1; then
    note "a failing apt-get does not fail the hook"
else
    fail "a failing apt-get made the hook fail, which would fail the job"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf '\nall job-hook checks passed\n'
