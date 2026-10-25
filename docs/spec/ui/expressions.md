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
with `fallback` and `args`; `map` with `as` and `to`; `filter` with `as` and `where`; `sort` with `as`, `by` and `order`). An array is
not an expression — a list is `{"list": [...]}` or `{"lit": [...]}` — and a number with a fraction
is refused: an exact value is `{"num", "den", "dp"}`. Every node carries its JSON Pointer, which
refusals and diagnostics name.

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
- **Rounding** is half to even wherever `expr/1` rounds: `round`, `mean`, and every display.
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
  timestamps and strings (by bytes); anything else is a mismatch.
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
| Arithmetic | `add`, `sub`, `mul`, `div`, `neg`, `round(x, dp)` |
| Optional | `engaged(x)` (never `null`), `coalesce(x, ...)` (the first non-`null`) |
| Strings | `concat(s, ...)`, `format(template, args...)`, `trim`, `length` (code points), `isEmpty` (a string or a list), `contains` |
| Lists | `count`, `sum` (0 for an empty list), `mean`, `min`, `max`, `any`, `all`, `first`, `includes`, `indexOf`, `concatLists(list, ...)` |
| Display | `number`, `decimal(x, dp)`, `quantity(x, unit?)`, `money(x, currency)`, `date(x, style?)`, `dateTime(x, style?)` |
| Entry | `parseDecimal(text, dp?)` |
| Text | `t(key, fallback, args?)` |

- `format`'s placeholders are `{0}`, `{1}`, …; `{{` and `}}` are literal braces. An argument shows
  as text, a bool as `true`/`false`, a number or quantity as `number` and `quantity` show it, a
  timestamp as `dateTime`.
- `sum` and `mean` take numbers, or quantities in one dimension (converted to the first element's
  unit); `mean` is the exact sum over the count, rounded half to even at the elements' widest places.
  `min` and `max` return the element itself.
- `any` and `all` are Kleene over the elements: `any([])` is `false`, `all([])` is `true`.
- `indexOf` is `null` when the value is absent; `includes` is `false`.
- `concatLists` takes one to 255 lists and answers their elements in order. Each argument must be a
  list where its type is known; the static type is `list<T>` when every known element type is `T`,
  else `list<any>`. A `null` argument makes the result `null`, as every strict function's does (a
  list that is still loading reads `coalesce(q, [])`), and any other value that is not a list is a
  reported mismatch. `concat` stays strings-only.
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
- `date` and `dateTime` take a timestamp or ISO-8601 text, shift it to the display zone and format
  it in a style (`short`, `medium`, the default, or `long`) through the `DateFormatter`; without one
  they write `2026-10-11` and `2026-10-11 08:30` (`long` adds seconds).

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
hold reads `null`. `walk(value, path)` reads a path out of a value. `StaticEnvironment` holds its
values in public members and records diagnostics once per path.

## Design decisions

- **Parse once, check once, evaluate often.** Parsing and checking happen at load; evaluation, on
  every reactive read, walks a tree of shared, immutable nodes and allocates only what it returns.
- **Coarse static types.** A document does not restate action schemas, so the checker cannot type
  most data. It types what the document itself writes, and that is where its errors are.
- **Messages are a subset of a standard.** MessageFormat 2 is what catalogues and translators
  already use; the subset is what a UI needs, and a pattern outside it is refused rather than half
  understood.
