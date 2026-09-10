# Minimal reproduction: three Mull 0.34.0 mutators report survivors that kill

Self-contained, ~18 lines of C++, no morph code. Reproduces the defect behind
[morph#510](https://github.com/LASTRADA-Software/morph/issues/510), and is
shaped to be handed upstream to `mull-project/mull` as-is.

## What it shows

`perm.cpp` has five mutable sites. Each mutation, if applied, makes `main`
return non-zero — so every one of them *should* be reported killed. Mull kills
two and reports the other three as survived:

| mutator | site | mutation | should be | Mull reports |
|---|---|---|---|---|
| `cxx_replace_scalar_call` | `k == "register"` | result → 42 | killed | **survived** |
| `cxx_replace_scalar_call` | `overLimit(5)` | result → 42 | killed | **survived** |
| `cxx_init_const` | `bool flag = false` | → truthy | killed | **survived** |
| `cxx_assign_const` | `assigned = false` | → truthy | killed | **survived** |
| `cxx_gt_to_ge` | `n > 5` | `>` → `>=` | killed | killed ✓ |
| `cxx_add_to_sub` | `a + b` | `+` → `-` | killed | killed ✓ |

`cxx_gt_to_ge` and `cxx_add_to_sub` are the **controls**, and they matter: they
run in the same binary, in the same `mull-runner` invocation, and they are
detected. So the report is not explained by a broken harness, by the runner
being unable to observe a failing exit code, or by mutant selection failing
wholesale.

One further site is a genuine equivalent and is correctly reported as survived —
`bool assigned = true` mutated to another truthy value, then unconditionally
overwritten on the next line. Included on purpose: it shows the defect is
per-mutant rather than "this mutator never works", and it is why an earlier
version of morph#510 withdrew its family-level percentage.

## The mutation *is* generated

Not a codegen no-op like the `cxx_remove_void_call` defect in morph#434.
Disassembling the instrumented object shows the mutated function body differs
from its `mull__..._original` twin, and `.mull_mutants` carries the identifier.
Whatever goes wrong is at or after mutant activation, not at IR generation —
which makes this a *different* upstream bug from #434, not another instance.

## Running it

Needs Mull 0.34.0 for your clang major (this was measured on the LLVM-22 build,
clang 22.1.8):

```sh
clang++ -std=c++23 -g -grecord-command-line -O0 \
    -fpass-plugin=/path/to/mull-ir-frontend-22 perm.cpp -o perm
./perm; echo "baseline exit: $?"        # 0 -- the "test" passes unmutated
MULL_CONFIG=$PWD/mull.yml mull-runner-22 --reporters IDE --report-dir . --report-name perm ./perm
cat perm.txt
```

`-grecord-command-line` is required; without it the frontend warns "Mull cannot
find compiler flags" and emits no `.mull_mutants` section at all.

Observed: `Surviving mutants: 5` of 8, `Mutation score: 37%`.

## Why this directory opts out of the repo's own lint and formatting

`.clang-format` (`DisableFormat: true`) and `.clang-tidy` (`Checks: -*`) apply
here and nowhere else. This is an artefact meant to be handed to someone else
unchanged, so house style would make it diverge from what was measured -- and
two of the rules would actively break it: `misc-const-correctness` wants `const`
on the very variable whose initialiser `cxx_init_const` mutates, which is the
behaviour under test.
