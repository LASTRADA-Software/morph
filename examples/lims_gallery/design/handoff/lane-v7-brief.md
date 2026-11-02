## Lane V7: verify the batch P fold and make every CI check pass

Worktree `/home/yaraslau/repo/morph-lims-e1`, branch `lg-fold-p` (head `e8b601a75`). It is PR #1045's head `ad3cc392d`
(batches A–O, verified by lane V6, CI green) with batch P on top: P-F's 8 framework commits (equal grid columns on Qt
Quick with the terminal's natural width, Fixes #1077; `captionWidth` on keyValue/form/collection/wizard;
`control.touchTextSize` and `Theme.controlFontOf`; `button.smallHeight`/`smallTouchHeight` and a round button never
taller than wide; the `caption` text role; stacked key-value captions in the caption font; conformance failure-arm
tests; allowlist hints) and 18 gallery commits (P-S, P-S2). Design: `$S/lg-p-design.md`, review `$S/lg-p-review.md`.
P-F split one working tree into commits by hunk, so its intermediate commits were never built: the per-commit build
check below matters. P-F measured ui branch coverage at 91.41 % on a narrower configure (examples and ladder off) than
V6's 92.02 %; measure it exactly as V6 did (clang-coverage preset, `PATH=/usr/lib/llvm21/bin:$PATH` for
scripts/coverage.sh, `LLVM_PROFILE_FILE=<build>/prof/%m.profraw` under ctest -j10) and add tests for uncovered batch P
arms until it is at least 92.1 %. P-F ran no Qt 6.8.1 run; `test_ui_document_rules` ("a state declared any…") failed
once for P-F and passed after: run it 20 times and report.
Extra checks for this batch:
- Every commit builds: without rewriting history, check out each of the 26 commits `ad3cc392d..lg-fold-p` detached
  in turn, build only `morph_tests morph_qt_quick_tests morph_tui_tests lims_gallery_tests` (under the lock), and report
  any that fail. Return to `lg-fold-p` afterwards. Do not rewrite history; report a failing commit and the fix you'd make.
- Qt 6.8.1: the `[qt_quick]` suite and qmllint, and the new grid conformance case; #1077 says "if the new case passes
  on 6.8.1 without the fix, the defect is 6.12-specific": run that control once on 6.8.1 (MorphGrid.qml from
  `ad3cc392d`) and report.
- Grep every test, smoke and grab log for `Binding loop`.
Commit trailers for this session:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
`Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn`.
Make this branch pass every check PR #1045's CI runs, fixing what fails,
without changing behaviour. The batch P briefs are `$S/lg-p-common.md`, `$S/lg-pf-brief.md`, `$S/lg-ps-brief.md`, `$S/lg-ps2-brief.md`; V5's brief `$S/lg-v5-brief.md` lists how it ran each check, and V6's scripts are `$S/lg-v6-*`.

PR #1045's CI failed earlier on things local checks miss; guard against them too: an `auto const*` bound to a
`std::ranges::find` result over a `std::array` (fine on libstdc++, breaks libc++/MSVC: grep the branch diff for it);
a range-for over a member of a temporary (`for (… : f(x.json().data))` where `json()` returns by value: emscripten's
clang `-Wdangling-gsl`); `for (T const& x : {…})` over an initializer list of a different type
(`-Werror=range-loop-construct`); GCC 15 `-O3` `array-bounds` false positives on braced `std::vector` arguments.

Scratchpad `S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`. Scratch
files are named `$S/lg-v7-*`.

### Checks, in this order

1. **Build** `build/linux-everything` (configure it first if it is stale or missing; the preset is
   `linux-everything`). No warnings.
2. **Full `ctest -j10`.** All pass. (Running a test binary directly shows two expected failures in `morph_tests`; ctest
   is the measure.)
3. **GCC `-Wshadow`**: `g++ -std=c++23 -fsyntax-only -Wshadow -Werror @$S/lg-ci5-inc.rsp $S/lg-ci5-all.cpp` from the
   worktree root (it includes every ui header; check the rsp's include paths point at this worktree, and copy them to
   `lg-v7-*` files if they don't). Exit 0. Do not add `-Wall -Wextra`.
4. **Qt 6.8.1** (CI's floor): configure your own build dir `build/lg-qt681-jb` (already configured in this worktree; reconfigure if stale), build `all_qmllint` and
   `morph_qt_quick_tests`. qmllint must print **no warnings** (Info lines are fine). Run the Qt Quick tests with `QT_QPA_PLATFORM=offscreen` (without it 11 size tests fail
   spuriously): all pass, and grep the output for `Binding loop` — none.
5. **clang-tidy 22.1.8 over the whole branch diff** (the CI job `clang-tidy-diff`):
   - binary `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy`;
     `clang-tidy-diff.py` in `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/bin/` (needs `PYTHONPATH`; see
     the wrapper `$S/lg-tidy-run.sh` and copy it as `$S/lg-v7-tidy-run.sh` with paths adapted).
   - `git diff -U0 $(git merge-base origin/master HEAD) > $S/lg-v7-changed.diff`, then
     `-path build/linux-everything -p1 -j10 -extra-arg=-std=c++23 -extra-arg=-Wno-missing-include-dirs -extra-arg=-Wno-pragma-once-outside-header -quiet`.
   - Zero findings with a file:line. Errors without a file:line
     (GCC-only warning options) and `glaze/glaze.hpp not found` from `forms/engine/detail/text.hpp` are local noise.
   - Control: introduce `int x = 0; (void)x;` style short-name variable in a changed `.cpp`, confirm the run reports
     it, then remove it.
   - Follow the conventions lanes T, V3 and V4 used (see the subjects of the commits named "Clear clang-tidy findings…" and their NOLINT reasons). Never touch `.clang-tidy`.
6. **clang-format 22** (`$S/lg-cf22/bin/clang-format`): no diff on any file the branch changes
   (`git diff --name-only $(git merge-base origin/master HEAD)` filtered to `.cpp/.hpp/.h`).
7. **Coverage allowlist**: for every entry of `scripts/branch_partial_allowlist.json`, `resolve_allowlist_source_line`
   from `scripts/check_branch_coverage.py` must resolve. Update stale hints (the commits named "Update the document.hpp hints…" show how).
8. **Doxygen**: every public symbol batch N added in `include/morph` has complete `@brief/@param/@tparam/@return`
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
  see `git log -1 --format=%B 8b3478a6e`) and report the `include/morph/ui` figure; batch N's new branches (percent, splitter bounds, align, head bar) need tests if the figure is below 92%. It was 92.09% on the batch M fold.
