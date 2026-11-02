# Lane GC: the render-smoke segfault that batch Q's run-face commit triggers (#1083)

Rules: $S/lg-r-common.md (S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad) binds: no Agent/Workflow, no run_in_background / `&` / nohup / pkill -f / Monitor / git stash; builds under `flock $S/morph-build.lock` -j10; scratch files `lg-gc-*`; commit trailers Opus 5.5 + session; do not push.
Worktree `/home/yaraslau/repo/morph-lims-e1`, branch `lg-fold-q` (head 58eb4158a, verified by V8 except for this crash). Commit the fix there.

Known (read #1083 and its comments: `gh issue view 1083 --comments`; V8's logs `$S/lg-v8-*`; lane INV's `$S/lg-inv-*`, backtraces `$S/lg-inv-bt.txt`):
- `lims_gallery_render_smoke` segfaults in Qt 6.12.0's incremental QML GC (`QV4::GCStateMachine::transition`, via `Qt.font()` → `QQuickFontValueType::create` or the GC timer). Under load or with a reduced font set (Liberation + D050000L via QT_QPA_FONTDIR) it reproduces: 5/32 at head, 0/32 on `4c0e46a76`. First bad commit `21886b50f` (runs measured in their drawn faces: `runFontOf`, FontMetrics for strong/mono runs in MorphText.qml). `QV4_GC_TIMELIMIT=0` removes it. Removing `Qt.font` from `runFontOf` did not.
Goal: find the actual defect and fix it in morph if it is morph's; if it is Qt's, find the smallest morph change that stops triggering it, and record the Qt bug with a minimal plain-QML reproducer.
Suggested approach (decide yourself):
- Symbols: Arch's debuginfod (`DEBUGINFOD_URLS=https://debuginfod.archlinux.org gdb ...`) gives Qt 6.12 symbols for the cores (`coredumpctl`), so you can see which object is freed.
- Diff `47622d32e..21886b50f` in QML: look for a JS object/value (font, FontMetrics, a `var` holding a QML value type, an array, a function returning a value type) kept alive across a binding re-evaluation, a C++ object (`TextMeasure`) exposed to QML via a `var` without QML ownership/parent (JS GC may delete or touch it — check `QQmlEngine::setObjectOwnership` / `QJSEngine::CppOwnership`), or a property written from `ItemBase::write` during a GC step.
- `QV4_MM_AGGRESSIVE_GC=1` on a single small QML scene, `QML_DISK_CACHE=0`, an ASan build of the smoke (`-fsanitize=address` on the clang preset, Qt itself uninstrumented) to see a use-after-free in morph code.
- Bisect inside the commit by reverting hunks.
Verify the fix with the reproducer: ≥64 runs under the reduced font set and ≥3 full `ctest -j10` with 0 crashes, and show the same harness still crashes with the fix reverted. Then full ctest, qmllint (Qt 6.12 and the 6.8.1 preset as V8 ran it), clang-format/tidy on changed files. Report root cause (measured vs inferred), the fix, the reproducer numbers, and text to add to #1083 (or a Qt bug reproducer).
