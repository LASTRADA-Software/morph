# Expressions — design

Design spec for `include/morph/ui/expression.hpp` (values, types, the `expr/1` tree, its evaluator and function
library) and `include/morph/ui/message.hpp` (the MessageFormat 2 subset behind `t`). The language itself is
spec 5 §4 (`docs/superpowers/specs/2026-10-07-ui-document-design.md`); this page is how morph implements it.
The document that holds expressions is [`document.md`](document.md).

## Contents

- [Values](#values)
- [Types](#types)
- [JSON](#json)
- [The tree](#the-tree)
- [Evaluation](#evaluation)
- [The function library](#the-function-library)
- [Units](#units)
- [Display](#display)
- [Messages](#messages)
- [The corpus](#the-corpus)
- [Design decisions](#design-decisions)

## Values

`ui::Value` is null, a bool, an `int64`, a UTF-8 string, a decimal (`math::Rational` with its declared places), a
quantity (`QuantityValue`: a decimal and a unit id), a timestamp (`time::DateTime`, UTC), a list or a record
(fields in declaration order). A value is immutable and cheap to copy: lists and records are shared.

Equality is deep. A decimal compares by value, as `Rational` does, so `1/2` at one place equals `1/2` at two.

## Types

`ui::Type` is one of spec 5 §3's types: `bool`, `int`, `string`, `decimal`, `quantity`, `timestamp`, `key`,
`list<T>`, a record `{name: T, ...}`, `T?`, and `any` for a type not known. `parseType` reads the written form,
and `Type::text` writes it back. `typeFromSchema` reads a type from a served JSON Schema: `$ref` into `$defs`,
`type: [T, "null"]` as optional, an object of `num`, `den` and `dp` (and `unit`) as a decimal (a quantity),
`format: date-time` as a timestamp, a member outside `required` as optional, and `x-decimalPlaces` as the places.

`fits(from, into)`: the same kind (an `int` or a `string` is a `key`, an `int` is a `decimal`), a record when it has
every field the target requires, and `null` only into an optional type. `any` fits either way.

## JSON

A document's JSON is read as `glz::generic_u64`, so an integer is never rounded through a double.

- `decode(json, type)` reads a value as its type says. The type is needed: a timestamp and a string share a JSON
  form, and so do a decimal and a record with fields `num`, `den` and `dp`.
- `decodeUntyped(json)` reads a literal on its face: an object of exactly `num`, `den` and `dp` is a decimal, with
  `unit` as well a quantity, anything else a record; a string is a string.
- **A fractional JSON number is refused.** By the time it is read it has been rounded to a double; an exact
  fraction is written `{num, den, dp}`.
- `encode` writes a value back, and `canonicalJson` writes it with every object's keys sorted, so equal values
  give equal text. The query cache keys on it.

## The tree

`parseExpression(json, path)` parses once into an immutable `ui::Expr`. Every node records its JSON pointer,
which names it in a load-time refusal and in a runtime report. One shape serves every form: `name`, `literal`,
`fallback`, the operands `args`, and for `obj` and `t` the operand names `keys`.

Refused at parse, with the path and the reason: an object that is no form, a bare array (a list is
`{"list": [...]}`, a constant `{"lit": [...]}`), a key a form does not take, a function outside `expr/1`, a wrong
argument count, a fractional number, and a tree of more than `kMaxExpressionNodes` (256) nodes. The count stops
at the first node past the bound, so an oversized tree is never built whole.

`{"ref": "event"}` and `{"ref": "event.path"}` parse to their own form, `Event`, so the validator can refuse them
outside a command list without reading names.

## Evaluation

`evaluate(expr, context, diagnostics)` walks the tree. It never throws.

- **`ExpressionContext`** answers what an expression reads: names, the innermost row, the running event, query
  values, mutation results, pending and error state, `can`, `pref`, `env`, `session` and `host`, and supplies the
  unit table, the locale, its numeric facts, the display offset, the message catalogue and the display formatter.
  Every reader answers null by default, so a context overrides only what its caller knows.
- **Absence propagates.** A function whose argument is null yields null, except `engaged` and `coalesce`, which
  exist to test for it.
- **Kleene logic.** `and`, `or`, `not`, `any` and `all` are three-valued: `false` decides an `and` whatever else
  is null, `true` decides an `or`, and otherwise a null makes the result null.
- **Errors are values.** A type mismatch, an overflow or a division by zero yields null and is reported to
  `Diagnostics` **once per expression path**: a binding re-evaluates on every change, and reporting each time
  would flood the channel with one fault.
- A comprehension binds its `as` name for its body; a binding shadows a name of the context.

## The function library

Exact wherever its inputs are.

- **Numbers.** An `int` stays an `int` through `add`, `sub`, `mul` and `neg`, checked for overflow before it is
  formed. A decimal operand makes the result a decimal; `div` is always a decimal. A `Rational` overflow is null,
  never a clamped value.
- **`round(x, dp)`** rounds half to even. `roundToDecimalPlaces` saturates rather than failing, so the scaled
  value is checked first and an overflow is null.
- **`mean`** is the exact sum over the count, rounded once, half to even, at the largest places among the
  operands. No double is formed, so `(2.67 + 2.68) / 2` is the tie `2.675` and reads `2.68`.
- **Comparison** is exact across `int`, `decimal` and `quantity`, and across units through the unit table. Strings
  compare by bytes, timestamps by instant.
- `length` counts code points. `trim` removes ASCII white space and U+00A0.
- `format(template, args...)` substitutes `{0}`, `{1}`, ... with each argument's plain text (a decimal at its
  places with `.`); `{{` and `}}` are braces.
- `includes` and `indexOf` compare as `eq` does; `indexOf` is null when the value is absent.

## Units

`UnitTable` is the bundle's unit table: each unit's id, display text and default places, and exact ratios
(`1 from = fromTo × to`). A client compiles in no unit enum. `factor(from, to)` walks the ratios breadth first,
each usable both ways, so a table may declare a chain instead of every pair. A quantity in a unit the table
cannot join is null, reported as `no conversion from ... to ...`.

Arithmetic and comparison convert the right operand into the left one's unit. A converted amount keeps its
declared places. `mul` and `div` take at most one quantity, and a number does not divide by a quantity: the
library carries no derived units.

## Display

- `number`, `decimal(x, dp)`, `quantity(x, unit?)` and `money(x, currency)` write digits with the context's
  `render::NumericLocale`.
- `date(x, style)` and `dateTime(x, style)` go through the context's `DisplayFormatter`: morph carries no CLDR
  date patterns, and a toolkit does. The default writes ISO-8601 in the display zone: `short` is the date,
  `medium` (the default) the date and minutes, `long` the date and seconds. `money` also goes through it; the
  default writes the amount, a no-break space and the currency code.
- A style other than `short`, `medium` or `long` is a runtime error.

## Messages

`t(key, fallback, args)` looks the key up in the context's `TranslationProvider` for the active locale, and
otherwise uses the fallback. A catalogue entry outside the supported subset is reported and the fallback is shown.

`parseMessage` reads this subset of Unicode MessageFormat 2 and refuses anything else with the reason and the
offset:

- text with placeholders `{$name}`, `{$name :number}` and `{$name :string}`;
- the escapes `\{`, `\}`, `\|` and `\\`;
- a complex message: `.input {$name :number}` or `.input {$name :string}` declarations, then `.match` on declared
  names, then variants, each with one key per selector and a quoted pattern `{{...}}`. A key is a plural category,
  an exact value (`0` or `=0`), a literal (bare or `|quoted|`) or `*`, and one variant must be all `*`.

`formatMessage` picks, per selector, an exact value before the plural category before `*`, and of the matching
variants the one whose keys rank best in selector order. `:number` writes the locale's digits; an argument is
shown at its declared places, and its plural category is read from the digits shown, so `1.0` is not `one` in
English.

Plural rules are compiled in for `en`, `de`, `fr`, `pl` and `ar` (CLDR cardinal rules), chosen by the language
subtag. Any other language reads `other` for every number, as CLDR's root locale does, so its catalogue's exact
values and `*` variant still apply.

## The corpus

`tests/data/ui/expressions.json` holds named cases over shared names and a unit table: an expression, and its
value, its runtime report, or its parse refusal. The same file is meant to be read by the interpreter's and the
server's tests, so the language has one statement of its behaviour.

## Design decisions

- **One `Expr` shape for every form**, not a variant of node types: the forms differ in which fields they use,
  not in their structure, and the validator walks one shape.
- **Errors are reported once per path, not once per evaluation.** The path is stable across re-evaluation;
  the sink sees each fault once.
- **The evaluator owns no state between calls.** Caching, dependency tracking and re-evaluation belong to the
  interpreter, which runs `evaluate` inside its reactive computations.
- **Dates go through a formatter interface; numbers do not.** `locale_format.hpp` already formats numbers from a
  handful of locale facts; date patterns need CLDR data that a toolkit has and morph does not.
