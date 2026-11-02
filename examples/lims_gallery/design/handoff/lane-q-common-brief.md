# Batch Q — common lane rules

Base: batch P (`lg-fold-p`, being verified by lane V7, which may add lint-only fix commits; the screen lane works from `lg-ps` e8b601a75, the same behaviour).
Batch Q design: $S/lg-q-design.md (under review; $S/lg-q-review.md when done). Batch P design: $S/lg-p-design.md.
Scratchpad S=/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad ; your scratch files are `lg-<lane>-*`.
Findings behind this batch: $S/lg-pr-findings-A.md, $S/lg-pr-findings-B.md; after-P grabs $S/lg-cmp/app-p/ (1000x830, 1215x830, 390x844, 1920x1080).
Mockups: examples/lims_gallery/design/mockups/*.dc.html, renders $S/lg-cmp/mock/*.png; the 1215x830 grab gives a ~1000 px page like the mockups; at 1000x830 (785 px page) only breakage counts.

- Forbidden: Agent/Workflow, /code-review, /simplify, run_in_background, `&`, nohup, pkill -f, Monitor, git stash.
- Wrap every build/test/tidy in `flock $S/morph-build.lock`, -j10 max; let CompileCache.cmake pick fastcache-cc.
- AGENTS.md: comments state current behaviour only, no issue numbers or hashes; public API needs full Doxygen. Read the docs/spec page before changing a public type and update it (docs/spec/qt_quick/backend.md, docs/spec/ui/...).
- Prefer making ui/1 express the mockup over screen workarounds. A misconfiguration is reported, never silently ignored.
- Grab: `QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen build/linux-everything/examples/client/morph_examples_client --app lims-gallery --ui=qt --user "Dr. A. Meyer" --screen lims-gallery.<id> --grab-size WxH --grab $S/lg-<lane>-<id>-<w>.png 2>log` — Read the PNGs, check the logs are empty.
- CI pitfalls: no `auto const*` on std::ranges::find over std::array; no range-for over a member of a temporary (hold it in a local); no `for (T const& x : {..})` with mismatched types; no braced std::vector arguments in hot test code under GCC 15 -O3. clang-tidy 22.1.8 (/home/yaraslau/.local/share/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy, -p build/linux-everything) on changed files; clang-format 22 ($S/lg-cf22/bin/clang-format); edits above a line listed in the branch-coverage allowlist need the hints re-verified (scripts/check_branch_coverage.py). Add tests for new branches in include/morph/ui (coverage floor 92%).
- Verify: build/linux-everything build + ctest -j10 all green; tests must fail if the feature is removed (mutate once and say so).
- Commit per item as the current git user, trailers:
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BmVLNhUvbWZjv85Hx2saxn
- Do not push. Decide yourself; don't ask. Final report: commits, measured vs inferred, before/after grabs for visual items.
- Commit trailers must be exactly the two above (Opus 5.5), whatever model you are.
