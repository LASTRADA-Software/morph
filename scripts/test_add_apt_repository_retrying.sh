#!/usr/bin/env bash
# Usage: bash scripts/test_add_apt_repository_retrying.sh
#
# Self-test for scripts/add_apt_repository_retrying.sh.
#
# A retry wrapper nobody tests is worth less than no wrapper: it looks like
# resilience while possibly retrying nothing, and the only way to find out is
# during the outage it was meant to survive. So each property is checked by
# driving the script against a stub whose failure pattern the test controls.
#
# The stub records one line per invocation, which is what makes "it retried"
# distinguishable from "it happened to succeed".
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly script="${repo_root}/scripts/add_apt_repository_retrying.sh"
readonly workdir="$(mktemp -d)"
trap 'rm -rf "${workdir}"' EXIT

failures=0

# Writes a stub that fails its first $1 invocations, then succeeds.
make_stub() {
    local fail_times="$1"
    cat > "${workdir}/stub.sh" <<STUB
#!/usr/bin/env bash
echo "call" >> "${workdir}/calls"
attempts=\$(wc -l < "${workdir}/calls" | tr -d ' ')
if [ "\$attempts" -le ${fail_times} ]; then exit 1; fi
exit 0
STUB
    chmod +x "${workdir}/stub.sh"
    : > "${workdir}/calls"
}

calls() { wc -l < "${workdir}/calls" | tr -d ' '; }

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "ok    ${name}"
    else
        echo "FAIL  ${name}: expected '${expected}', got '${actual}'"
        failures=$((failures + 1))
    fi
}

run() {
    MORPH_ADD_APT_REPOSITORY="${workdir}/stub.sh" MORPH_RETRY_BACKOFF_SECONDS=0 \
        bash "${script}" "$@" > "${workdir}/out" 2>&1
}

# 1. A PPA that works first time is added once, with no retry.
make_stub 0
run ppa:example/test 5 && rc=0 || rc=$?
check "succeeds first time: exit status" 0 "${rc}"
check "succeeds first time: one call only" 1 "$(calls)"

# 2. A transient failure is retried and then succeeds -- the whole point.
make_stub 2
run ppa:example/test 5 && rc=0 || rc=$?
check "recovers after two failures: exit status" 0 "${rc}"
check "recovers after two failures: called three times" 3 "$(calls)"

# 3. A persistent outage still fails, rather than silently continuing without
#    the PPA and letting the job build against the distro's older compiler.
make_stub 99
run ppa:example/test 3 && rc=0 || rc=$?
check "persistent outage: exit status" 1 "${rc}"
check "persistent outage: exhausted every attempt" 3 "$(calls)"

# 4. The final failure is reported as a CI error, and names the likely cause so
#    the next person does not start by suspecting their own change.
if grep -q "::error::" "${workdir}/out" && grep -qi "launchpad" "${workdir}/out"; then
    echo "ok    persistent outage: emits an ::error:: naming Launchpad"
else
    echo "FAIL  persistent outage: no ::error:: naming Launchpad"
    failures=$((failures + 1))
fi

# 5. The retry must not be silent: a job that recovered should say so, or a
#    reader cannot tell a slow green from a healthy one.
make_stub 1
run ppa:example/test 5 || true
if grep -q "retrying in" "${workdir}/out"; then
    echo "ok    announces each retry"
else
    echo "FAIL  retried without saying so"
    failures=$((failures + 1))
fi

if [ "${failures}" -ne 0 ]; then
    echo ""
    echo "${failures} check(s) failed."
    exit 1
fi
echo ""
echo "All checks passed."
