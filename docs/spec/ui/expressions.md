# Expressions (`expr/1`) — design

Design spec for the expression language of the UI document (`include/morph/ui/expression.hpp`) and
the MessageFormat 2 subset its `t` reads (`include/morph/ui/message.hpp`). Every bound property,
query body, condition and command argument of a document is an expression: a JSON tree that
`parseExpression` reads into an immutable `ui::Expr`, `checkExpression` checks against the
document's names, and `evaluate` evaluates against an `Environment` — the interpreter's reactive
reads, or a `StaticEnvironment` of plain values.

Read this before adding a form or a function. A new form or function joins the current version,
`expr/1`, and a document that uses one needs a client that has it: an older client refuses the
document at load, since an unknown form or function is a load error, so a server that serves older
clients keeps such documents free of it. The shared corpus is
`tests/ui/corpus/expressions.json`, and the message corpus `tests/ui/corpus/messages.json`.

## Contents

- [Forms](#forms)
- [Semantics](#semantics)
- [Static checking](#static-checking)
- [Functions](#functions)
- [Formatting](#formatting)
- [Messages](#messages)
- [The environment](#the-environment)
- [Design decisions](#design-decisions)

## Forms

| Form | `ExprKind` | Reads |
|---|---|---|
| A JSON scalar, `{"lit": v}` | `Literal` | The constant; `lit` wraps a list or a record |
| `{"ref": "name.path"}` | `Ref` | A name, then a path (record fields by name, list elements by index) |
| `{"row": "path"}` | `Row` | A field of the innermost row; `""` is the whole row |
| `{"get": e, "path": "a.b"}` | `Get` | A path read out of any expression's value, as `ref` reads below its name |
| `{"query": "q"}` | `Query` | A query's value |
| `{"pending": "x"}`, `{"error": "x"}`, `{"errorKind": "x"}` | `Pending`, `Error`, `ErrorKind` | A query's or mutation's flight, error text and error kind |
| `{"result": "m"}` | `Result` | A mutation's last result |
| `{"can": "x"}` | `Can` | Whether the principal may run a query or mutation, or the client has `host:<command>` |
| `{"pref": "point.path"}` | `Pref` | A customization point's choice |
| `{"fn": "name", "args": [...]}` | `Call` | A library function |
| `{"t": "key", "fallback": "text", "args": {...}}` | `Call` (`t`) | The translated message |
| `{"if": [c, a, b]}` | `If` | |
| `{"obj": {...}}`, `{"list": [...]}` | `Object`, `List` | Constructors |
| `{"map": e, "as": "x", "to": e}`, `{"filter": e, "as": "x", "where": e}` | `Map`, `Filter` | Comprehensions over a list |
| `{"runs": e, "as": "x", "by": e}` | `Runs` | The list split into runs of adjacent elements whose `by` values are equal, each a record `{key, count, first, last, items}` |
| `{"sort": e, "as": "x", "by": e, "order": e}` | `Sort` | The list ordered by the value `by` gives each element; `order` is `"asc"` (also when absent or `null`) or `"desc"` |
| `{"env": "name"}` | `Env` | `widthClass`, `locale`, `platform`, `online`, `backend` (see below) |
| `{"session": "name"}` | `Session` | `principal`, `userName`, `displayName`, `userKey`, `tenant`, `authenticated` |
| `{"host": "name"}` | `Host` | A value the application registered, `null` when absent |

`widthClass` is the width of the surface the client draws on, in the unit `fixed(n)` counts: `compact` below 80
units, `medium` below 160, `expanded` otherwise (`ui::widthClassOf`). The client sets it from its view backend before
the first mount and updates it on a resize. The terminal counts its columns, so an 80-column terminal is `medium`; Qt
Quick counts the window's width in `Theme.unitWidth`s. A document's breakpoints therefore sit at the same number of
its own units on both frontends.

An expression object has one form key and only that form's companion keys (`fn` with `args`; `t`
with `fallback` and `args`; `map` with `as` and `to`; `filter` with `as` and `where`; `sort` with
`as`, `by` and `order`; `runs` with `as` and `by`; `get` with `path`). An array is not an expression
— a list is `{"list": [...]}` or `{"lit": [...]}` — and a number with a fraction is refused: an exact
value is `{"num", "den", "dp"}`. Every node carries its JSON Pointer, which refusals and diagnostics
name.

`get` reads its path with `walk` (see [The environment](#the-environment)), the reader `ref` and
`row` use, so `{"get": e, "path": p}` means what a `ref` means after its head and the two never
disagree on a path: `first(xs)`'s `lot.stock` is `{"get": {"fn": "first", ...}, "path":
"lot.stock"}`. The path is a literal, non-empty string in `ref`'s dotted syntax, which keeps every
read a document makes checkable; a path computed at run time is not in `expr/1`. `get` of `null`, a
missing field, and a path through a scalar are `null` without a report. Its static type is `any`;
an operand whose type is known to be neither a record nor a list is refused at load.

**Size bound.** An expression of more than 256 nodes is refused (`kMaxExpressionNodes`). A node is
one JSON expression: a literal, however large its value, is one node. The parser counts as it
descends. The text underneath is bounded first: `parseJson` refuses JSON nested deeper than 64
objects and arrays (`kMaxJsonDepth`) before the JSON parser runs, because that parser and every pass
over the tree recurse, and a few hundred levels exhaust a 1 MB thread stack.

## Semantics

- **Total.** `evaluate` always returns. There is no recursion, no user function, and the only
  loop is a comprehension over a list the data already holds.
- **Absence propagates.** A function whose required argument is `null` returns `null` without
  looking further. `if` with a `null` condition is `null`. `engaged`, `coalesce`, `and`, `or` and
  `any`/`all` over elements are the exceptions their definitions give.
- **Three-valued logic.** `and`, `or` and `not` are Kleene's: `false` dominates `and` and `true`
  dominates `or`, whatever else is `null`.
- **Errors are values.** A type mismatch, an overflow, a division by zero or units that do not
  convert yields `null` and is reported through `Environment::report` with the failing node's path.
  `StaticEnvironment` keeps each path once.
- **Exact arithmetic.** Numbers are `math::Rational`, through its checked operations. The result of
  `add`, `sub` and `mul` of two `int`s is an `int`; with a decimal, a decimal at the wider of the
  operands' places. `div` keeps the exact quotient: an `int` when two `int`s divide, otherwise a
  decimal at the operands' places, which display rounds — so a document shows a quotient with
  `decimal(x, dp)`.
- **Rounding** is half to even wherever `expr/1` rounds, unless the call names `"halfUp"`, which is
  half away from zero: `round(0.125, 2)` is `0.12`, `round(0.125, 2, "halfUp")` is `0.13` and
  `round(-0.125, 2, "halfUp")` is `-0.13`, as the pharmacopoeial rule (USP General Notices 7.20,
  Ph. Eur. 1.2) and `HALF_UP` round a magnitude. The mode is the last, optional argument of
  `round`, `decimal`, `sqrt`, `stdev` and `rsd`; it is the document's word at the call, not a client setting, because a
  rounded value is data a screen compares with a limit and submits. A literal mode other than
  `"halfEven"` or `"halfUp"` is refused at load; a computed one is a reported mismatch, and a
  `null` mode makes the result `null` like any absent argument. `mean`, `number`, `quantity`,
  `money` and `format` round half to even. Every rounding is one checked rounding,
  `math::checkedRoundToDecimalPlaces`: the value is scaled by `10^dp` and divided in 128 bits, so
  it never saturates, and its result is empty only when the rounded value's numerator does not fit
  an `int64`. `10/3` at 18 places is `3.333333333333333333`;
  `28/3` at 18 places is not representable, and every function that would round it (`round`,
  `mean`, `decimal`, `number`, `quantity`, `money`, `format`) is `null`, reported "the result
  overflows", rather than showing a clamped value. In a message the placeholder shows as `{$name}`
  and the error is returned, and such a number matches only a `.match`'s `*`.
- **Quantities.** Two quantities add, subtract and compare when their units convert; the result is
  in the left operand's unit. A quantity times or over a plain number keeps its unit; a quantity over
  a quantity is a decimal ratio; a product of two quantities, or a sum of a quantity and a plain
  number, is a mismatch. Conversion uses the client's `UnitTable` — each unit's canonical unit and
  its exact factor, from `x-unitAlternatives` — and a unit missing from the table is its own
  canonical unit. A converted value is exact; its places widen by the decimal places of the
  conversion ratio, so 1500 mg converts to 1.500 g, not 2 g.
- **Equality.** `eq` and `ne` compare numbers by value (an `int` and a decimal; quantities across
  units), lists and records element by element, and anything else by kind and value; values of
  kinds that cannot compare are unequal. `lt`, `le`, `gt` and `ge` order numbers, quantities,
  timestamps and strings (by bytes); anything else is a mismatch. Two `yyyy-MM-dd` dates order
  correctly as strings; ISO date-time text orders correctly only against text in the same spelling
  (`…:00Z` sorts after `…:00.500Z`), so a document that orders instants holds timestamps. A date
  and a timestamp do not compare: relating a calendar day to an instant needs a zone, which a
  document states by converting one of them.
- **Sorting.** `sort`, and a table's sort, order values as `lt` does, except strings: those compare
  case-folded as the table engine's default collator folds them (`table::CodePointCollator`: ASCII
  `A`-`Z` and Latin-1 `À`-`Þ` but `×` lowered, other bytes as they are), and by their own bytes when
  they fold the same, so `Paracetamol` comes before `paracetamol` whatever the source order. `null`
  sorts last in both directions. The sort is stable, and descending reverses the comparison, not the
  list, so equal values keep their source order either way; when every value is `null` the list
  keeps its order. Values `lt` cannot order (a string against a number) are a mismatch: `sort`
  yields `null` and reports it. `order` other than `"asc"` or `"desc"` is a mismatch too, and a
  `null` list yields `null`. `by` reads the element as `x`; `order` is read once, outside it. Folding
  covers ASCII and Latin-1 case only, not accents or other scripts, as the engine's collator does.
- **Runs.** `runs` splits a list into runs of adjacent elements whose `by` values are equal, in the
  list's order: `eq`'s equality, except that `null` equals `null`, so the elements without the value
  make one run. Each run is `{"key": <the shared value>, "count": n, "first": <element>, "last":
  <element>, "items": [...]}`, and a table's group heading reads the same record. An empty list gives
  `[]` and a `null` list `null`; a `by` that fails for an element fails the form, which yields `null`.
  Elements that share a value but are not adjacent make separate runs; sort first to group them all.
- **Dates and instants** are two values. A **date** is `yyyy-MM-dd` text, a calendar day with no
  time of day and no zone: a pull date or an expiry date is the same day in Basel and in Boston. An
  **instant** is a timestamp, or ISO date-time text that `DateTime::fromIso8601` reads. `""` in any
  date function is absence, `null` without a report: it is what a cleared date input writes. Other
  text is a mismatch, and so is `yyyy-MM-dd` text that names no day (`2026-02-30`). The date
  functions and a date input read a value through one reader, `readDateValue`.
- **`dateAdd(x, n, unit)`** moves a date or an instant by `n` whole units, an `int`, negative to go
  back: `days`, `weeks`, `months`, `years`, `hours` or `minutes`. The result keeps the value's
  form: a date gives a date, a timestamp a timestamp, and ISO text the instant as
  `DateTime::toIso8601` writes it (`yyyy-MM-ddTHH:MM:SS.mmmZ`, what a date input in `date-time`
  mode writes).
  - Days and weeks move a date's calendar day, and add 24 and 168 hours to an instant, which is the
    same at a fixed offset. Hours and minutes add exact durations to an instant; on a date they are
    a mismatch ("a date has no time of day").
  - Months and years move the calendar month and keep the day, clamped to the month's last day:
    `2026-01-31` plus one month is `2026-02-28`, and `2028-02-29` plus one year is `2029-02-28`. A
    year is twelve months. On an instant the calendar is the one at the display offset: the instant
    is shifted by `utcOffsetMinutes`, its civil date moved, and the offset taken off again, so the
    wall-clock time at that offset is kept. The client holds an offset, not zone rules, so a month
    added to `09:00 +01:00` in January is `09:00 +01:00` in July; a document that means a calendar
    day holds a date, which no offset or daylight saving time can move.
  - Clamping is not associative: `2026-01-31` plus one month, plus one month again, is
    `2026-03-28`, while plus two months is `2026-03-31`. A schedule adds each timepoint to its
    start, never to the previous one. This is the convention of `java.time`'s `plusMonths` and
    .NET's `AddMonths`, not JavaScript's, which would put a month after 31 January on 3 March.
  - A result outside years 1 to 9999, which ISO text cannot write, is `null`, reported "the date is
    out of range". `n` is compared with the span of those years in its unit before any calendar
    arithmetic, whose day and month counts are narrower than an `int`, so a huge `n` is reported
    and never narrowed into a date.
  - A literal `unit` other than the six, and an `n` whose type is known and is not `int`, are
    refused at load. The static type is the first argument's (`timestamp` or `string`), else `any`.

## Static checking

`checkExpression(expr, resolver)` walks the tree once. Every name is resolved through the
`NameResolver` the document supplies (comprehension variables first); it decides what each name
means and returns its type or why it does not resolve (`PermissiveResolver` accepts everything as
`any`, for expressions outside a document). A function's arguments are checked against the kinds it
takes when the argument's type is known; the condition of `if` and the `where` of `filter` must be
`bool` when known; a `sort`'s `order` must be a string when known; a literal `t` fallback must be in the message
subset. The result is the
expression's static type: precise for literals, constructors, comparisons, formatting and integer
arithmetic, and `any` where it depends on data the document does not describe.

## Functions

| Group | Functions |
|---|---|
| Logic, comparison | `and`, `or` (one or more), `not`, `eq`, `ne`, `lt`, `le`, `gt`, `ge` |
| Arithmetic | `add`, `sub`, `mul`, `div`, `neg`, `abs`, `round(x, dp, mode?)`, `sqrt(x, dp, mode?)` |
| Optional | `engaged(x)` (never `null`), `coalesce(x, ...)` (the first non-`null`) |
| Strings | `concat(s, ...)`, `format(template, args...)`, `trim`, `length` (code points), `isEmpty` (a string or a list), `contains` |
| Lists | `count`, `sum` (0 for an empty list), `mean`, `min`, `max`, `stdev(list, dp, mode?)` (sample, n − 1), `rsd(list, dp, mode?)` (in percent), `any`, `all`, `first`, `includes`, `indexOf`, `concatLists(list, ...)`, `slice(list, start, count?)` |
| Dates | `dateAdd(x, n, unit)` |
| Display | `number`, `decimal(x, dp, mode?)`, `quantity(x, unit?)`, `money(x, currency)`, `date(x, style?)`, `dateTime(x, style?)` |
| Entry | `parseDecimal(text, dp?)` |
| Text | `t(key, fallback, args?)` |

- `format`'s placeholders are `{0}`, `{1}`, …; `{{` and `}}` are literal braces. An argument shows
  as text, a bool as `true`/`false`, a number or quantity as `number` and `quantity` show it, a
  timestamp as `dateTime`.
- `sum` and `mean` take numbers, or quantities in one dimension (converted to the first element's
  unit); `mean` is the exact sum over the count, rounded half to even at the elements' widest places.
  A result reported to a rule that rounds once from the unrounded value, as the pharmacopoeias'
  does, is the exact mean `div(sum(xs), count(xs))` rounded once with `round(…, dp, "halfUp")`:
  `decimal(mean(xs), 1, "halfUp")` rounds twice, and for `99.6` and `99.3` shows `99.4` where the
  exact `99.45` reports as `99.5`.
  `min` and `max` return the element itself.
- `abs(x)` is the magnitude of a number or quantity, of the same kind, places and unit; `abs` of the
  most negative `int` overflows, as `neg` does.
- `sqrt`, `stdev` and `rsd` are **exact until one correctly rounded result**: of the decimals at `dp`
  places (0 to 18), the one nearest the exact real result, and on an exact tie the one `mode` picks
  (the rounding modes above). The result is always a decimal at `dp` places, so `sqrt(4, 0)` is the
  decimal `2`. A root is never computed in floating point and then rounded, since that loses ties:
  `√0.2025` is exactly `0.45`, whose nearest `double` is above it.
  - `sqrt(x, dp, mode?)` takes a plain number of 0 or more. A negative number is `null`, reported
    "the square root of a negative number"; a quantity is a mismatch, since `expr/1` has no unit to
    the half power.
  - `stdev(list, dp, mode?)` is the sample standard deviation, dividing by `n − 1` as USP <1010>,
    Ph. Eur. 2.2.46 and USP <621> do for a set of results and replicate injections:
    `√((n·Σx² − (Σx)²) / (n·(n − 1)))`. `rsd(list, dp, mode?)` is `100 × SD / |mean|` in percent,
    `√(10⁴ · n · (n·Σx² − (Σx)²) / ((n − 1) · (Σx)²))`, a plain decimal. Neither rounds the mean
    or the variance first; the one-pass formula is safe because exact arithmetic has no
    cancellation. They take numbers, or quantities in one dimension converted to the first
    element's unit, as `sum` does: `stdev` of quantities is a quantity in that unit, `rsd` of
    quantities a plain decimal. Fewer than two values, or a `null` element, is `null` without a
    report (a worksheet holds one preparation while the second is typed); `rsd` of values whose
    sum is zero is `null`, reported; a negative mean counts by its magnitude.
  - **Width.** The elements are put over one denominator `D`, the least common multiple of their
    denominators (a decimal's divides its `10^places`, an exact quotient such as `div(1, 3)` has
    its own), as integers `X = x · D`, and the sums are computed in a checked 128-bit integer. The
    root is of `M = A / B` with, for `stdev`, `A = (n·ΣX² − (ΣX)²) · 10^(2·dp)` and
    `B = n·(n − 1)·D²`, and for `rsd` `A = 10^(4 + 2·dp) · n · (n·ΣX² − (ΣX)²)` and
    `B = (n − 1)·(ΣX)²`. The result is exact whenever `D`, each `X`, `A`, `B` and every
    intermediate fit (`D` in 63 bits, `X` in 64, the rest in 128) and the result's numerator fits
    an `int64`; past that it is `null`, reported "the statistics overflow". In the worst case for ten
    thousand values of seven significant digits, bounding `n·ΣX² − (ΣX)²` by `n·ΣX² ≤ 10²²`, `A`
    fits for `stdev` to 8 places and for `rsd` to 4; real data, whose spread is small beside its
    magnitude, sits far inside.
  - **The root.** With `F = ⌊A / B⌋` and `q = ⌊√F⌋`, which is `⌊√M⌋`, the result is `q / 10^dp`,
    raised by one unit in the last place when `M > (q + ½)²`, or when `M = (q + ½)²` and the mode is
    `halfUp` or `q` is odd. The comparison reads the remainder of `A / B`, not `F` alone: `F` against
    `(q + ½)²` never ties and misses every value just above one, so `sqrt(6.3, 0)` would be `2`
    instead of `3`.
- `any` and `all` are Kleene over the elements: `any([])` is `false`, `all([])` is `true`.
- `indexOf` is `null` when the value is absent; `includes` is `false`.
- `concatLists` takes one to 255 lists and answers their elements in order. Each argument must be a
  list where its type is known; the static type is `list<T>` when every known element type is `T`,
  else `list<any>`. A `null` argument makes the result `null`, as every strict function's does (a
  list that is still loading reads `coalesce(q, [])`), and any other value that is not a list is a
  reported mismatch. `concat` stays strings-only.
- `slice` answers `count` elements of the list from `start`, or every element from `start` without a
  count; a start past the end answers `[]`. The list is checked as `concatLists`' are, and `start` and
  `count` as integers, where their types are known; a negative one, or one that is not an integer, is
  a reported mismatch. A `null` argument makes the result `null`.
- `parseDecimal` reads text typed in the active locale (its separators, sign and digits, through
  `render::parseDecimal`) as an exact decimal, at `dp` places when given and otherwise at the places
  typed. Text that does not spell a number, or has more fraction digits than `dp`, is `null` and not
  an error: it is what an entry holds while it is being typed. It is how a text input's `event`
  becomes a number a body can carry.

## Formatting

`FormatContext` carries the active locale, its `render::NumericLocale` (separators, sign, digits),
the `render::TranslationProvider`, the `UnitTable`, the display zone's offset and an optional
`DateFormatter`.

- `number(x)` shows an `int`, or a decimal at its own places; `decimal(x, dp)` at `dp` places;
  `quantity(x, unit?)` the magnitude and the unit's display text, converted first when a unit is
  given; `money(x, currency)` at the currency's minor digits (0 for `JPY` and the like, 3 for `KWD`
  and the like, otherwise 2) followed by the code. All go through `render::formatCanonicalNumber`.
- `date` and `dateTime` take a date, a timestamp or ISO-8601 date-time text, and format it in a
  style (`short`, `medium`, the default, or `long`) through the `DateFormatter`; without one they
  write `2026-10-11` and `2026-10-11 08:30` (`long` adds seconds). An instant is shifted to the
  display zone first. A date is that calendar day at any offset, and `dateTime` shows it as
  midnight of that day in the display zone, as a date input does. `""` is `null` without a
  report.

## Messages

`t(key, fallback, args)` asks the `TranslationProvider` for `(key, locale)`; the catalogue's
pattern is used when it parses, and otherwise the fallback is, with the refusal reported. A
fallback that does not parse is shown as its raw text, reported.

**The subset** (`parseMessage`) is MessageFormat 2's: text with `\` escaping `\`, `{`, `}` and `|`;
placeholders `{$name}`, `{$name :number}`, `{$name :string}`; `.input {$name :function}`
declarations; and `.match` on declared `:number` or `:string` variables, each variant a key per
selector — a plural category, an exact number (`0` or `=0`), an exact or `|quoted|` string, or `*` —
and a quoted pattern `{{...}}`. A `.match` needs a catch-all variant. Literal placeholders (`{0}`,
`{|x|}`), options, markup, other functions and `.local` are outside the subset and refuse the
pattern.

**Selection** (`formatMessage`) picks the variant whose keys match best, selector by selector: an
exact key before a plural category before `*`. A variable that is missing, `null` or of the wrong
kind shows as `{$name}` and is returned as an error, which `t` reports.

**Plural categories** (`pluralCategory`) are CLDR's, from the operands `n`, `i` and `v` (a
decimal's places are its visible fraction digits): `en`, `de`, `nl`, `sv`, `it`, `es`, `fr`, `pt`,
`pl`, `ru`, `uk`, `cs` and `ar` carry their rules; every other language selects only `other`. The
language subtag decides, so `pl-PL` uses Polish.

## The environment

`Environment` is what the interpreter implements: `ref`, `row`, `control` (the query and mutation
forms), `pref`, `client` (`env`, `session`, `host`), `formatting` and `report`. A name it does not
hold reads `null`. `walk(value, path)` reads a path out of a value: a record's field by name, a
list's element by a decimal index. A segment that names nothing reads `null`, with no report: a
missing field, a segment of a list that is not all digits, and an index past the end, including
one too large for any list to reach, which is never wrapped onto an element that exists.
`StaticEnvironment` holds its values in public members and records diagnostics once per path.

## Design decisions

- **Parse once, check once, evaluate often.** Parsing and checking happen at load; evaluation, on
  every reactive read, walks a tree of shared, immutable nodes and allocates only what it returns.
- **Coarse static types.** A document does not restate action schemas, so the checker cannot type
  most data. It types what the document itself writes, and that is where its errors are.
- **Messages are a subset of a standard.** MessageFormat 2 is what catalogues and translators
  already use; the subset is what a UI needs, and a pattern outside it is refused rather than half
  understood.
