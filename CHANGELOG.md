# Changelog

Notable changes to morph, in [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
format. morph is pre-1.0 and has no tagged releases yet: `master` is the only
supported line, this file tracks notable changes going forward, and history
predating it lives in `git log`. From 1.0 on, versioning follows the policy
published in `docs/spec/VERSIONING.md` (SemVer for the public, non-`detail`
API surface).

## [Unreleased]

### Added

- Root project docs: `CHANGELOG.md`, `SECURITY.md`, `CONTRIBUTING.md`.
- Planned specs: connection-scoped model cleanup
  (`docs/planned/connection_scoped_cleanup.md`), graceful shutdown & drain
  (`docs/planned/graceful_shutdown.md`), journal format versioning & retention
  (`docs/planned/journal_evolution.md`), GUI localisation
  (`docs/planned/gui_i18n.md`), with matching roadmap entries in
  `docs/todo.md` (A7, C5, B4, E-G10).
- GUI program: presentation rules (`visibleWhen`/`readonlyWhen`) in
  `docs/planned/gui_cross_field_rules.md`; accessibility assertions in
  `docs/planned/gui_renderer_toolkit.md`'s conformance kit; a
  banned-terminology check in `docs/planned/drift_guard.md`'s prose lint.
- API stability & versioning policy (`docs/spec/VERSIONING.md`, folded in
  from `docs/planned/api_stability.md`): the semantic-versioning rules, the
  stable (non-`detail`) surface definition, and the deprecation-window
  discipline morph commits to starting at 1.0. Backed by two new mechanical
  checks: the `include/morph/version.hpp` version constants (cross-checked
  against `CMakeLists.txt`'s `project(VERSION ...)` by
  `tests/test_version.cpp`), and the `deprecation-lint` CI job
  (`scripts/check_deprecated_markers.sh`) enforcing that every
  `[[deprecated("...")]]` marker names a replacement and a target removal
  version.

### Fixed

- Stale pre-JSON "5-part"/"6-part protocol" wording in `docs/ARCHITECTURE.md`
  and a test comment — the wire has been a JSON `Envelope` since it superseded
  the pipe-delimited protocol.
