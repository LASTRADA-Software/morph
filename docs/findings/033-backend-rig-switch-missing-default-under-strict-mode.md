---
id: 033
title: "`BackendRig`'s constructor `switch (mode)` has no `default:` label, so every ladder rung's tests fail `-Wswitch-default` the moment `MORPH_ENABLE_STRICT_COMPILATION=ON` is set — pre-existing, not rung-specific"
subsystem: core
severity: minor
source: rung 3 (polls) task 11 — CMakeLists.txt completing the buildable rung skeleton
disposition: fixed
test: spec-cited (repro below is a per-translation-unit `-Werror` check against the real compile commands from `build/clang-coverage`)
---

`examples/common/testkit/backend_rig.hpp`'s `BackendRig` constructor switches
exhaustively over `enum class Mode { Local, LocalSingleThread, Socket }`
(lines 140, 195-227) with no `default:` label. `-Wswitch-default` (part of
`-Weverything`, which `apply_warnings()` always turns on for every
`ladder_<rung>_tests` target) fires on any `switch` lacking a `default:`
label regardless of enum exhaustiveness — distinct from `-Wswitch-enum`,
which checks enumerator coverage. The moment `-Werror` is added (i.e.
`MORPH_ENABLE_STRICT_COMPILATION=ON`), this becomes a hard error in every
rung's test binary that includes `backend_rig.hpp` — which is effectively
all of them, since `morph_ladder_testkit` is the common base every rung's
`tests/*.cpp` links against.

## Repro

```
python3 - <<'EOF'
import json, re, subprocess
data = json.load(open('build/clang-coverage/compile_commands.json'))
e = next(x for x in data if x['file'].endswith('examples/polls/tests/test_poll_model.cpp'))
cmd = e['command'].replace(' -c ', ' ').replace(
    '-o ', '-Werror -Wno-thread-safety-negative -Wno-poison-system-directories -fsyntax-only -o ', 1)
# Finding 028's own workaround: remap Lightweight/unixodbc's plain -I to
# -isystem so their own (unrelated, already-filed) warnings don't hit
# -Werror first and mask this finding behind clang's default -ferror-limit=20.
cmd = re.sub(r'-I(\S*(?:lightweight-src|unixodbc)\S*)', r'-isystem \1', cmd)
print(subprocess.run(cmd, shell=True, cwd=e['directory'], capture_output=True, text=True).stderr)
EOF
```

(Confirmed by the review of the task that filed this finding: running the script
*without* the `-isystem` remap does not reach `backend_rig.hpp:195` at all —
clang's default `-ferror-limit=20` exhausts itself on unrelated finding-028-class
errors in Lightweight's own headers first. The remap above is required for this
repro to be self-contained.)

```
examples/common/testkit/backend_rig.hpp:195:9: error: 'switch' missing
  'default' label [-Werror,-Wswitch-default]
        switch (mode) {
        ^
```

Confirmed on **both** `polls` (rung 3, this task, via `test_poll_model.cpp`)
and `bookmarks` (rung 2, via `test_bookmark_model.cpp`) with the identical
per-translation-unit check — not new, not rung-3-specific, and present since
`backend_rig.hpp` was authored (rung-0 build wiring). The normal
`build/clang-coverage` tree (`MORPH_ENABLE_STRICT_COMPILATION=OFF`) never
surfaces it, which is why no earlier task's real build hit it — same root
cause pattern as findings 028/029.

## What should happen instead

Add a `default:` case to the `switch (mode)` in `BackendRig`'s constructor
(`examples/common/testkit/backend_rig.hpp:195`) — e.g. an
`std::unreachable()`/`assert(false)` default, since the switch is already
meant to be exhaustive over `Mode`'s three enumerators. A one-file,
shared-testkit change; not a rung's file to make unilaterally (every rung's
`ladder_<rung>_tests` links `morph_ladder_testkit`).

## Consequence for rung 3 while this is open

Task 11's own verification (this task) found zero warnings in `polls`'s own
code (`src/`, `include/polls/`, `tests/`) under `-Weverything` via the normal
`cmake --build` (which already applies `-Weverything` without `-Werror` to
`ladder_polls_tests`), and zero designated-field-initializer issues (unlike
rung 2's own task 13, which fixed 43). A *fully* clean
`-DMORPH_ENABLE_STRICT_COMPILATION=ON` build of `ladder_polls_tests` cannot
be reached end-to-end via the normal `cmake --build` flow until this,
finding 028, and finding 029 are all fixed — verification was done
per-translation-unit against the real compile commands with
`-Wno-thread-safety-negative` (finding 029) and Lightweight/unixodbc include
dirs remapped to `-isystem` (finding 028's workaround) added, isolating the
check to code this task actually owns.
