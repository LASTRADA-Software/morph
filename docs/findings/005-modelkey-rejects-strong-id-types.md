---
id: 005
title: morph::model::ModelKey admits only integrals and std::string, so no model can be keyed on the strong id type IMPLEMENTATION.md rule 3 mandates
subsystem: core
severity: minor
source: lims rung 6, build order §2 (third rung to hit it — rule-of-three)
disposition: open
test: spec-cited (compile-error repro below)
---

`examples/IMPLEMENTATION.md` rule 3 requires entity identity in a DTO to be
"a per-entity strong id type (e.g. `struct PasteId`) exposing `hasValue()`".
`BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` (`include/morph/core/model_key.hpp`)
deduce the model's `PrimaryKey` as the *type* of the named action member and
route the value through `morph::model::keyToString<K>`, constrained by

```
template <typename K>
concept ModelKey =
    (std::integral<K> && !std::same_as<std::remove_cv_t<K>, bool>) ||
    std::same_as<std::remove_cv_t<K>, std::string>;
```

A strong id satisfies neither arm, so the two rules are mutually exclusive:
a rung obeying rule 3 cannot use the keying macros at all.

## Repro

```cpp
#include <morph/core/model_key.hpp>
#include <cstdint>
#include <optional>

struct SampleId {
    std::optional<std::int64_t> value{};
    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
};
struct OpenSample { SampleId sampleId; };
struct SampleModel {};

BRIDGE_MODEL_KEY(SampleModel, OpenSample, &OpenSample::sampleId);

int main() { return 0; }
```

`clang++ -std=c++23 -fsyntax-only -I include` (Homebrew clang 22.1.8):

```
error: no matching function for call to 'keyToString'
  note: candidate template ignored: constraints not satisfied [with K = SampleId]
  note: because 'SampleId' does not satisfy 'ModelKey'
  note: because 'is_integral_v<SampleId>' evaluated to false
  note: and 'std::same_as<std::remove_cv_t<SampleId>, std::string>' evaluated to false
```

## Why it is filed now

Three rungs now carry the *same* hand-written workaround — declare
`ModelKeyTraits<M>::PrimaryKey = std::int64_t` and hand-write
`ActionKeyTraits<A>::key()` to unwrap the id:

- `examples/ledger/include/ledger/models/ledger_model.hpp` (~line 323)
- `examples/kanban/include/kanban/models/board_model.hpp` (~line 417)
- `examples/lims/include/lims/models/sample_model.hpp`

`examples/IMPLEMENTATION.md`'s rule-of-three says the third consumer is the
point at which an app-built answer must either be promoted into
`include/morph` or explicitly dispositioned as app-layer by design. Both
earlier rungs' comments explicitly deferred the decision as out of their own
scope ("rather than widen `morph::model::ModelKey` itself ... out of this
ledger-only task's scope"), so it has never been taken.

## What should happen

One of:

1. Widen `ModelKey` to admit a type that exposes a `ModelKey`-satisfying
   payload — e.g. accept any `T` with `*t` convertible to an integral or
   `std::string`, which is exactly the shape `Quantity`/`Choice`/`Tagged`/
   the ladder's strong ids all share; `keyToString` then unwraps. The
   disengaged case needs a stated contract (throw, as all three rungs' hand
   written `key()` already does via `std::bad_optional_access`, is the
   behavior `BridgeHandler::execute`'s own `catch (...)` block already
   documents as the sanctioned rejection point).
2. Or state in `docs/spec/core/shared_instances.md` that keyed models are
   keyed on the *unwrapped* scalar by design and that the hand-written
   two-specialisation pair is the intended idiom, with a worked example — so
   the fourth rung copies a documented pattern instead of rediscovering it.

Widening also removes a real footgun the current workaround has: because the
macro is bypassed, nothing checks that the hand-written `key()` and the
hand-written `PrimaryKey` agree, and a mismatch routes callers to the wrong
shared instance silently.
