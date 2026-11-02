## Lane V9: verify the batch R fold and make every CI check pass

Worktree `/home/yaraslau/repo/morph-lims-e1`, branch `lg-fold-r` (head `0a9a0bf9c`). It is PR #1045's head `ed12be19a`
(batches A–Q, verified by V8, with the GC workaround for #1083 and a tidy fix) with batch R cherry-picked on top, no
conflicts: R-F's 7 commits (key-value captions on the value's first baseline; `actions.gap` and an in-place
`actionBar`; `HeadTrailEnd`; an unfocused single-line input that is cut ends in "…", Fixes #1082; `grid.minColumnWidth`;
lint; coverage tests and allowlist hints) and 6 gallery commits (R-S, R-S2, and a MorphText whole-pixel fix in
`src/qt_quick/qml/MorphText.qml` with its test). Design `$S/lg-r-design.md`, review `$S/lg-r-review.md`.
R-F built and fully tested each item commit on its own. R-F measured ui coverage at 92.03 % (4 arms over the floor).
R-F ran nothing on Qt 6.8.1. CI's clang-tidy-diff just failed `readability-container-contains` on `find(...) == npos`
in tests: run clang-tidy over the batch R diff on test files too, exactly as CI's clang-tidy-diff job does (read
.github/workflows for its invocation).
Extra checks for this batch:
- Every commit builds: without rewriting history, check out each of the 13 commits `ed12be19a..lg-fold-r` detached in
  turn, build `morph_tests morph_qt_quick_tests morph_tui_tests lims_gallery_tests lims_gallery_render_smoke`
  (under the lock), and report any that fail. Return to `lg-fold-r` afterwards. Do not rewrite history.
- Qt 6.8.1: the `[qt_quick]` suite and qmllint.
- Reduced font sets: the `[qt_quick]` suite under gsfonts only and under Liberation + D050000L (see `$S/lg-v8-*`,
  `$S/lg-ci9-*`); fix any batch R test that fails there for a font reason.
- The render smoke 32 times under the reduced font set (`$S/lg-gc-stress.sh`): 0 crashes expected with the GC
  workaround; report the count.
Commit trailers for this session:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and
`Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn`.
Make this branch pass every check PR #1045's CI runs, fixing what fails,
without changing behaviour. The batch R briefs are `$S/lg-r-common.md`, `$S/lg-rf-brief.md`, `$S/lg-rs-brief.md`, `$S/lg-rs2-brief.md`; V5's brief `$S/lg-v5-brief.md` lists how it ran each check, and V6's scripts are `$S/lg-v6-*`.

PR #1045's CI failed earlier on things local checks miss; guard against them too: an `auto const*` bound to a
`std::ranges::find` result over a `std::array` (fine on libstdc++, breaks libc++/MSVC: grep the branch diff for it);
a range-for over a member of a temporary (`for (… : f(x.json().data))` where `json()` returns by value: emscripten's
clang `-Wdangling-gsl`); `for (T const& x : {…})` over an initializer list of a different type
(`-Werror=range-loop-construct`); GCC 15 `-O3` `array-bounds` false positives on braced `std::vector` arguments.

Scratchpad `S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`. Scratch
files are named `$S/lg-v9-*`.

### Checks, in this order

1. **Build** `build/linux-everything` (configure it first if it is stale or missing; the preset is
   `linux-everything`). No warnings.
2. **Full `ctest -j10`.** All pass. (Running a test binary directly shows two expected failures in `morph_tests`; ctest
   is the measure.)
3. **GCC `-Wshadow`**: `g++ -std=c++23 -fsyntax-only -Wshadow -Werror @$S/lg-ci5-inc.rsp $S/lg-ci5-all.cpp` from the
   worktree root (it includes every ui header; check the rsp's include paths point at this worktree, and copy them to
   `lg-v9-*` files if they don't). Exit 0. Do not add `-Wall -Wextra`.
4. **Qt 6.8.1** (CI's floor): configure your own build dir `build/lg-qt681-jb` (already configured in this worktree; reconfigure if stale), build `all_qmllint` and
   `morph_qt_quick_tests`. qmllint must print **no warnings** (Info lines are fine). Run the Qt Quick tests with `QT_QPA_PLATFORM=offscreen` (without it 11 size tests fail
   spuriously): all pass, and grep the output for `Binding loop` — none.
5. **clang-tidy 22.1.8 over the whole branch diff** (the CI job `clang-tidy-diff`):
   - binary `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy`;
     `clang-tidy-diff.py` in `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/bin/` (needs `PYTHONPATH`; see
     the wrapper `$S/lg-tidy-run.sh` and copy it as `$S/lg-v9-tidy-run.sh` with paths adapted).
   - `git diff -U0 $(git merge-base origin/master HEAD) > $S/lg-v9-changed.diff`, then
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
