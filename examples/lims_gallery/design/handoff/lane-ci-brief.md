## Lane CI: make PR #1045's CI green

PR #1045 (`feat/lims-gallery`, head `300cee8c1`) failed 8 jobs in CI run 41146136081 (and WASM in run 41146136019):
Application ladder, Build & deploy WASM demo, Linux / Qt6 WebSockets, Linux / clang-coverage, Linux / gcc-release,
Windows / cl-debug, Windows / clangcl-debug, clang-tidy-diff. Passing: clang-asan, clang-release, clang-tsan,
clang-debug, clang-ubsan, ladder ASan+UBSan, clang-format, docs, find_package.

Worktree `/home/yaraslau/repo/morph-lims-e1` (clean, on `lg-fold-j` at `300cee8c1`): `git checkout -b lg-ci7 300cee8c1`.
Read `AGENTS.md`. Scratchpad `S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`;
scratch files `$S/lg-ci7-*`.

### Step 1: diagnose every failure from its log

`gh run view 41146136081 --job <id> --log-failed` (job ids: `gh run view 41146136081 --json jobs`), WASM:
`gh run view 41146136019 --log-failed`. For each job write down the first real error (file:line, message) in
`$S/lg-ci7-diagnosis.md`. Classify: a code defect (fix it), a CI-runner/infrastructure failure (runner shutdown, exit
143, OOM, network — note the evidence and do not change code for it), or a flaky test (evidence: passes locally N
times, failure text). Known history on this branch: WASM ran out of memory compiling the gallery screens (a job pool of
2 for `lims_gallery_screens` was added); Qt6 WebSockets and WASM were once killed by runner shutdown (exit 143).

### Step 2: fix, reproducing locally where you can

- **clang-tidy-diff**: the CI log lists each finding; fix them all. Locally: clang-tidy 22.1.8
  `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy`, with `clang-tidy-diff.py`
  from `.../22.1.8/bin/` (needs `PYTHONPATH`; wrapper `$S/lg-tidy-run.sh`, copy as `$S/lg-ci7-tidy-run.sh` with paths
  adapted). `git diff -U0 $(git merge-base origin/master HEAD) | … -path build/linux-everything -p1 -j10 -extra-arg=-std=c++23 -extra-arg=-Wno-missing-include-dirs -extra-arg=-Wno-pragma-once-outside-header`.
  The local run may differ from CI (CI's compile database adds headers): trust the CI log's list. Do not touch
  `.clang-tidy`.
- **gcc-release**: CI is GCC 15.2 at `-O3` with `-Werror` (local GCC is 16 and misses some warnings, e.g.
  `-Wstrict-overflow`). If needed, reproduce in docker `fastcache-fleet-toolchain` (see the memory note in the repo's
  history, or just fix from the log's exact warning).
- **Windows cl-debug / clangcl-debug**: MSVC/clang-cl errors from the log (typical: dependent default template args
  need `typename`; `qualified-auto` fix-its with `auto*` on iterators; missing includes; narrowing). Fix from the log.
- **clang-coverage**: usually the branch-coverage allowlist (`scripts/branch_partial_allowlist.json`, resolved by
  `scripts/check_branch_coverage.py`) or a coverage threshold; read the log for which. Verify every allowlist entry
  resolves with `resolve_allowlist_source_line`.
- **Application ladder, Qt6 WebSockets**: read which test failed; reproduce it locally (`ctest -R <name>` repeated) and
  fix a real defect, or record evidence of flakiness/infrastructure.
- **WASM**: read the log; memory, a compile error in WASM-only code, or infrastructure.

### Step 3: verify

Build `build/linux-everything` with no warnings; full `ctest -j10`; clang-format 22 (`$S/lg-cf22/bin/clang-format`) on
changed files; GCC `-Wshadow` check as before if headers changed
(`g++ -std=c++23 -fsyntax-only -Wshadow -Werror @$S/lg-v-inc.rsp $S/lg-ci5-all.cpp`, rsp paths pointing at this
worktree). Commit fixes grouped by job, messages saying what changed, ending with:
```
Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
```
Do not push. The coordinator pushes and reruns CI.

Rules: no Agent/Workflow, /code-review, /simplify, `run_in_background`, `&`, `nohup`, `pkill -f`, Monitor, or
`git stash`. Wrap every build, test and clang-tidy run in `flock $S/morph-build.lock`, at most `-j10`. Comments state
what the code does now; no history, no issue numbers.

Report: per job, the cause (with the log line), what you changed or why you did not, and what you verified locally
vs. could not.
