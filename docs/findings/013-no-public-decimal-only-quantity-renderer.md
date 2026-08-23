---
id: 013
title: A Quantity's exact decimal can only be rendered with its unit appended -- the decimal-only formatter is in morph::units::detail
subsystem: units
severity: paper-cut
source: lims rung 6, GUI work (rendering a result table)
disposition: open
test: examples/lims/gui_lib/lims_qml_conversions.hpp (the workaround) + examples/lims/tests/test_lims_qml_bridges.cpp
---

`morph::units::toString(quantity)` is the only public renderer of a
`Quantity`'s exact decimal, and it always appends the unit's display text
with no separator:

```cpp
lims::Concentration a{Rational{Numerator{1}, Denominator{400}, DecimalPlaces{4}}};
lims::Concentration b{Rational{Numerator{12}, Denominator{5}, DecimalPlaces{3}}};
std::printf("[%s] [%s]\n", morph::units::toString(a).c_str(),
                            morph::units::toString(b).c_str());
// [0.0025mg/L] [2.4mg/L]
```

The decimal-only formatter that produces the numeric half —
`morph::units::detail::formatRationalDecimal` — is in `detail`, and
`docs/CMakeLists.txt` excludes `morph::units::detail` from the public docs
entirely, so it is not offered as API.

`std::format("{}", rational)` is not the alternative it looks like: the
`Rational` formatter's empty spec prints the **fraction**, not the decimal.

```cpp
Rational b{Numerator{12}, Denominator{5}, DecimalPlaces{3}};
std::format("{}", b);   // "12/5", not "2.4"
```

A non-empty spec (`"{:.3f}"`) is documented as delegating to
`std::formatter<double>` on `toDouble()` — i.e. it leaves the exact domain,
which is the one thing a value of this type exists to stay inside.

## Why it matters

Any view that places the number and the unit separately — a table with the
unit in its column header, a right-aligned suffix, an editable field whose
adjacent label shows the unit — has to take the unit back off a string that
just had it put on. `examples/lims/gui_lib/lims_qml_conversions.hpp` does
exactly that:

```cpp
auto text = QString::fromStdString(::morph::units::toString(quantity));
return text.chopped(unitText<Q>().size());
```

This is safe today only because `toString` is literally
`formatRationalDecimal(value) + std::string{display}` — verifiable by reading
it, but not a documented contract, and silently wrong the day a separator or
a locale-aware layout is introduced between the two halves.

## What should happen

Promote the decimal-only renderer, e.g.

```cpp
namespace morph::units {
/// The exact decimal alone, with no unit suffix.
template <auto U, std::uint32_t Dec>
[[nodiscard]] std::string toDecimalString(const Quantity<U, Dec>& quantity);
}
```

`toString` then becomes `toDecimalString(q) + display`, which is what it
already is, and no caller has to reverse a concatenation. The unit half is
already reachable (`Quantity::unitMeta().display`), so this is the only
missing piece.
