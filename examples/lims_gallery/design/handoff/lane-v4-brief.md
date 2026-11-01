## Lane V4: verify the batch M fold and make every CI check pass

Worktree `/home/yaraslau/repo/morph-lims-e1`, branch `lg-fold-m` (head `a30f51c93`). It is PR #1045's head `24d939686`
(batches A–L, verified by lane V3) with batch M on top: M-F's 6 commits (`lg-mf`: a per-button caption weight by DTCG
name, the shadow's outer layers fading, `color.dialogRule`, `color.placeholder`, a table of optional colour flags,
allowlist hints) and M-S's 12 screen commits (`lg-ms`, only `examples/lims_gallery`). No conflicts. M-F ran full ctest
(5299/5299) before its last refactor; M-S's final ctest summary was cut off. Not yet run on the fold: everything.
M-F's commit `4bb796860` (dialogRule) was never built on its own; Verify the head only.
Commit trailers for this session:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
`Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn`.
Make this branch pass every check PR #1045's CI runs, fixing what fails,
without changing behaviour. The batch L/M briefs are `$S/lg-l-common.md`, `$S/lg-m-common.md`, `$S/lg-mf-brief.md`, `$S/lg-ms-brief.md`.

PR #1045's CI failed earlier on things local checks miss; guard against them too: an `auto const*` bound to a
`std::ranges::find` result over a `std::array` (fine on libstdc++, breaks libc++/MSVC: grep the branch diff for it);
a range-for over a member of a temporary (`for (… : f(x.json().data))` where `json()` returns by value: emscripten's
clang `-Wdangling-gsl`); `for (T const& x : {…})` over an initializer list of a different type
(`-Werror=range-loop-construct`); GCC 15 `-O3` `array-bounds` false positives on braced `std::vector` arguments.

Scratchpad `S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`. Scratch
files are named `$S/lg-v4-*`.

### Checks, in this order

1. **Build** `build/linux-everything` (configure it first if it is stale or missing; the preset is
   `linux-everything`). No warnings.
2. **Full `ctest -j10`.** All pass. (Running a test binary directly shows two expected failures in `morph_tests`; ctest
   is the measure.)
3. **GCC `-Wshadow`**: `g++ -std=c++23 -fsyntax-only -Wshadow -Werror @$S/lg-ci5-inc.rsp $S/lg-ci5-all.cpp` from the
   worktree root (it includes every ui header; check the rsp's include paths point at this worktree, and copy them to
   `lg-v4-*` files if they don't). Exit 0. Do not add `-Wall -Wextra`.
4. **Qt 6.8.1** (CI's floor): configure your own build dir `build/lg-qt681-jb` (already configured in this worktree; reconfigure if stale), build `all_qmllint` and
   `morph_qt_quick_tests`. qmllint must print **no warnings** (Info lines are fine). Run the Qt Quick tests with `QT_QPA_PLATFORM=offscreen` (without it 11 size tests fail
   spuriously): all pass, and grep the output for `Binding loop` — none.
5. **clang-tidy 22.1.8 over the whole branch diff** (the CI job `clang-tidy-diff`):
   - binary `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy`;
     `clang-tidy-diff.py` in `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/bin/` (needs `PYTHONPATH`; see
     the wrapper `$S/lg-tidy-run.sh` and copy it as `$S/lg-v4-tidy-run.sh` with paths adapted).
   - `git diff -U0 $(git merge-base origin/master HEAD) > $S/lg-v4-changed.diff`, then
     `-path build/linux-everything -p1 -j10 -extra-arg=-std=c++23 -extra-arg=-Wno-missing-include-dirs -extra-arg=-Wno-pragma-once-outside-header -quiet`.
   - Zero findings with a file:line. Errors without a file:line
     (GCC-only warning options) and `glaze/glaze.hpp not found` from `forms/engine/detail/text.hpp` are local noise.
   - Control: introduce `int x = 0; (void)x;` style short-name variable in a changed `.cpp`, confirm the run reports
     it, then remove it.
   - Follow the conventions lanes T and V3 used (see the subjects of the commits named "Clear clang-tidy findings…" and their NOLINT reasons). Never touch `.clang-tidy`.
6. **clang-format 22** (`$S/lg-cf22/bin/clang-format`): no diff on any file the branch changes
   (`git diff --name-only $(git merge-base origin/master HEAD)` filtered to `.cpp/.hpp/.h`).
7. **Coverage allowlist**: for every entry of `scripts/branch_partial_allowlist.json`, `resolve_allowlist_source_line`
   from `scripts/check_branch_coverage.py` must resolve. Update stale hints (the commits named "Update the document.hpp hints…" show how).
8. **Doxygen**: every public symbol batch M added in `include/morph` has complete `@brief/@param/@tparam/@return`
   (WARN_AS_ERROR in CI). Run the doc target as AGENTS.md says if doxygen is installed; otherwise grep the new
   declarations by hand.

9. **Render smokes of every example app** (bank, forms, gallery, ladder lims, ladder bookmarks, lims_gallery) pass, and
   none prints a `morph.layout` overflow warning at its default size.
10. **The TUI**: `morph_tui_tests` passes, and `lims-gallery` runs under `--ui=tui` for one screen without a crash (if the
    client has a non-interactive TUI smoke, use that).

### Rules

- Implementation agent: no Agent/Workflow, /code-review, /simplify, `run_in_background`, `&`, `nohup`, `pkill -f`,
  Monitor, or `git stash` (the stash stack is shared across worktrees). Wrap every build, test and clang-tidy run in
  `flock $S/morph-build.lock`, at most `-j10`.
- Comments state what the code does now; no history, no issue numbers (AGENTS.md).
- Commit fixes grouped by kind (build, tests, tidy, Qt 6.8 lint, allowlist, docs), messages saying what changed, ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
  ```
  Do not push, rebase, or rewrite existing commits.
- Report each check's result as measured (with counts), the controls you ran, the commits, and anything you could not
  run.

### Also

- **Branch coverage floor**: CI's clang-coverage job requires `include/morph/ui` branch coverage of at least 92%; it
  was 91.84% before the last CI fixes added tests. If you can, run the `clang-coverage` preset locally (lane CI did:
  see `git log -1 --format=%B 8b3478a6e`) and report the `include/morph/ui` figure; batch M's new branches (caption weight, dialogRule, placeholder) need tests if the figure is below 92%. Coverage was 92.08% on the batch L fold.
