# LIMS gallery batch E: expressions and charts

Batch E of the LIMS gallery initiative (`examples/lims_gallery/design/batches.md`, section E): the arithmetic a
result worksheet and a stability protocol compute as they are typed, and the chart a control screen draws. A field
read from a computed value, dates that follow a study's start, the standard deviation and RSD of a set of
preparations, the pharmacopoeial half-up rounding beside the half-even rounding `expr/1` already has, and a chart node
with series, reference lines and marked points that both frontends draw. It lands on `feat/lims-gallery` as one
commit per item, in two parts (below); the gallery drops each workaround in the same commit as the feature that makes
it unnecessary.

Every claim below was checked against `679e7b5d1` (batches A to D designed, A landed). Each item says whether its
premise was **reproduced** (run on this revision) or **read** (inferred from the code). Where the gap log was wrong
or incomplete, the item says so. Four throwaway probes were run, then kept out of the tree:

- an expression probe: `parseExpression` and `evaluate` on a `StaticEnvironment`, compiled with clang 23 against the
  headers of `679e7b5d1` (exported with `git archive`, so another lane's edits in the worktree do not reach it);
- a Qt Quick probe: a `Canvas` that draws a dashed line, a polyline and a filled circle, run with `qml6` (Qt 6.12.0,
  offscreen platform) and read back through `grabToImage`;
- exact reference values (Python `fractions` and `math.isqrt`) for every number this document and its corpus cases
  quote, the contrast ratios and colour distances of the chart palette (WCAG 2 relative luminance, CIE76 ΔE,
  Machado's colour-vision simulation), and the integer widths the statistics reach;
- a second Qt Quick probe: a 0.5 px line in a `Canvas` grabbed at `QT_SCALE_FACTOR` 1 and 2, on the default and the
  software scene graph.

Their output is quoted where an item rests on it.

| Item | Where it lands | Verdict on the gap log |
|---|---|---|
| walk's numeric segment | `walk`, which `ref`, `row`, `pref` and `get` read through | Not a gap; a library defect found while checking G36, fixed first so `get` does not inherit it (reproduced) |
| saturated rounding | one checked rounding for every place `expr/1` rounds, and the checked 128-bit integer it computes in | Not a gap; a library defect found while checking the rounding, fixed first so the mode and G45 do not inherit it (reproduced) |
| G36 | expressions (a `get` form), author | Right that it cannot be written. **Mis-attributed:** the refusal quoted is the C++ author API's (`author.hpp:584-588`), not the loader's; the loader has no form to refuse, since `ref` takes only a literal path (reproduced) |
| G37 | expressions (`dateAdd`), and the date functions' reading of a date | Right. **Incomplete:** `date("2026-11-03")` is `null` with a report today (reproduced), so even a correct pull date could not be shown; a date input in date mode writes exactly that text |
| G45 | expressions (`abs`, `sqrt`, `stdev`, `rsd`) | Right. The gallery's two-preparation formula also differs from the exact RSD in the second place for some inputs (computed below) |
| (review) half-up | expressions (a rounding mode on `round`, `decimal`, `sqrt`, `stdev`, `rsd`) | Right. `math::RoundingMode::HalfAwayFromZero` already exists (`rational.hpp:190-193`); `expr/1` never passes it |
| G55 | a `chart` node: loader, interpreter, view tree, `ChartWidget`, every backend, theme | Right. The mockup's own note, "Drawn by a host-provided chart component", names the route the generic client cannot take; a palette node is what reaches both frontends |

**Two parts.** The expressions touch no backend, and the chart touches every backend; they share nothing but the
gallery. So the batch lands as two pull requests on `feat/lims-gallery`, each green on its own:

| Part | Items | Conformance | Layout | Depends on |
|---|---|---|---|---|
| E1, expressions | walk's numeric segment, G36, saturated rounding, half-up, G45, G37 (with the date reading) | — | — | C's G13 for the gallery's `03-Dec-2026` text only |
| E2, the chart | G55 | 54 | L16 | Nothing; it can land before or after E1 |

Order inside E1: walk's numeric segment (`get` reads through `walk`), G36 (a form, no numbers), saturated rounding (it
brings the checked 128-bit integer and the one checked rounding every later item calls), the rounding mode (it changes
`round` and `decimal`, which G45's functions then take the same way), G45, then G37.

## The shape of the batch

Four rules hold throughout, so an author learns them once.

- **Numbers stay exact until a document says where to round.** `expr/1`'s numbers are `math::Rational`
  (`expressions.md:76-80`). A square root is the first operation whose result is not a rational, so every function
  that takes one (`sqrt`, `stdev`, `rsd`) takes the places it rounds to as an argument, and rounds the exact root
  once, correctly. No intermediate value is a floating-point number, and none is rounded before the last step.
- **Rounding is named where it happens.** Every function that rounds takes an optional last argument, the mode:
  `"halfEven"` (the default, and `expr/1`'s rounding today) or `"halfUp"`. The mode is the document's own word at the
  call, not a client setting, because a rounded value is data: it is compared with a limit, written into a mutation's
  body and signed. So a document rounds once, with `round`, and both shows and compares that value; `decimal` with a
  mode is for a value that is only shown. `dp` is always the number of decimal places of the result, never
  significant digits. Open question 2 weighs an application-wide default.
- **Absence is quiet, a domain error is loud.** As today (`expressions.md:66-75`), a `null` argument makes a strict
  function `null` without a report, and a type mismatch or an overflow is `null` with a report. This batch places its
  new edge cases on that line by one test: *is it what a screen holds while a user is still typing?* A standard
  deviation of fewer than two values, or a date input that has been cleared (`""`), is: `null`, unreported. A square
  root of a negative number, an RSD of a list whose mean is zero, or a date past year 9999 is not: `null`, reported.
- **A chart's drawing is the frontend's; everything it says is the interpreter's.** The lowering decides the range,
  the ticks, every label's text and the text alternative, from the exact values; the view tree carries doubles only
  for positions. So a chart never shows a number computed in floating point, and the Qt Quick chart, the terminal
  chart and the `RecordingBackend` log agree on every word.

**The expression version.** The form `get` and the functions `abs`, `sqrt`, `stdev`, `rsd` and `dateAdd` join
`expr/1` in place, as batch B's `sort`, `runs` and `slice` do and for B's reason (B, "The expression version"): the
loader and the corpus treat `expr/1` as current (`kExprVocabulary`, `bundle.hpp:36`), and a document that uses a new
form needs a client that has it. The optional mode on `round` and `decimal` is a new arity of existing functions; a
client older than this batch refuses `round(x, 2, "halfUp")` at load ("`round` takes 2 arguments, not 3",
reproduced), which is the same failure, in the same place, as an unknown function. The date functions' reading of
`yyyy-MM-dd` and `""` (G37) is the one change an older client cannot refuse at load, since it changes what an existing
function does with a value: there `date("2026-11-03")` stays `null` with a report, so the screen shows nothing where a
newer client shows the date. It never shows a different date, which is why it needs no new version.

## walk's numeric segment past the largest index (#1066)

**Problem (reproduced).** `walk` reads a list element by accumulating the segment's digits into a `std::size_t` with
no bound (`expression.hpp:1122-1126`), so a segment past 2⁶⁴ wraps and reads an element that exists. Its own contract
is "`null` when a segment does not exist" (`:1117`). The probe, with `xs` = `["a", "b"]`:

```
xs.1: "b"
xs.18446744073709551616 (2^64): "a"
xs.18446744073709551617 (2^64+1): "b"
```

A document writes such a path only by mistake, but the read is then a wrong value without a report, and `ref`, `row`,
`pref` and G36's `get` all read through `walk`.

**Decision.** The segment is parsed with `std::from_chars` into a `std::size_t`; a parse that does not consume the
whole segment, or reports `result_out_of_range`, reads `null`, as an index past the end does. No report: a path
through data that lacks the element is absence, as today.

**Tests.** Corpus, `evaluate`, with `xs` = `["a", "b"]`: `xs.18446744073709551616` and `xs.18446744073709551617`
are `null` with no report; `xs.1` stays `"b"`. **Control:** put the unbounded accumulator back, and the two long
paths read `"a"` and `"b"`.

## G36: a field of a computed value

**Problem (reproduced, and read).**

- `expr/1` reads a field only below a name. `{"ref": "name.path"}` and `{"row": "path"}` carry a literal path
  (`expressions.md:30-31`), and no other form reads a path. The probe:

  ```
  ref below a computed value: refused at /ref: a name and path is written as a string `name.path`
  ```

- The author API refuses earlier, in C++: `detail::fieldOf` throws "a field is read below a name -- a param,
  state, let, query, row, element or bound name -- and not below a computed value" (`author.hpp:584-588`). That is
  the text the gap log quotes as the loader's; it is the author's, and it describes a missing form, not a check.
- Two screens work around it the same way, with `map` before `first`: stability's `dateOf` (`stability.cpp:164-175`,
  commented "A field is read only below a name") and its lot `stock` (`:195`), and results' `valueOf`
  (`results.cpp:254-259`). The same workaround in two screens is a framework seam (AGENTS.md, The bar).

**Decision: a form that reads a path below any expression.**

- `{"get": e, "path": "a.b.0"}` evaluates `e` and reads `path` out of it with `walk` (`expression.hpp:1118-1139`),
  the reader `ref` and `row` already use: a record's field by name, a list's element by index, `null` when a segment
  does not exist. So `get` means exactly what `ref` means after its head, and the two never disagree on a path.
- `path` is a literal string, non-empty, in `ref`'s dotted syntax. It is the form's companion key, beside `map`'s `as`
  and `to` (`expressions.md:52-53`). A path computed at run time stays out of `expr/1`: no screen needs one, and a
  literal path is what keeps a document's reads checkable.
- A form, not a function: a function would take its path as a string argument, which batch B rejected for `sort` for
  the same reason ("a function would take a field path as a string, which no other function does").
- `null` below propagates as `ref`'s path does: `get` of `null` is `null`, unreported. A path through a scalar is
  `null`, unreported, as `ref through a scalar` is today (corpus, line 23).
- Static type: `any`, as a `ref` with a path into data the document does not describe.

**Author.** `Handle<T>::at`, both the by-name and the by-member-pointer overload (`author.hpp:703`, `:711`), on a
value that is not a named read writes `get` instead of throwing; `at` on a `get` extends its path, so
`first(xs).lot.stock` is one node. `fieldOf`'s exception goes, and so does the documented `@throws` on it and on both
overloads.

**Spec.** `expressions.md`: the Forms table gains `get`, and the companion-key sentence (`:52-53`) lists `path`.
`author.md`: `at` on any handle.

**Tests.**

- `tests/ui/corpus/expressions.json`, `evaluate`:
  - `get` of `first` of a literal list of records, path `date`, is the first record's date;
  - a two-segment path through a list index (`items.1.name`);
  - `get` of `null`, of a missing field and of a scalar are `null` with no report.
- `refused`: `get` without `path`, with an empty `path`, with a non-string `path`, and with a stray companion key.
- `tests/test_ui_author.cpp`: `ui::call("first", {xs}).at("date")` writes `{"get": …, "path": "date"}`, and
  `.at("a").at("b")` on it writes one node with path `a.b`. **Control:** keep the throw, and both cases fail.
- **Control** for the form: make `get` return its operand unwalked, and the first corpus case returns the record.

**Gallery.** Stability's `stock` and results' `valueOf` read the field with `.at(…)` on the `first` of the filter. The
`map`-then-`first` workaround and its comments go. Stability's `dateOf` goes with G37.

**Risks.** None found. `get` reads through `walk`, whose numeric segment the item before this one bounds.

## Saturated rounding (#1065)

**Problem (reproduced, and read).**

- `roundToDecimalPlaces` scales the value by `10^places` with the saturating `operator*` (`rational.hpp:1396`), which
  clamps to the `int64` edge and logs. `round` checks the product first (`expression.hpp:1832-1834`) and reports;
  `mean` (`:1724-1728`) does not, and nor does `detail::decimalText` (`message.hpp:54-56`), through which `decimal`,
  `number`, `format`, `money`, a text's decimal and a message's `:number` placeholder show a value. The plural
  category and an exact `.match` key round the same way (`message.hpp:276-277`, `:713-714`). So these return the
  clamped value as if it were exact, which `expressions.md:73-75` rules out.
- The overflow depends on the numerator, not on the value: the product is `|num| · 10^places / gcd(den, 10^places)`,
  so an exact quotient whose denominator is prime to 10 overflows at 18 places once its numerator reaches 10, however
  small the value. Probe, at `679e7b5d1`, with `q28` = `div(28 at 18 places, 3)` and `10/3` = `div(1.0, 3)`:

  ```
  [ERROR] [Rational] operator*= overflowed int64 and saturated; the result is clamped, not exact. Use checkedMul to detect this instead.
  mean [9,9,10] at 18dp: {"num":9223372036854775807,"den":1000000000000000000,"dp":18}
  number(q28): "9.223372036854775807"
  format(q28): "9.223372036854775807"
  decimal 10/3 at 18: "9.223372036854775807"
  round 10/3 to 18 (result fits): null  [report at /: the result overflows]
  round 28/3 to 18: null  [report at /: the result overflows]
  ```

  `3.333…` shows as `9.223372036854775807`. And `round`'s check is too strict: `3.333333333333333333` fits an
  `int64` numerator, but the product on the way does not, so it is refused. Lab values rarely carry 18 places, but a
  wrong number shown without a report is the one outcome a result screen must not have, and E1's mode and G45's
  functions all round through this path.

**Decision: one checked rounding, computed in 128 bits.**

- `math::checkedRoundToDecimalPlaces(Rational, DecimalPlaces, RoundingMode) -> std::optional<Rational>` in
  `rational.hpp`: `|num| · 10^places` in the checked 128-bit integer (G45 uses the same type), divided by `den` with
  remainder `r`; the quotient is raised by one when `2r > den`, or when `2r = den` and the mode is half away from zero
  or the quotient is odd; the sign is put back. It is empty only when the rounded numerator does not fit an `int64`,
  so `10/3` at 18 places is `3.333333333333333333` and `28/3` at 18 places is empty.
- Every place `expr/1` rounds calls it: `round` (whose product check goes), `mean`, and `detail::decimalText`, which
  returns `std::optional<std::string>`. An empty result is `null`, reported "the result overflows", at the calling
  function's node; in a message the placeholder shows as `{$name}` and the error is returned, as for a value of the
  wrong kind, which `t` reports. The plural category and the exact key use it too, and a value they cannot round
  selects only `*`.
- `roundToDecimalPlaces` keeps its saturating contract for its callers outside `expr/1`.
- The 128-bit integer lands here: add, subtract, multiply, divide with remainder and compare, as one portable
  implementation (G45, Width). G45 adds its integer square root.

**Tests.** Corpus, `evaluate`:

- `mean` of `[9, 9, 10]` at 18 places, `decimal(q28, 18)`, `number(q28)` and `format("{0}", q28)` are `null` with one
  report each. **Control:** put the saturating `operator*` back, and each gives `9.223372036854775807`.
- `round(div(1.0, 3), 18)` and `decimal(div(1.0, 3), 18)` are `3.333333333333333333`. **Control:** keep `round`'s
  64-bit product check, and `round` is `null` with a report.
- `round(-2.5, 0)` is `-2` and `decimal(-0.125, 2)` is `"-0.12"`: half-even ties on the negative side are unchanged.
  The half-up ones are pinned with the mode (next item).
- `tests/test_ui_message.cpp`: `{$n :number}` with `n` = `q28` formats as `{$n}` and returns an error.
- `tests/test_rational_checked.cpp`: `checkedRoundToDecimalPlaces` against `roundToDecimalPlaces` on values whose
  product fits, both modes, both signs; empty for `28/3` at 18.

## Half-up rounding beside half-even (the review note)

**Problem (reproduced, and read).**

- "Rounding is half to even wherever `expr/1` rounds: `round`, `mean`, and every display" (`expressions.md:81`).
  The probe: `round(0.125, 2)` is `0.12`, and `decimal(0.125, 2)` shows `"0.12"`.
- The pharmacopoeias round the other way. USP's General Notices (7.20, Rounding Rules) and Ph. Eur. 1.2 round a
  result to the places of its limit by looking at the next digit only: below 5 it is dropped, 5 or above raises the
  digit before it. On the exact value that is half away from zero: `0.125` reports as `0.13`. A lab that reports to
  those rules cannot write it in `expr/1`.
- The arithmetic exists: `math::RoundingMode` has `HalfAwayFromZero` and `HalfEven` (`rational.hpp:190-193`), and
  `roundToDecimalPlaces` takes either (`:1380-1406`). `expr/1` passes `HalfEven` everywhere: `mean` and `round`
  (`expression.hpp:1727`, `:1836`), and `decimal`, `money` and every display through `detail::decimalText`
  (`message.hpp:54-56`).
- The rule's point is that a result is rounded once, from the unrounded value: rounding `1.2449` to `1.245` and then
  to `1.25` is what it forbids. `mean` already rounds, at the elements' widest places and half to even
  (`expressions.md:121-122`, `expression.hpp:1724-1728`), so `decimal(mean(xs), 1, "halfUp")` would round twice.
  For `99.6` and `99.3` the exact mean is `99.45`: `mean` gives `99.4`, and no later half-up can recover the `99.5`
  the pharmacopoeia reports.

**Decision: a mode argument on every function that rounds, and no change to `mean`.**

- `round(x, dp, mode?)`, `decimal(x, dp, mode?)`, and the new `sqrt`, `stdev` and `rsd` (G45) take an optional last
  argument, `"halfEven"` or `"halfUp"`. Absent, it is `"halfEven"`, so every existing document keeps its
  values.
- `"halfUp"` is half away from zero: `-0.125` rounds to `-0.13`. That is what the pharmacopoeial rule does to a
  negative result (it looks at the magnitude's next digit), what Java's and ICU's `HALF_UP` mean, and what
  `RoundingMode::HalfAwayFromZero` already computes. The name is the one a lab's SOP uses.
- A literal mode is checked at load; any other literal string is refused ("the rounding mode is `halfEven` or
  `halfUp`"). A computed mode that is neither is a mismatch: `null`, reported.
- **`mean` keeps its rounding.** Changing it would change values every existing document computes. A document that
  reports to the pharmacopoeial rule computes the exact mean, `div(sum(xs), count(xs))`, which `div` keeps exact
  (`expressions.md:78-80`; the probe: `div(1.0, 3)` is `{"num":1,"den":3,"dp":1}`), and rounds it once with
  `decimal(…, dp, "halfUp")`. `expressions.md` says so beside `mean`. Open question 3 asks whether `mean` should
  gain the mode too.
- `number`, `quantity`, `money` and `format` keep half to even: they show a value at its own places, which a
  computation already chose; a value that needs a rule rounds through `round` or `decimal` first.

**Lowering.** None: a function of the expression library. `FunctionSpec` (`expression.hpp:177-188`) gives `round`
and `decimal` a `maxArgs` of 3 with `String` as the third argument's kind. A mode is read once per call by one helper,
`roundingMode(Value const&) -> std::optional<math::RoundingMode>`, which the five functions share, and passed to the
one checked rounding (the item before this one); `decimal` passes it on through `detail::decimalText`, which gains the
mode as a parameter. The literal check is new to `checkExpression`: today a literal style of `date` is checked only
when it is evaluated (corpus, line 255), and the mode is checked at load because a wrong mode is a wrong value, not a
wrong look.

**Author.** `enum class Rounding : std::uint8_t {HalfEven, HalfUp}`, a `ui::round(value, places, Rounding)` overload
beside `ui::round` (`author.hpp:1020-1026`), and `ui::decimal(value, places)` with the same overload, which is new:
the author API has no `decimal` helper today, and the gallery writes `ui::call("decimal", …)`. Each writes the mode
only when it is not `HalfEven`, so existing golden documents are unchanged.

**Spec.** `expressions.md:81`: "Rounding is half to even unless the call names `halfUp`, which is half away from
zero", the five functions that take the mode, and the note on `mean` against `div(sum, count)`. The Functions table
shows `round(x, dp, mode?)` and `decimal(x, dp, mode?)`.

**Tests.** Corpus, `evaluate`:

- `round(0.125, 2)` is `0.12` and `round(0.125, 2, "halfUp")` is `0.13`; `round(-0.125, 2, "halfUp")` is `-0.13`;
  `round(0.135, 2, "halfUp")` is `0.14`, the same as half-even (a tie that is already even-up).
- `decimal(div(add(99.6, 99.3), 2), 1, "halfUp")` is `"99.5"`; `decimal(mean([99.6, 99.3]), 1, "halfUp")` is `"99.4"`.
  The pair pins the double rounding the spec warns about.
- `round(0.125, 2, null)` is `null` with no report: a `null` mode is an absent argument like any other
  (`call`, `expression.hpp:1747-1756`), so a mode read from data that is still loading never rounds the wrong way.
- `round(0.125, 2, {"ref": "m"})` with `m` = `"up"` is `null`, one report.
- `refused`: `round(x, 2, "up")`, a literal mode that is not one of the two.
- **Control:** pass `HalfEven` whatever the mode, and the three half-up cases fail; read a `null` mode as the
  default, and the `null`-mode case gives `0.12`.

**Gallery.** Results' assay mean is the pharmacopoeial reportable value. `sheet.mean` is `mean` today
(`results.cpp:327`), and the line both shows it with `decimal(mean, 1)` (`:282`) and compares it with the
specification limits (`:280`, `:285`). It becomes `round(div(sum, count), 1, "halfUp")`, shown with
`decimal(sheet.mean, 1)`, so the value the limit check reads is the value the analyst sees; rounding only the display
would show `99.5` while comparing `99.4`. The worksheet's typed values do not change; a new gallery test types `99.3`
into the second preparation and reads `Assay, mean 99.5`, where half-even would show `99.4`. A second test types
`105.0` and `105.1`: the exact mean `105.05` reports as `105.1`, above the mean line's `105.0` limit
(`results.cpp:99-101`), and the mean line is marked out of specification. **Control:** round only the display, and
the line shows `105.1` unmarked, since the half-even `105.0` it compares is within the limit.

**Risks.** A mode on `decimal` lets one screen show a value two ways. That is the document's choice, and the default
is today's.

## G45: square root, standard deviation and RSD

**Problem (read; the arithmetic computed).**

- The arithmetic functions are `add`, `sub`, `mul`, `div`, `neg` and `round`, and the list functions `count`, `sum`,
  `mean`, `min` and `max` (`expressions.md:110-113`; `kFunctions`, `expression.hpp:192-255`). No root, no absolute
  value. The probe: `sqrt(2)` is "refused at /fn: the function `sqrt` is not in expr/1".
- Results' workaround holds only for two preparations: SD = |a − b| / √2, so RSD = |a − b| × 70.7107 / mean
  (`results.cpp:170-174`, `:328-330`), the constant being 100 / √2 at four places and |a − b| a `cond`. Beyond the
  three-preparation limit the gap log names, it also rounds early: the mean is rounded at one place, and the root at
  four. Comparing it with the exact RSD over every pair of one-place results from 99.0 to 101.0, the first three pairs
  where the two differ:

  ```
  diff 99.3 99.8 workaround 0.35 exact 0.36
  diff 99.6 100.3 workaround 0.49 exact 0.5
  diff 99.8 100.7 workaround 0.64 exact 0.63
  ```

  For the design's 99.6 and 99.2 both give `0.28`.
- A root cannot be computed in floating point and then rounded: a tie is lost. `√0.2025` is exactly `0.45`, but the
  double nearest it is `0.450000000000000011102230246251565404236316680908203125`, which half-even rounds to `0.5`,
  not `0.4` (Python, reproduced). Results compared with a limit at its places meet ties often, since lab data has
  few digits.

**Decision: four functions, exact until one correctly rounded result.**

| Function | Takes | Gives |
|---|---|---|
| `abs(x)` | a number or quantity | its magnitude, of the same kind, places and unit |
| `sqrt(x, dp, mode?)` | a number, 0 or more; `dp` 0 to 18 | the square root, correctly rounded at `dp` places |
| `stdev(list, dp, mode?)` | numbers, or quantities in one dimension | the sample standard deviation (n − 1), correctly rounded at `dp` places, in the first element's unit |
| `rsd(list, dp, mode?)` | the same | the relative standard deviation in percent, 100 × SD / \|mean\|, correctly rounded at `dp` places, a plain decimal |

**Semantics.**

- **Correctly rounded** means: of the decimals at `dp` places, the one nearest the exact real result; on an exact
  tie, the one `mode` picks. The result is always a decimal at `dp` places: `sqrt(4, 0)` is the decimal `2` at 0
  places. (`round` returns an `int` argument unchanged; a root of an `int` is not in general one, so its kind does not
  depend on the value.)
- **Exact throughout.** `stdev` is the square root of the exact sample variance, `(n·Σx² − (Σx)²) / (n·(n − 1))`;
  `rsd` is the square root of `10⁴ · n · (n·Σx² − (Σx)²) / ((n − 1) · (Σx)²)`. Neither rounds the mean or the
  variance first. The one-pass textbook formula is safe here, because exact arithmetic has no cancellation; it is
  the formula floating-point code avoids.
- **The root.** The value whose root is taken is an exact fraction `m = a / b ≥ 0`. For `dp`: let `A = a · 10^(2·dp)`
  and `B = b`, so `M = A / B`; `q = ⌊√⌊A / B⌋⌋` by integer square root, which is `⌊√M⌋` exactly; the result is
  `q / 10^dp`, raised by one unit in the last place when `4A > (2q + 1)² · B`, or when `4A = (2q + 1)² · B` and the
  mode is `halfUp` or `q` is odd. The tie test reads `A` and `B`, not `⌊A / B⌋`: truncating `M` first makes
  `4M` even and `(2q + 1)²` odd, so it never sees a tie and misses every value just above one (`sqrt(6.3, 0)` would be
  `2`). Every step is an integer operation. This is the throwaway script's `round_sqrt`, which produced the values
  in this section.
- **Width.** `math::Rational` is a pair of `int64`s. The functions work in a checked 128-bit integer. The elements are
  put over one denominator `D`, the least common multiple of their canonical denominators, as integers
  `Xᵢ = xᵢ · D`: a decimal's canonical denominator divides its `10^places`, and an exact quotient such as `div(1, 3)`
  has its own, so scaling by the places alone would not make it an integer. Then
  `stdev`: `A = (n·ΣXᵢ² − (ΣXᵢ)²) · 10^(2·dp)`, `B = n·(n − 1)·D²`; and `rsd`, where `D` cancels:
  `A = 10^(4 + 2·dp) · n · (n·ΣXᵢ² − (ΣXᵢ)²)`, `B = (n − 1)·(ΣXᵢ)²`. The bound, stated in `expressions.md`: the result
  is exact whenever `D`, `A`, `B` and the result's numerator `q` fit (`D` and `q` in 63 bits, `A` and `B` in 127);
  past that it is `null`, reported "the statistics overflow". In the worst case for ten thousand values of seven
  significant digits (bounding `n·ΣXᵢ² − (ΣXᵢ)²` by `n·ΣXᵢ²`), `stdev` fits up to 7 places and `rsd` up to 3, which
  is past what either is reported to (computed: `stdev`'s `A` takes 120 bits at 7 places and 127 at 8; `rsd`'s 120
  at 3 and 127 at 4). Real data, whose spread is small beside its magnitude, sits far inside.
- **One 128-bit implementation.** The integer is portable two-limb arithmetic (add, subtract, multiply, divide with
  remainder, compare, integer square root) in `include/morph/util/rational.hpp`, beside the limb product it already
  has (`mulU64`, `:338-360`). It is the only implementation on every compiler: an `#ifdef __SIZEOF_INT128__` branch
  would leave the limb code to MSVC alone, where a mistake passes on Linux and fails only on Windows CI. On GCC and
  Clang the tests compare it with `unsigned __int128`.
- **Domain.**
  - `sqrt` of a negative number: `null`, reported ("the square root of a negative number").
  - `stdev` or `rsd` of fewer than two values: `null`, unreported. A worksheet holds one preparation while the second
    is typed, as `mean([])` is `null` today.
  - `rsd` of a list whose sum is zero: `null`, reported ("the RSD of values whose mean is zero").
  - A negative mean: `rsd` uses its magnitude, so the RSD is never negative.
  - `stdev` of equal values is `0` at `dp` places.
- **Null and kinds.** As `sum` and `mean` (`expressions.md:121-122`, `expression.hpp:1681-1720`): a `null` element
  makes the result `null`, unreported; an element that is not a number is a mismatch; quantities convert to the first
  element's unit, and a quantity among plain numbers is a mismatch. `stdev` of quantities is a quantity in that unit;
  `rsd` of quantities is a plain decimal, the units having cancelled. `sqrt` of a quantity is a mismatch: `expr/1`
  has no unit to the half power.
- `dp` outside 0 to 18 is a mismatch, as for `round` (`expression.hpp:1820-1823`).
- `abs` of the most negative `int64` overflows: `null`, reported, as `neg` does.
- Static types: `abs` is `any` (its argument's kind, as `neg`), `sqrt` and `rsd` `decimal`, `stdev` `any` (a
  decimal, or a quantity).

**Why these four, and not a power or a variance.** The designs ask for the SD and the RSD (08's "RSD 0.28 %", the
system-suitability "RSD of 6 injections"); `abs` is the piece of `cond(a > b, a − b, b − a)` every spread needs.
`variance` is exact and needs no new arithmetic, but no design shows one. A `pow` with a fractional exponent has the
root's problem without the root's use. Population SD (n) is not what a lab reports: preparations and injections are
a sample of the batch, and the definitions a lab reports against divide by n − 1 (USP <1010> for a set of results;
Ph. Eur. 2.2.46 and USP <621> for the system-suitability RSD of replicate injections). For two preparations the
population SD is the sample SD over √2, so the choice is not a rounding detail: 99.6 and 99.2 give an RSD of 0.28 %
with n − 1 and 0.20 % with n. All three stay out until a screen asks.

**Author.** `ui::abs(value)`, `ui::sqrt(value, places, Rounding)`, `ui::stdev(list, places, Rounding)`,
`ui::rsd(list, places, Rounding)`, each defaulting the mode, beside `ui::round`.

**Spec.** `expressions.md`: the Arithmetic row gains `abs` and `sqrt(x, dp, mode?)`; the Lists row gains
`stdev(list, dp, mode?)` and `rsd(list, dp, mode?)`; a paragraph gives correct rounding, the formulas, the width
bound and the domain rules above. `author.md`: the helpers.

**Tests.** Corpus, `evaluate` (every expected value from the exact reference script):

- `sqrt(2, 4)` is `1.4142`; `sqrt(2, 0)` is `1`.
- `sqrt(0.2025, 1)` is `0.4`, and with `"halfUp"` `0.5`. **Control:** compute the root in `double` and round it, and
  the half-even case gives `0.5`.
- `sqrt(6.25, 0)` is `2`, and with `"halfUp"` `3`; `sqrt(12.25, 0)` is `4` both ways (the even neighbour is the upper
  one). **Control:** drop the tie test, and `6.25` gives `3` half-even.
- `sqrt(6.3, 0)` is `3`. **Control:** truncate `M` to an integer before the tie test, and it is `2`.
- `sqrt(-1, 2)` is `null` with one report; `sqrt` of a quantity is `null` with one report; `sqrt(2, 19)` is `null`
  with one report.
- `stdev([99.6, 99.2], 4)` is `0.2828`, and `rsd([99.6, 99.2], 2)` is `0.28`.
- `stdev([100.2, 99.8, 100.1, 99.9, 100.0, 100.3], 3)` is `0.187`, and `rsd` of the same at 2 is `0.19` (variance
  exactly `7/200`).
- `rsd([99.3, 99.8], 2)` is `0.36`. **Control:** the gallery's formula gives `0.35`, which is what the corpus would
  read if `rsd` rounded its mean first.
- `stdev([5, 5], 2)` is `0.00`; `stdev([1], 2)` and `stdev([], 2)` are `null` with no report; `stdev([1, null], 2)` is
  `null` with no report.
- `rsd([-1, 1], 2)` is `null` with one report; `rsd([-99.6, -99.2], 2)` is `0.28`.
- `stdev([1 g, 1500 mg], 3)` is `0.354 g`; `rsd` of the same at 1 is `28.3` (a plain decimal).
- `abs(-0.125)` is `0.125` at 3 places; `abs(-5)` is the `int` `5`; `abs(-2 mg)` is `2 mg`.
- `stdev([div(1, 3), 1], 4)` is `0.4714`. **Control:** scale the elements by their places instead of a common
  denominator, and the quotient is not an integer: the result is wrong or `null`.
- An overflow: `stdev` of two values `±9·10¹⁸` at 0 places is `null` with one report (`n·ΣXᵢ²` takes 128 bits).
  **Control:** a 64-bit accumulator wraps and gives a number.
- `refused`: `sqrt` with one argument; `stdev` with a literal mode `"up"`.
- `tests/test_rational_checked.cpp`: the 128-bit integer's operations, and its square root on squares, on squares ± 1
  and on `2¹²⁶`, against a reference; on GCC and Clang also against `unsigned __int128` over a fixed pseudo-random
  sample. **Control:** an `isqrt` that rounds to nearest fails `squares − 1`.

**Gallery.** `kRsdFactor`, its comment, `spread` and the formula go (`results.cpp:170-174`, `:328-330`).
`assayRsd` is `rsd(list(prep1, prep2), 2)`. The design's `RSD 0.28 %` stays, and a third preparation would now be
counted. The console's control lines (G55) read the established mean and SD the gallery holds, not `stdev` of the
plotted runs: a Levey-Jennings chart's limits come from a reference period.

**Risks.**

- **The 128-bit arithmetic is new code.** It has one implementation on every compiler (Width, above), so the code
  Linux tests is the code Windows runs; the rational tests run on all three CI platforms, and on GCC and Clang they
  also compare it with `unsigned __int128`.
- **`rsd` in percent** is a convention a document cannot see from the name alone. The Functions table says
  "in percent" in the row, and the corpus pins `0.28` for the design's pair.

## G37: date arithmetic, and reading a date

**Problem (read, and reproduced).**

- **No date arithmetic (read).** The functions are logic, arithmetic, strings, lists and display
  (`expressions.md:107-116`). A timestamp can be compared (`lt`, `expression.hpp:1458-1459`) and shown, nothing else.
  Stability writes each pull's date into a table from the design's T0 (`stability.cpp:61-79`) and looks it up by
  month (`dateOf`, `:164-175`), so the dates stay 03-Dec-2026 and onwards whatever start the user picks in the
  "Study start (T0)" input (`:302-308`).
- **A date cannot be shown (reproduced; not in the gap log).** A date input in `date` mode bound to a string writes
  `yyyy-MM-dd` (`document.md:201-205`), and stability's `start` is such a string (`stability.cpp:187`). The date
  functions read text with `DateTime::fromIso8601` (`expression.hpp:1974`), which requires a time of day
  (`datetime.hpp:136-147`: "`YYYY-MM-DDTHH:MM:SS`, optionally followed by `.m`… and an optional trailing `Z`"). The
  probe, at offsets 0 and −300:

  ```
  date('2026-11-03') at +0: null  [report at /: the date functions take a timestamp and a style]
  date('2026-11-03') at -300: null  [report at /: the date functions take a timestamp and a style]
  ```

  So the value a date input writes is one `date(…)` refuses, and every screen that shows a picked date shows the raw
  ISO text (stability's summary shows `study.start` as it is, `stability.cpp:530`). A pull date computed from it
  would have the same problem. This belongs to G37, since its results are dates; it is fixed here, not filed.
- **The client holds an offset, not zone rules.** `FormatContext::utcOffsetMinutes` (`expression.hpp:1056-1057`) is
  one fixed offset, and batch C keeps it so: "Zone names such as `CEST` are not a token: the client holds an offset,
  not zone data" (C, G13). Any calendar arithmetic on an instant therefore happens at a fixed offset.

**Decision: dates and instants are two values, and one function adds to either.**

A LIMS works in both. A pull date, an expiry date and a retest date are calendar dates: "03-Dec-2026" is the same day
in Basel and in Boston, and has no time of day to shift. A sample's receipt, an injection and a signature are
instants. `expr/1` already carries both, without naming them: a timestamp, and a date input's `yyyy-MM-dd` text. This
batch names them and keeps them apart.

- **A date** is ISO text `yyyy-MM-dd`. It has no zone. Shown by `date(x)`, it is that calendar day at any display
  offset; shown by `dateTime(x)`, it is midnight of that day in the display zone, which is how a date input already
  reads one (`interpreter.md:213`, "shows a `yyyy-MM-dd` date as midnight in the display zone";
  `interpreter.hpp:4217`).
- **An instant** is a timestamp, or ISO date-time text as today.
- **`""`** in any date function is absence: `null`, unreported. It is what a cleared date input writes
  (`document.md:204`), and a screen with an empty start date is a screen being filled, not an error. Other text that
  is neither form is a mismatch, as today.

`dateAdd(value, amount, unit)`:

| Argument | Accepts |
|---|---|
| `value` | a date, an instant (timestamp or ISO date-time text), `""` |
| `amount` | an `int`, negative to go back |
| `unit` | `"days"`, `"weeks"`, `"months"`, `"years"`, `"hours"`, `"minutes"` |

- **The result keeps the value's form**: a date gives a date (`yyyy-MM-dd`), a timestamp a timestamp, and ISO
  date-time text the same instant as `DateTime::toIso8601` writes it (`yyyy-MM-ddTHH:MM:SS.mmmZ`), which is also what
  a date input in `date-time` mode writes (`interpreter.hpp:6230`). A document that binds the result to a date input
  gets what it gave. Two dates compare correctly with `lt`, which orders strings by bytes (`expressions.md:89-92`);
  instant text compares correctly only against text in the same spelling, since `…:00Z` sorts after `…:00.500Z`, so
  a document that orders instants holds timestamps. `expressions.md` says so beside `lt`.
- **Days and weeks** on a date move the calendar day. On an instant they add 24 and 168 hours, which is the same thing
  at a fixed offset.
- **Months and years** move the calendar month, keeping the day and clamping it to the month's last day: `2026-01-31`
  plus one month is `2026-02-28`, and `2028-02-29` plus one year is `2029-02-28`. A year is twelve months. On an
  instant the calendar is the one at the display offset: the instant is shifted by `utcOffsetMinutes`, its civil
  date moved, and the same offset taken off again, so the wall-clock time at that offset is kept.
- **Clamping is not associative**, and the spec says so: `2026-01-31` plus one month, plus one month again, is
  `2026-03-28`, while plus two months is `2026-03-31`. A schedule adds each timepoint to T0, never to the previous pull.
  This is the convention of `java.time`'s `plusMonths` and .NET's `AddMonths`; the other convention, overflowing into
  the next month (JavaScript's `Date`), would put a pull one month after 31 January on 3 March.
- **Hours and minutes** on a date are a mismatch: `null`, reported ("a date has no time of day; add hours to a
  timestamp"). On an instant they add exact durations.
- **Range.** A result outside years 1 to 9999 is `null`, reported ("the date is out of range"): ISO text has four year
  digits. The arithmetic is checked, so a huge `amount` reports the same and never wraps: `amount` is compared with
  the span of years 1 to 9999 in its unit before any `std::chrono` arithmetic, because `std::chrono::days` and
  `months` count in an `int`-sized type and an `int64` amount converted to one would narrow first.
- **Null.** Any `null` argument gives `null`, unreported.
- **Static checking.** `amount` must be `int` where its type is known, so a literal `1.5` is refused at load. A
  literal `unit` is checked at load against the six names. The result's static type is the first argument's
  (`timestamp` or `string`), else `any`.

**Daylight saving time.** At a fixed offset, a month added to `09:00 +01:00` in January is `09:00 +01:00` in July,
which a wall clock in Basel shows as 10:00 CEST. That is correct arithmetic on the information the client has, and
the display shows the same `+01:00` it computed with (batch C's `xxx`), so nothing is shown that was not computed.
Documents that mean a calendar date hold a date, which has no offset and no DST: a pull date is right at any offset,
in any season. Wall-clock arithmetic across a DST change on an instant needs zone rules in the client; it stays out of
ui/1 (Not in this batch).

**Lowering.** None beyond the library: `dateAdd` is a function, and `date`/`dateTime` read a value through one
helper, `readDateValue(Value) -> {date | instant | absent | unreadable}`, which `dateAdd` shares. The date input
already has this reading: `dateInstantOf` (`interpreter.hpp:4215-4249`) takes a timestamp as its instant, a
`yyyy-MM-dd` string as midnight in the display zone, `null` and `""` as empty, and other ISO text through
`fromIso8601`. `readDateValue` moves that reading into `expression.hpp`, and `dateInstantOf` becomes a call to it
that turns a date into its midnight, so the date input and the date functions cannot read one string two ways.
`dateInstantOf` recognises a date with the forms engine's `isIsoDate` (`forms/engine/detail/text.hpp:506`), which
`expression.hpp` does not include; the reader parses the ten characters itself and checks the day with
`year_month_day::ok()`, and the forms engine's `isIsoDate` is not changed. Civil
arithmetic uses `std::chrono::year_month_day` and `sys_days`, as `DateTime`'s constructor does
(`datetime.hpp:89-94`).

**Author.** `enum class DateUnit {Days, Weeks, Months, Years, Hours, Minutes}` and `ui::dateAdd(value, amount,
DateUnit)`, where `amount` is a `Handle<std::int64_t>` or an integer.

**Spec.** `expressions.md`: a Dates row in the Functions table, `dateAdd(x, n, unit)`; a "Dates and instants"
paragraph under Semantics with the rules above; Formatting (`:147-149`) says `date` and `dateTime` take a date as
well, and that `""` is absent. `document.md:201-205` links to it, since the date input's rule for a date is now the
expression language's too.

**Tests.** Corpus, `evaluate`, all at offset 0 unless given:

- `dateAdd("2026-11-03", 1, "months")` is `"2026-12-03"`, with `36` it is `"2029-11-03"`, and with `3` it is
  `"2027-02-03"` (the table the gallery holds today, computed).
- `dateAdd("2026-01-31", 1, "months")` is `"2026-02-28"`; `dateAdd("2026-01-31", 2, "months")` is `"2026-03-31"`;
  `dateAdd("2028-02-29", 1, "years")` is `"2029-02-28"`; `dateAdd("2026-08-31", -6, "months")` is `"2026-02-28"`.
  **Control:** overflow instead of clamping, and the first gives `"2026-03-03"`.
- `dateAdd("2026-11-03", 2, "weeks")` is `"2026-11-17"`.
- Timestamp `t1` = `2026-02-28T23:30:00Z`, offset 60: `dateAdd(t1, 1, "months")` is `2026-03-31T23:30:00Z` (the local
  date is 1 March, so one month on is 1 April, local). **Control:** compute the calendar in UTC, and it is
  `2026-03-28T23:30:00Z`.
- `dateAdd(t1, 90, "minutes")` is `2026-03-01T01:00:00Z` at any offset.
- `dateAdd("2026-11-03", 1, "hours")` is `null` with one report; `dateAdd("9999-12-31", 1, "days")` is `null` with one
  report; `dateAdd("2026-11-03", {"ref": "n"}, "days")` with `n` = `1.5` is `null` with one report.
- `dateAdd("", 1, "months")` is `null` with no report.
- `dateAdd("2026-11-03", 9223372036854775807, "days")` and the same in `"months"` are `null` with one report.
  **Control:** convert `amount` to `std::chrono::days` or `months` before the range check, and the narrowed count
  gives a date.
- `dateAdd("2026-02-30", 1, "days")` is `null` with one report: the text is ten characters of digits and dashes but
  not a day. **Control:** read the date without `ok()`, and it gives a date in March.
- `date("2026-11-03")` is `"2026-11-03"` at offset 0 and at offset −300; `dateTime("2026-11-03")` at offset −300 is
  `"2026-11-03 00:00"`. **Control:** read a date as midnight UTC and shift it, and the −300 cases give the 2nd.
- `date("")` is `null` with no report; `date("yesterday")` keeps its one report (corpus, line 256).
- `refused`: `dateAdd(x, 1, "fortnights")`; `dateAdd(x, 1.5, "days")` with a literal decimal.

**Gallery.**

- `kTimepoints` loses its `date` column and its comment (`stability.cpp:61-79`), and `dateOf` goes (`:164-175`). A
  pull's date is `dateAdd(start, month, "months")`, so the summary's first and last pull follow the T0 the user picks.
- The summary and the pull dates show through `date(…)`, so they read `03-Dec-2026` with batch C's application
  pattern (C, G13) and `2026-12-03` without it. The existing checks of `03-Dec-2026` (`test_stability.cpp:49-50`,
  `:67`, `:88`) hold once C's pattern is set by the gallery's app document; until then E1 changes them to ISO.
- A new gallery test sets the start to `2027-01-31` and reads the first pull `28-Feb-2027 (40 °C, 1 m)`. **Control:**
  today's table answers `03-Dec-2026` whatever the start.

**Risks.**

- **A string that is a date is now a date to the date functions.** A document that held `yyyy-MM-dd` text and relied
  on `date(…)` refusing it gains a shown date where it had a report; none in the tree does.
- **Mixed comparisons stay a mismatch.** `lt` of a date and a timestamp compares a string with a timestamp
  (`expressions.md:89-92`), which is a reported mismatch, as today. Comparing a date with an instant needs a zone
  decision a document should make explicitly; open question 5.

## G55: a chart

**Problem (read).**

- Charts are not in the palette: "Not in the palette: themes beyond `TextRole`, images, charts, and translation
  catalogues" (`view_tree.md:546`).
- A `custom` node needs a component the client loads. `Backend::createCustom` returns null when the frontend was given
  no components (`src/qt_quick/backend.cpp:31-35`), and the generic client gives none
  (`FrontendConfig{… .components = nullptr …}`, `examples/client/main.cpp:233`). So a document served to the generic
  client always shows the fallback, and the terminal has no component mechanism at all. The mockup's own caption under
  the chart, "Drawn by a host-provided chart component" (`10-Console.dc.html`), names the route that does not reach
  the generic client or the terminal.
- The gallery's workaround (`controlChart()`, `console.cpp:541-573`) is a 7 × 20 grid of text cells, one `●` in the
  row nearest each run's SD: 147 widgets that show which rule broke, but no value, and no spacing between ±1 SD and the
  mean that a reader can judge.
- The same need is in stability (assay against time at each condition, with the specification limits) and in result
  review (a trend of a result across lots). They need several series, an axis proportional to time, and lines at
  limits. That shapes the node below more than the one Levey-Jennings chart does.

**Decision: a `chart` node in the palette, drawn by every frontend.**

- A palette node, not a component: it reaches the generic client, the terminal and `RecordingBackend`, and its
  semantics (what is plotted, what is marked, what is said) are the framework's, so they are the same everywhere.
- One kind of chart: points against a numeric axis, optionally joined, with horizontal reference lines. That covers a
  control chart, a trend and a stability plot. Bars, areas, pies and histograms stay out of ui/1 (Not in this batch).
- Everything the chart *says* is computed by the interpreter from exact values, as a `ChartData` the frontends draw
  (the fourth rule of the batch).
- A text alternative is required. A chart is the one node whose content is not text, so the document states what a
  reader must know from it.

**Vocabulary.** ⓘ marks an expression evaluated once per point, with `row` bound to the point's element, as a table's
`key` is (`document.md:180`).

| Key | Where | Shape | Meaning |
|---|---|---|---|
| `summary` | `chart`, required | expression, text | The text alternative: the accessible description, `textOf`, and the terminal's text when too small to plot |
| `series` | `chart`, required | 1 to 4 parts | The plotted series, in drawing order |
| `lines` | `chart` | 0 to 16 parts | Horizontal reference lines |
| `yMin`, `yMax` | `chart` | expression, number | Fixed ends of the value axis; absent, from the data |
| `yPlaces` | `chart` | literal integer, 0 to 18 | The places the axis's labels are shown at; absent, the widest places of the values and lines |
| `yLabel` | `chart` | expression, text | The axis's unit or name, drawn above the labels: `% LC` |
| `points` | series, required | expression, list | The elements, one point each |
| `y` ⓘ | series, required | expression, number or quantity | The value; `null` leaves a gap |
| `x` ⓘ | series | expression, number, timestamp or date | The position along the axis; absent on every series, points are evenly spaced by index |
| `xLabel` ⓘ | series | expression, text | The point's label on the horizontal axis |
| `tone` ⓘ | series | expression, `neutral`, `info`, `ok`, `warn`, `err` | Marks the point in that tone; `null`, an ordinary point |
| `note` ⓘ | series | expression, text | Text drawn beside the point: `+3.4SD, rule 1-3s` |
| `label` | series | expression, text | The series' name, in the legend; required when there are two or more series |
| `joined` | series | literal bool, default `true` | Join the points; `false` draws points alone |
| `value` | line, required | expression, number or quantity | Where the line is drawn |
| `label` | line | expression, text | Its label: at the axis when the lines label it (three or more), otherwise at the line's right end; absent, the value at `yPlaces` |
| `tone` | line | expression, a tone name, default `neutral` | Its colour |
| `style` | line | literal `solid` (default), `dashed`, `dotted` | Its stroke |

Design 10's chart, written with it:

```json
{ "kind": "chart", "id": "control", "testId": "chart",
  "summary": { "ref": "controlSummary" },
  "yLabel": "% LC", "yPlaces": 1,
  "series": [
    { "points": { "ref": "runs" },
      "y": { "row": "value" }, "xLabel": { "row": "run" },
      "tone": { "if": [ { "fn": "engaged", "args": [ { "row": "violation" } ] }, "err", null ] },
      "note": { "row": "violation" } } ],
  "lines": [
    { "value": { "ref": "limits.plus3" }, "label": "+3SD", "tone": "err", "style": "dashed" },
    { "value": { "ref": "limits.plus2" }, "label": "+2SD", "tone": "warn", "style": "dashed" },
    { "value": { "ref": "limits.plus1" }, "label": "+1SD", "style": "dotted" },
    { "value": { "ref": "limits.mean" }, "label": "mean" },
    { "value": { "ref": "limits.minus1" }, "label": "−1SD", "style": "dotted" },
    { "value": { "ref": "limits.minus2" }, "label": "−2SD", "tone": "warn", "style": "dashed" },
    { "value": { "ref": "limits.minus3" }, "label": "−3SD", "tone": "err", "style": "dashed" } ] }
```

and a stability trend, two conditions against months with the specification:

```json
{ "kind": "chart", "summary": { "ref": "trendSummary" }, "yLabel": "Assay, % LC",
  "series": [
    { "label": "25 °C / 60 % RH", "points": { "ref": "assay25" }, "x": { "row": "month" }, "y": { "row": "value" },
      "xLabel": { "fn": "format", "args": ["{0} m", { "row": "month" }] } },
    { "label": "40 °C / 75 % RH", "points": { "ref": "assay40" }, "x": { "row": "month" }, "y": { "row": "value" },
      "xLabel": { "fn": "format", "args": ["{0} m", { "row": "month" }] } } ],
  "lines": [ { "value": 95, "label": "LSL 95.0", "tone": "err", "style": "dashed" },
             { "value": 105, "label": "USL 105.0", "tone": "err", "style": "dashed" } ] }
```

**Semantics.** The lowering turns the node into one `ChartData`:

- **Values.** `y` and a line's `value` are numbers or quantities. Quantities convert to the unit of the chart's first
  non-null value, as `sum` converts (`expressions.md:121`); a value that does not convert, or is not a number, is left
  out and reported once for its path. A `null` `y` is a gap: the point is not drawn and the line does not join across
  it. A stability study with a missed pull draws that.
- **The horizontal axis.** With no `x` on any series, point *i* of every series sits at *i*, so series of equal length
  line up run by run. With `x`, the loader requires it on every series, and its values are all numbers or all instants
  and dates (a date is midnight of it in the display zone, as everywhere in this batch). A point whose `x` is `null` is
  left out; a mix of kinds is reported once and the chart falls back to the index.
- **The range.** The value axis spans every drawn value and every line, widened by 5 % of the span at each end, so the
  outermost line and point are inside the plot (design 10's run at +3.4 SD is drawn above the +3SD line, inside the
  plot; the mockup's SVG draws it outside its plot rectangle). A span of zero is widened to one unit in the last of
  `yPlaces` either side. `yMin` and `yMax` replace the computed ends. A value outside them is not drawn at the edge
  as if it were the edge's value: it is a clipped point, an open triangle at the edge pointing out of the plot (`△` or
  `▽` on the terminal), so a reader sees that the value is off the scale and not that it is the scale's end. It is
  reported nowhere, since a document that fixed the axis chose that; the summary carries the value.
- **Ticks.** A chart with three or more reference lines labels its axis with them: the lines are the scale a reader
  reads a control chart against (design 10), and `yTicks` is empty. A chart with fewer lines labels 3 to 6 ticks at a
  step of 1, 2 or 5 × 10ᵏ covering the range, computed exactly and shown at `max(yPlaces, the step's places)` through
  the active locale's separators, and draws each line's label at the line's right end, inside the plot: two
  specification lines alone would leave the values between them without a scale.
- **Labels.** Every text the chart draws (tick and line labels, `xLabel`, `note`, the legend, the summary) is
  formatted by the lowering through the session's `FormatContext`, so a decimal shows with the locale's separator and
  batch C's date pattern applies to a date in `xLabel`. An `xLabel` or `note` that is not text shows as `format`'s
  `{0}` shows it, so design 10's `{"row": "run"}` of an `int` needs no `format` call. A line's default label is its
  value at `yPlaces`.
- **Tone.** A point's `tone` draws it as a marked point: larger, filled in the tone's colour, and drawn with a shape no
  series uses (a cross on Qt Quick, `✕` on the terminal), so the mark does not rest on colour alone. A name outside the
  five is reported once and ignored.
- **Bounds.** A series draws at most its last 1000 points; past that, one report. A chart is a picture of a few
  hundred values; a larger set is a table's.
- **No interaction in ui/1.** A chart takes no focus and has no handler (open question 6).

**Validation.**

- Loader: `summary` and `series` present; `points` and `y` in each series; `label` on each series when there are two or
  more; `x` on every series or on none; `yPlaces` a literal 0 to 18; `style` a literal of the three; a literal `tone`
  one of the five; `yMin` not above `yMax` when both are literal; at most 4 series and 16 lines; a chart has no
  children. Each refusal names its path.
- Mount: a `points` that is neither a list nor `null` is reported once for its path and draws as empty. `null` draws as
  empty without a report: a query still loading.

**View tree.** `Chart{Prop<ChartData> data; Common common}`, and `view.hpp` gains:

```cpp
enum class ChartLineStyle : std::uint8_t { Solid, Dashed, Dotted };
enum class ChartClip : std::uint8_t { None, Below, Above };
struct ChartPoint  { double x; std::optional<double> y; ChartClip clip; std::string xLabel; std::optional<Tone> tone; std::string note; };
struct ChartSeries { std::string label; bool joined = true; std::vector<ChartPoint> points; };
struct ChartLine   { double value; std::string label; Tone tone = Tone::Neutral; ChartLineStyle style; };
struct ChartTick   { double value; std::string text; };
struct ChartData {
    double xMin, xMax, yMin, yMax;           // the plotted range, padding included
    std::string yLabel;
    std::vector<ChartTick> yTicks;           // empty when three or more lines label the axis
    std::vector<ChartLine> lines;
    std::vector<ChartSeries> series;
    std::string summary;
    bool operator==(ChartData const&) const = default;
};
```

each with complete Doxygen. One value, not a setter per part: the range depends on every point and line, so the
parts change together, and one equality check keeps an unchanged chart from redrawing. Positions are doubles, read
from the exact values as a `progress` value is (`interpreter.md:302-306`); text is never derived from them. On a time
axis `x` is the instant's milliseconds since the epoch, which a `double` holds exactly for every year 1 to 9999. A
clipped point's `y` is the edge it is drawn at.

**Lowering.** `lowerChart` builds one computed `ChartData` in the chart's scope. Inside the one tracked read it
evaluates `points` per series and each ⓘ expression per element with `row` bound, decorating as batch B's `sortBy` does
(B, G31), so a change to the list or to anything a point reads recomputes the chart once. No row scope is made per
point, so a point expression cannot read a row-scope name; the loader resolves names as for `key`, as B states for
`sortBy`. The range, the ticks and the labels are computed there in `math::Rational`, and only then converted to
`double`.

**Author.** `ui::chart(ChartOptions{.summary, .series, .lines, .yMin, .yMax, .yPlaces, .yLabel})`, with
`ChartSeries{.label, .points, .y, .x, .xLabel, .tone, .note, .joined}` (each ⓘ a function from the element's handle to an
expression, as `ui::map` takes, `author.hpp:1171-1193`) and `ReferenceLine{.value, .label, .tone, .style}`.

**Theme.** Four tokens join the closed set "with the components that read them" (`theme.md:143`): `color.chart.1` to
`color.chart.4`, the series colours in order. Like every colour row they are also roles of each surface's
`MorphScheme` (`theme.md:195-196`), so a chart on the `sidebar` surface reads that surface's values, and the
Theme-property table (`theme.md:150-161`) gains `chart1` to `chart4`. A reference line and a marked point use their
tone's colour (`color.err` and the others; `neutral` is `color.muted`).

- **Contrast.** A series is a graphical object, so each default holds 3:1 against `color.surface` (`#ffffff`) and
  `color.background` (`#f2f3f7`), WCAG 1.4.11. Okabe and Ito's set as published does not: computed, `#E69F00` is 2.25:1
  on white and 2.03:1 on the background, and `#CC79A7` 2.76:1 on the background. The defaults are their hues darkened
  until they pass: `#0072B2`, `#C25400`, `#00805E`, `#A8508A` (5.19, 4.60, 4.94 and 5.02:1 on white; 4.68, 4.15, 4.46
  and 4.53:1 on the background). A dark theme sets its own four, as it sets every colour; the defaults are for the
  light surfaces.
- **Not colour alone.** No four series colours stay apart from the four tone colours under colour-vision deficiency:
  computed (CIE76 ΔE), Okabe and Ito's blue is 20 from `color.info` and its orange 25 from `color.warn` with full
  vision, and under Machado's deutan simulation the blue is 13 from `color.info`. So on both frontends a series also has a shape (circle, square, triangle,
  diamond, in token order), a marked point has the cross no series uses, and a clipped point the open triangle. Two
  series, or a series and a marked point, never differ by colour alone (WCAG 1.4.1).

**Backend contract.** `IViewBackend::createChart(parent)` and `ChartWidget : Widget` with one pure virtual,
`setData(ChartData const&)`. The mount calls it at mount and on every change. A chart calls no handler.

**Per backend.**

- **Qt Quick.** `MorphChart.qml`, a `MorphWidget` holding a `Canvas` and plain `Text` items:
  - **No new dependency.** `Canvas` is part of `QtQuick`, which the frontend already links (`Qt6::Quick`,
    `src/qt_quick/CMakeLists.txt:81`). The probe drew a dashed line, a polyline and a filled circle and read them back
    through `grabToImage` on the offscreen platform. The default and the software scene graph gave the same colours;
    the software run:

    ```
    (200, 100) [(198, 40, 40, 255), (198, 40, 40, 255), (0, 0, 0, 0), (0, 0, 0, 0), (198, 40, 40, 255)] marker (21, 101, 192, 255) line (23, 120, 74, 255)
    ```

    The list is row 10 at x = 0, 3, 4, 6 and 7: the dash pattern 4 on, 3 off, in the line's colour, with nothing in the
    gap; then the marker's centre and a pixel on the polyline.

    Qt Graphs and Qt Charts are separate modules (a new package for every build and the WASM client), and
    `QtQuick.Shapes` is a separate QML module to deploy; `Canvas` needs neither, and a chart of a few hundred
    points draws in well under a frame.
  - `ChartImpl` writes the `ChartData` to the item as one `QVariantMap` of numbers and strings; the item calls
    `requestPaint()` when it changes or resizes.
  - Layout: the axis labels (the line labels, or the ticks) in a left gutter as wide as the widest label; the plot,
    with each line's label at its right end when the ticks label the axis; the `xLabel`s in one row below,
    left to right, each skipped when it would overlap the last one drawn, the last one always drawn (it is the newest
    run); the legend in a row below that when there are two or more series.
  - Drawing: lines first (dashes 4 px on 3 off, dots 2 on 3 off, 1 px), then each series as a 1.5 px polyline broken at
    gaps, then its points (the series' shape, 5 px across, series colour), then clipped points (an open triangle at
    the edge) and marked points (a cross 10 px across, 2 px stroke, tone colour) and their notes (`Theme.bodyFont`, the
    tone's text colour, beside the point and flipped to its left near the right edge). The legend draws each series'
    shape beside its label.
  - Accessibility: `Accessible.role: Accessible.Chart` (already in the role map, `widget.cpp:188`). The name is the
    node's `a11y` name, and otherwise the summary; `Accessible.description` is the summary only when the name is not,
    so a screen reader does not read the summary twice.
  - Content size: 40 by 8 units (`Theme.unitWidth`, the unit height), so a chart in a stretching column is wide and
    readable before any `sizing`.
- **Terminal.** `ChartImpl` in `display_widgets.cpp` plots in character cells:
  - The gutter holds the line labels when three or more lines label the axis, and the tick labels otherwise (at most
    8 columns); with ticks, a line's label is written at the right end of its row, over the line, the bottom row the `xLabel`s, thinned as on Qt
    Quick, and the legend row (two or more series) `● 25 °C  ■ 30 °C  ▲ 40 °C`.
  - A value is drawn on row `round((yMax − y) / (yMax − yMin) × (rows − 1))`, a position on column
    `round((x − xMin) / (xMax − xMin) × (columns − 1))`.
  - A reference line is a row of `─` (solid), `╌` (dashed) or `┄` (dotted) in its tone's colour, its label in the
    gutter. Two lines on one row draw the one of the more severe tone, and its label.
  - A point is its series' glyph (`●`, `■`, `▲`, `◆`) in the series colour; a marked point is `✕` in its tone's
    colour, with its note after it when it fits; a clipped point is `△` on the top row or `▽` on the bottom row. Points
    are not joined: a cell is twice as tall as it is wide, and box characters between cells would draw slopes that are
    not there. A point is drawn over a reference line in the same cell. Two points in one cell draw the marked one,
    else the later.
  - Smaller than 20 × 5 cells, the chart draws its summary, wrapped and muted, in place of the plot.
  - Content size: 40 columns by 8 rows, as on Qt Quick.
- **`RecordingBackend`.** Kind `Chart`; `setData` records `yRange` (`[lo,hi]`), `xRange`, `yLabel`, `yTicks`
  (`[text@value,…]`), `lines` (`[label@value;tone;style,…]`), `series` (one entry per series:
  `label;joined;[xLabel@x,y;tone;note …]`, `gap` for a `null` y, `y↑` or `y↓` for a clipped one) and `summary`, each
  as its own property and logged
  only when it changed. Doubles are written shortest-round-trip (`std::to_chars`), so a log is stable across
  platforms.
- **The probe.** `textOf(chart)` is the summary on every backend, as it is a text's text.

**Spec.**

- `document.md`: the palette gains `chart` with the keys above; `view_tree.md:546` drops charts from Out of scope, and
  its palette table gains `Chart`.
- `interpreter.md`: `lowerChart`, the range, the ticks and the reports.
- `backend_contract.md`: `ChartWidget`, `createChart`, the RecordingBackend properties, `textOf` for a chart,
  conformance case 54 and layout case L16.
- `theme.md`: `color.chart.1` to `.4`. `qt_quick/backend.md` and `tui/frontend.md`: how each draws it.
- `author.md`: `ui::chart`.

**Tests.**

- **Conformance case 54**, "a chart shows its current summary": `setData` with summary `A`, then with `B`;
  `textOf` reads `B`. Case 17 ("no setter calls a handler") gains `setData`. **Control:** a backend that reads the
  summary at creation only reads `A`. `conformanceCases()` goes from 53 (after D) to 54, and
  `test_ui_conformance.cpp`'s count with it.
- **Layout case L16**, "a chart's content size is 40 by 8 units": a content-sized chart in a 60-unit column measures
  40 × 8 on both frontends. **Control:** a `MorphChart` whose implicit size is its `Canvas`'s (0 × 0) measures 0.
  `layoutConformanceCases()` goes from 15 (after D) to 16.
- `tests/test_ui_interpreter_chart.cpp` (new), on `RecordingBackend`:
  1. Design 10's chart with 20 runs records the seven lines with their labels, `yTicks` empty, and the 20th point
     with `tone=err` and its note; `yRange` is `[98.672,101.488]`: the run at 101.36 and the −3SD line at 98.8, with
     5 % of the span on either side.
  2. An inner run's value (neither the highest nor the lowest) changed through state: one `set Chart#n series=…` log
     line, and no other property line.
  3. A `null` y records `gap`; a quantity series in `mg` with a line in `g` records the line at the converted value.
  4. No lines: ticks at a step of 1, 2 or 5 × 10ᵏ, three to six of them, at `yPlaces`; a German locale shows `99,5`.
  5. The stability trend above, with its two specification lines: `yTicks` holds three to six ticks, and `lines`
     records `LSL 95.0` and `USL 105.0`. **Control:** let any line replace the ticks, and `yTicks` is empty.
  6. A series with `x` as months spaces 0, 3 and 12 proportionally (`x` 0, 3, 12 recorded, range padded).
  7. A string in `y` is left out and reported once, however many times the chart recomputes.
  8. `yMax` 101 with design 10's runs: the 20th point records `y↑` at 101, not `101`.
  **Control:** compute the range from the points only, and case 1 records the ±3SD lines outside `yRange`; join
  across a gap, and case 3 records no `gap`; clamp without marking, and case 8 records a point at 101.
- `tests/ui/corpus/documents.json`: under `valid`, design 10's chart and the stability trend above; under `invalid`,
  each of these as one key broken in design 10's chart, with its `at` and `reason`: a chart without `summary`; two
  series without labels; `x` on one series of two; `style: "wavy"`; five series. **Control:** without the two valid
  cases every refusal would pass on a client that has no `chart` at all, refused as an unknown kind; with them, that
  client fails the valid cases, and the `at` of each refusal fails it too.
- `tests/qt_quick/test_qt_quick_chart.cpp` (new): grab a 400 × 200 chart with one dashed `err` line at the middle
  value, three points and the last marked `err`, at a scale factor of 1 and of 2. The line's row holds the `err` colour
  at a dash and not in the gap after it; the marked point's centre is the `err` colour; the accessible role is Chart
  and the description the summary. **Control:** skip drawing lines, and the line's row has no `err` pixel.
- `tests/tui/test_tui_chart.cpp` (new): the same chart at 30 × 8 cells, compared as text with a golden screen; at
  15 × 4 it draws the summary. The marked point's value sits more than half a row below a row boundary, so
  **Control:** draw points without the row formula's rounding (truncate), and the golden's marked point moves a row.
- `tests/test_ui_author.cpp`: `ui::chart` writes the JSON above; the palette screen gains a chart, and
  `tests/ui/golden/palette.json` is regenerated.

**Gallery.** `controlChart()` becomes a `chart` over the 20 runs the console holds as values (`kControlRuns` keeps
tenths of an SD today; it becomes `% LC` values from the stated mean 100.0 and SD 0.4), with the seven lines computed
from that mean and SD and the 20th run marked `err` with its rule as the note. The grid, its 140 cells, and the
hand-picked `●` rows go; the gallery's `a11y` description becomes the chart's `summary`; the violation line under it
stays, as the design draws it. `test_console.cpp:25` finds a `Chart` where it found a `Row`. On the terminal the
chart is `sizing` 12 rows tall, so the seven lines get rows of their own.

**Risks.**

- **Canvas on high-DPI screens (reproduced: not a risk on Qt 6.12).** `Canvas` paints at the device pixel ratio. A
  0.5 px black line at y = 10.25 in a 200 × 100 canvas, grabbed with `QT_SCALE_FACTOR=2` on the offscreen platform,
  fills exactly device row 20 (`[(20, 0)]`, red channel per row), on the default and the software scene graph alike;
  at a ratio of 1 the same line is a blend over rows 9 and 10 (`[(9, 224), (10, 159)]`). An image painted at the
  item's size and scaled up would blend over two device rows. The Qt test runs at both factors to keep it so.
- **Repaint cost.** A change repaints the whole canvas from the `QVariantMap`, at most 4 × 1000 points; the equality
  check on `ChartData` keeps a recomputation that changes nothing from repainting.
- **The terminal plot is coarse.** At the content height, seven plot rows place a value to within 1/12 of the range. That is the terminal; the
  summary carries what a reader must not miss, and a document that needs the values shows a table too.
- **One more node kind for every backend.** Four implementations (Qt Quick, terminal, `RecordingBackend`, the echoing
  test backend) gain a factory.

## Totals

- **Expressions (E1).** Two library defects fixed first (`walk`'s numeric segment; saturated rounding, now one
  checked rounding for every place `expr/1` rounds). One form (`get`), five functions (`abs`, `sqrt`, `stdev`,
  `rsd`, `dateAdd`), a mode argument on `round` and `decimal`, and the date functions reading `yyyy-MM-dd` and `""`
  through the reader the date input already uses. About 60 corpus cases, each with the control named beside it. A
  checked 128-bit integer with division and square root in `rational.hpp`, one implementation on every compiler,
  tested on its own.
- **The chart (E2).** One palette node, one view-tree kind and one `ChartWidget` with one setter; four `IViewBackend`
  implementations gain `createChart`. Conformance case 54 (53 to 54), layout case L16 (15 to 16; batch D takes L15), and case 17 covers
  `setData`. Four theme tokens, which are also scheme roles. A Qt Quick grab test at two scale factors and a terminal
  golden screen.
- **Gallery.** Removed: `kRsdFactor` and the two-preparation formula; the timepoint date table and `dateOf`; the
  `map`-then-`first` reads in stability and results; the 147-widget grid chart. Added: half-up reporting of the assay
  mean, compared with its limits as shown, pull dates that follow T0, and four tests (a third value counted by
  `rsd`, `99.5` reported half-up, `105.1` reported and marked out of specification, a T0 of 31 January).
- **Spec files.** `expressions.md`, `author.md`, `document.md`, `view_tree.md`, `interpreter.md`,
  `backend_contract.md`, `theme.md`, `qt_quick/backend.md`, `tui/frontend.md`.

## Not in this batch

- **`now` and date differences.** "Overdue", "days to expiry" and "age of sample" need the current time in an
  expression. `expr/1` is total and pure (`expressions.md:66-67`): a `now` would make every expression that reads it
  depend on a clock the reactive graph does not track, and would need a tick to recompute. A server sends `overdue` as
  data today (console's worklist rows). `dateDiff` without `now` has no screen that needs it; it joins with `now`, by
  the same rules as `dateAdd`.
- **Zone rules.** Wall-clock arithmetic across a DST change, and zone names, need tz data in the client (batch C keeps
  an offset for the same reason). Dates cover what the designs schedule.
- **Other charts.** Bars (dissolution profiles per vessel), histograms (a capability study), areas and pies. Each is a
  different drawing with different semantics for the axis; none of the ten designs draws one. A `chart` with a `type`
  key can take them later without changing the line chart's keys.
- **Chart interaction.** Hover tooltips, a point that opens its run, zoom. Open question 6.
- **Statistics beyond SD and RSD.** Regression for a stability shelf-life estimate (ICH Q1E), confidence intervals,
  Westgard rule evaluation. They are a server's computations over a study, and their results reach the screen as
  data; the chart draws a regression line a server computed as a series.

## Open questions

All eight are decided; the design above follows each decision.

1. **`get` with a computed path.** Should `path` accept an expression, for a column a user picks? **Decided:** no.
   No screen asks, and a literal path keeps reads checkable and the form total.
2. **An application-wide rounding mode**, as batch C sets the date pattern in the app document. A lab rounds one way
   everywhere, and repeating `"halfUp"` at every call is what C's G13 avoided for dates. **Decided:** no, keep it
   per call. A date pattern changes how a value looks; a rounding mode changes the value a screen compares and
   submits, and the same document must compute the same value on every client. The author API's `Rounding` argument
   keeps C++ screens to one constant. Revisit if a second screen repeats the mode on every call.
3. **`mean` with a mode**, or an exact `mean`. `mean`'s half-even rounding at the elements' places is a double rounding
   for a pharmacopoeial report. **Decided:** leave `mean` as it is, document `div(sum(xs), count(xs))`, and add a
   mode only if a screen needs a rounded mean in the pharmacopoeial way at the elements' own places.
4. **`rsd` in percent, or a ratio.** **Decided:** percent. Every LIMS screen, pharmacopoeia and SOP states RSD in
   percent, and a ratio would make every document multiply by 100.
5. **Comparing a date with an instant.** `lt("2026-11-03", t)` is a mismatch today and stays one. **Decided:**
   keep it a mismatch; a document converts explicitly, and a later `startOfDay(t)` returning a date would give it the
   words to.
6. **Chart interaction.** A Levey-Jennings point that opens its run is a natural next step. It needs a keyboard model
   (arrows between points, Enter to activate) on both frontends and a conformance case. **Decided:** out of
   ui/1's first chart; the document puts a table or a list of runs beside the chart, which already has activation.
7. **Ticks when there are lines.** Design 10 labels its axis by its seven control lines; a stability trend has only
   two specification lines. **Decided:** the lines replace the ticks only when there are three or more; with fewer,
   the chart draws ticks and writes each line's label at its right end (Ticks, above; interpreter test 5).
8. **The series palette.** **Decided:** `#0072B2`, `#C25400`, `#00805E`, `#A8508A`, Okabe and Ito's hues darkened to
   hold 3:1 on both light surfaces, as the defaults of `color.chart.1` to `.4` and as roles of each surface's
   `MorphScheme`, overridable like every token; each series also has its own marker shape, so the distinction never
   rests on colour (Theme, above).

## Defects found outside this batch

Library defects (`include/morph`, `src`), found while checking this batch's premises. Not fixed here; each is for
filing. The two found in `expr/1` itself are E1's first items (walk's numeric segment, saturated rounding), and the
date functions' refusal of `yyyy-MM-dd` is G37's.

1. **The forms engine's decimal text rounds through the saturating path (read, not reproduced).**
   `forms::detail::engine::decimalText` (`forms/engine/detail/text.hpp:374-377`) calls `roundToDecimalPlaces`, whose
   scale-up saturates (`rational.hpp:1396`), and turns the clamped result into text. The expression evaluator's copy
   of the same function shows `9.223372036854775807` for `10/3` at 18 places (saturated rounding, above); this one has
   the same shape. Not run: whether a form field can hold an exact quotient at that many places was not checked.
   **Would close it:** a probe or test that formats `10/3` at 18 places through it and gets either the exact digits
   or a refusal, never `9.223372036854775807`.
2. **Two tone colours fall below 3:1 on the sidebar surface (computed; weak).** The `sidebar` scheme's surface is
   `#1f2430` and it inherits the theme's tones (`Theme.qml:232-243`): `error` `#c62828` is 2.76:1 and `info`
   `#1565c0` 2.70:1 there, below WCAG 1.4.11's 3:1 for a dot or an outline. Not checked: whether any shipped
   component draws a tone on that surface today. **Would close it:** the sidebar scheme sets tones that reach 3:1 on
   its surface, or no component draws a tone there.
