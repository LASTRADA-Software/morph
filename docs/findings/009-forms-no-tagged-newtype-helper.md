---
id: 009
title: No Tagged<T, "Name"> opaque-newtype helper for protocol scalars
subsystem: forms
severity: major
source: examples/IMPLEMENTATION.md rule 3, protocol scalars row
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/49
---

No `Tagged<T, "Name">` helper exists under `include/morph/forms/` or `include/morph/util/`. Per IMPLEMENTATION.md rule 3, every protocol scalar (pagination cursor, event id, job id, token) should be an opaque newtype that joins glaze and the forms palette with `hasValue()` capability, serialising as its underlying scalar. Without a reusable helper, each rung hand-rolls wrapper sets — a duplication the promotion rule forbids after the third rung.

**What should happen:** a single `Tagged<T, "Name">` helper providing:
- Transparent serialization (via glaze `write_json_schema` integration)
- `hasValue()` support for the forms palette
- Type-safe identity preventing category errors (confusing `UserId` and `AccountId`)

This is a framework day-one finding, not a per-rung task.
