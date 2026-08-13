---
id: 014
title: DecimalPlaces has a floor of 1
subsystem: forms
severity: minor
source: examples/LADDER.md, forms-subsystem gaps
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/53
---

`Quantity<U, DeclaredDecimals>` enforces `static_assert(DeclaredDecimals >= 1 && DeclaredDecimals <= math::kMaxDecimalPlaces, ...)` in `include/morph/util/quantity.hpp:550-551`, forbidding zero-decimal quantities. This is incompatible with currencies like JPY (Japanese Yen) and KRW (South Korean Won), which have no decimal subunit and conventionally represent prices as whole numbers.

**What happens instead:** apps that need zero-decimal currencies must either apply an app-layer convention (represent JPY prices as multiples of 100, then divide on display) or use a different type entirely, losing the forms palette integration and strong typing that `Quantity` provides.
