# Contributing to morph

## Toolchain

morph is a header-only C++23 library. You need a C++23 compiler, CMake with
Ninja, and the dependencies declared in `vcpkg.json` (Glaze, Catch2; Qt 6 only
when building the optional Qt integration, `-DMORPH_BUILD_QT=ON`). CMake
presets are provided — `cmake --list-presets` shows the configured matrix; the
README documents the full set of build options
(`MORPH_BUILD_TESTS`, `MORPH_BUILD_EXAMPLES`, `MORPH_BUILD_QT`,
`MORPH_BUILD_FORMS_QML`, …).

A plain configure/build/test loop:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## The spec-first workflow (please read this one section)

`docs/spec/` is the **authoritative design reference** — one file per public
type or subsystem. The rules, from `CLAUDE.md`:

- **Before changing any public type or subsystem, read its spec.** The spec
  carries the invariants and reasoning the code alone does not.
- **If a change invalidates any part of a spec, update the spec in the same
  change** — never the other way around. Where `docs/ARCHITECTURE.md` (the
  cross-cutting map) and a spec disagree, the spec wins.
- **Planned work** lives in `docs/planned/`, one spec per feature, each with a
  `Status: planned — not yet implemented` banner; `docs/todo.md` is the
  prioritised index. When you implement one: build against the spec, then flip
  its banner and rewrite it to present tense, and update `ARCHITECTURE.md`.
- **Feature docs** (`docs/superpowers/`) are compressed reference docs — one
  file per feature, under 500 lines, present tense only, no changelogs or
  migration notes (git history covers that).

## Quality gates

- **Tests:** behavior changes come with Catch2 tests under `tests/`.
- **Doxygen is strict:** the Docs CI job runs with
  `WARN_AS_ERROR = FAIL_ON_WARNINGS` — every public symbol needs complete
  `@param`/`@tparam`/`@return` docs. Reproduce locally with
  `cmake -S . -B build -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON
  -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF` and
  `cmake --build build --target doc`.
- **Formatting/linting:** `.clang-format` and `.clang-tidy` govern C++;
  markdown follows `.markdownlint.yaml` (119-column limit; code blocks and
  tables exempt). `pre-commit run --all-files` runs the configured hooks.
- **Keep mechanical facts honest:** if you change a pinned constant, enum
  cardinality, or canonical error string, update the spec prose that cites it
  in the same commit (`docs/planned/drift_guard.md` describes the CI guard
  this feeds).

## Security

Suspected vulnerabilities go through private reporting — see
[SECURITY.md](SECURITY.md), not the public issue tracker.

## License

morph is Apache-2.0 (see [LICENSE](LICENSE)); contributions are accepted under
the same license.
