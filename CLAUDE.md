# CLAUDE.md

## Feature documentation (`docs/superpowers/`)

One file per feature, compressed reference documentation:

- Keep each file **under 500 lines**; be concise, not exhaustive.
- Describe **only the existing behavior, in present tense**. Never document
  the previous state, migrations, diffs, or task checklists — these files
  are not changelogs or implementation plans (git history covers that).
- When a feature changes, rewrite the affected sections to state the new
  current behavior.

## CI notes

- The Docs workflow runs Doxygen with `WARN_AS_ERROR = FAIL_ON_WARNINGS`:
  every public symbol needs complete `@param`/`@tparam`/`@return` docs or
  the build fails. Reproduce locally with
  `cmake -S . -B build -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF`
  then `cmake --build build --target doc`.
