## Lane W: verify the batch I fold and make every CI check pass

Worktree `/home/yaraslau/repo/morph-lims-e2`, branch `lg-fold-i`. It is lane I-S's `lg-is` (all of batch I: lanes I-Q,
I-V and I-S) with lane T's seven clang-tidy commits cherry-picked on top. Three conflicts were resolved by hand
(`include/morph/ui/host.hpp`: the new `headJoined`/`leadPlacement`/`headSurface` members written with `remembered`;
`include/morph/ui/theme.hpp`: `kKeywords` with three designated-initializer entries;
`examples/lims_gallery/tests/test_audit.cpp`). Your job: make this branch pass every check PR #1045's CI runs, fixing
what fails, without changing behaviour except for the two warnings below.

Scratchpad `S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad`. Scratch
files are named `$S/lg-w-*`.

### Checks, in this order

1. **Build** `build/linux-everything` (configure it first if it is stale or missing; the preset is
   `linux-everything`). No warnings.
2. **Full `ctest -j10`.** All pass. (Running a test binary directly shows two expected failures in `morph_tests`; ctest
   is the measure.)
3. **GCC `-Wshadow`**: `g++ -std=c++23 -fsyntax-only -Wshadow -Werror @$S/lg-ci5-inc.rsp $S/lg-ci5-all.cpp` from the
   worktree root (it includes every ui header; check the rsp's include paths point at this worktree, and copy them to
   `lg-w-*` files if they don't). Exit 0. Do not add `-Wall -Wextra`.
4. **Qt 6.8.1** (CI's floor): configure your own build dir `build/lg-qt681-w` with the Qt prefix used by
   `/home/yaraslau/repo/morph-lims-gallery/build/lg-qt681-is` (read its `CMakeCache.txt`), build `all_qmllint` and
   `morph_qt_quick_tests`. qmllint must print **no warnings** — lane I-S saw
   `MorphWidget.qml:34 Member "childBleedInset" not found on type QQuickItem`; fix it (type the parent properly or read
   the property safely). Run the Qt Quick tests with `QT_QPA_PLATFORM=offscreen` (without it 11 size tests fail
   spuriously): all pass, and grep the output for `Binding loop` — none.
5. **clang-tidy 22.1.8 over the whole branch diff** (the CI job `clang-tidy-diff`):
   - binary `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy`;
     `clang-tidy-diff.py` in `/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/bin/` (needs `PYTHONPATH`; see
     the wrapper `$S/lg-tidy-run.sh` and copy it as `$S/lg-w-tidy-run.sh` with paths adapted).
   - `git diff -U0 $(git merge-base origin/master HEAD) > $S/lg-w-changed.diff`, then
     `-path build/linux-everything -p1 -j10 -extra-arg=-std=c++23 -extra-arg=-Wno-missing-include-dirs -extra-arg=-Wno-pragma-once-outside-header -quiet`.
   - Zero findings with a file:line. Known so far: `document.hpp:4122` (container-contains), `author.hpp:4088`
     (use-anyofallof), and possibly `display_widgets.cpp`, `fields.cpp`, `samples.cpp`. Errors without a file:line
     (GCC-only warning options) and `glaze/glaze.hpp not found` from `forms/engine/detail/text.hpp` are local noise.
   - Control: introduce `int x = 0; (void)x;` style short-name variable in a changed `.cpp`, confirm the run reports
     it, then remove it.
   - Follow the conventions lane T used (see `git log -7` subjects and its NOLINT reasons). Never touch `.clang-tidy`.
6. **clang-format 22** (`$S/lg-cf22/bin/clang-format`): no diff on any file the branch changes
   (`git diff --name-only $(git merge-base origin/master HEAD)` filtered to `.cpp/.hpp/.h`).
7. **Coverage allowlist**: for every entry of `scripts/branch_partial_allowlist.json`, `resolve_allowlist_source_line`
   from `scripts/check_branch_coverage.py` must resolve. Batch I edited files above allowlisted lines; update the hints
   (lane T's commit "Update the document.hpp hints in the branch-coverage allowlist" shows how).
8. **Doxygen**: every public symbol batch I added in `include/morph` has complete `@brief/@param/@tparam/@return`
   (WARN_AS_ERROR in CI). Run the doc target as AGENTS.md says if doxygen is installed; otherwise grep the new
   declarations by hand.

### Two runtime warnings to fix

Every gallery run prints:
- `theme: surface darkHead: color.track: color.text is 1.21:1 on it, below 4.5:1` and `color.muted is 1.38:1`
  (console's `darkHead` surface in the gallery theme bundle `examples/lims_gallery/.../theme/index.json`): give the
  surface a `track` colour that passes, as lane I-S did for `controlSurface`/`controlBorder`.
- `icon nav.back is not in the theme's icon set` (and `nav.forward`), on the pager screens: the pagers draw I-Q's
  chevrons, so either the theme should name them (`Theme.iconGlyphs` / the icon set) or the warning is wrong for a
  name the chevron draws. Read how `MorphButton`/`MorphChevron` resolve the name and fix it at the right layer; a
  framework fix needs a test with a control.

Rerun a grab after fixing, and confirm the warnings are gone:
`QT_QPA_PLATFORM=offscreen build/linux-everything/examples/client/morph_examples_client --app lims-gallery --ui=qt --screen lims-gallery.audit --grab-size 1000x830 --grab $S/lg-w-audit.png`
(also `lims-gallery.console`).

### Rules

- Implementation agent: no Agent/Workflow, /code-review, /simplify, `run_in_background`, `&`, `nohup`, `pkill -f`,
  Monitor, or `git stash` (the stash stack is shared across worktrees). Wrap every build, test and clang-tidy run in
  `flock $S/morph-build.lock`, at most `-j10`.
- Comments state what the code does now; no history, no issue numbers (AGENTS.md).
- Commit fixes grouped by kind (tidy, Qt 6.8 lint, allowlist, warnings), messages saying what changed, ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
  ```
  Do not push, rebase, or rewrite existing commits.
- Report each check's result as measured (with counts), the controls you ran, the commits, and anything you could not
  run.
