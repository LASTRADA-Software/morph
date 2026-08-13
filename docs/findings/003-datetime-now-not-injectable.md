---
id: 003
title: DateTime::now()/Timestamp::now() are not injectable for remotely-constructed models
subsystem: units
severity: major
source: IMPLEMENTATION.md rule 3
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/44
---

`DateTime::now()` (`include/morph/util/datetime.hpp:76-77`) and `Timestamp::now()` (`datetime.hpp:259-260`) call `std::chrono::system_clock::now()` directly with no injection point. Registry-constructed models are default-constructed via `include/morph/core/registry.hpp` with no constructor parameter, leaving no way to inject a mocked `now()` for deterministic testing of time-dependent behavior.

**What happens instead:** tests of time-dependent logic (e.g. "this record expires after 24 hours") must use real time or live with non-determinism, making the test suite harder to reason about and slower to run.
