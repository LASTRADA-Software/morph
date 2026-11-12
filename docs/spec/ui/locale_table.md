# Locale table

`include/morph/ui/locale_table.hpp` is the table of facts a locale writes numbers, money and dates with. The client
shell installs a bundle's message catalogues ([`catalogue.md`](catalogue.md)) and, through `Session::setLocale`, the
facts of the locale the person is in, so a screen's `t`, `number`, `money` and `date` follow the locale.

| Name (`morph::ui`) | Is |
|---|---|
| `SymbolPlacement` | `Before` or `After` the amount. |
| `CurrencySymbol` | `{code, symbol}`. |
| `CurrencyFacts` | `{placement, symbols}`; empty `symbols` writes every currency by its code. |
| `LocaleFacts` | `{tag, numeric, currency, datePattern, dateTimePattern}`. |
| `localeTable()` | The rows (`en`, `de`). |
| `findLocaleFacts(tag)` | A row by the tag, then shorter tags; null when none. |
| `localeFacts(tag)` | `findLocaleFacts`, else `en`'s row. |
| `MoneyForm` | `Symbol` or `Code`. |
| `placeCurrency(number, currency, facts, numeric, form)` | The amount with its currency. |
| `FormatContext::currency` | The `CurrencyFacts` the session writes `money` with; default empty. |

## Contents

- [What the table holds](#what-the-table-holds)
- [Lookup](#lookup)
- [Who installs the facts](#who-installs-the-facts)
- [Both frontends](#both-frontends)
- [Money](#money)
- [Months](#months)
- [Design decisions](#design-decisions)

## What the table holds

One `LocaleFacts` row per locale:

| Member | Holds |
|---|---|
| `tag` | The BCP 47 tag the row answers to (`en`, `de`). |
| `numeric` | The `render::NumericLocale`: decimal and group separators (`en`: `.` and `,`; `de`: `,` and `.`), the sign, the digits. |
| `currency` | A `CurrencyFacts`: where the symbol stands (`SymbolPlacement::Before` or `After` the amount) and the symbols the locale writes, as `CurrencySymbol{code, symbol}`: `EUR` `€`, `USD` `$`, `GBP` `£`, `JPY` `¥`, `CHF` `CHF`, in both rows. |
| `datePattern`, `dateTimePattern` | The locale's default date and date-and-time patterns. `de`: `dd.MM.yyyy` and `dd.MM.yyyy HH:mm`, the DIN 5008 numeric form. `en`: none. |

Every pattern in the table passes `checkDatePattern`, which a unit test asserts.

`en` has no date pattern because English has no one convention: `11/10/2026` reads as 10 November in one country and
11 October in another. ISO (`2026-10-11`) stays, which is what the client writes without a pattern.

The table is `inline constexpr` data in the header, with static storage: `NumericLocale` holds `string_view`s, so the
strings they name must outlive every `FormatContext` that copies a row. `CurrencyFacts::symbols` is a span over a
static array of the same kind. An empty `symbols` means every currency is written by its code: the form a default
`FormatContext` has.

`localeTable()` answers the rows (`en`, `de`). A locale an application needs is a row added to the table.

## Lookup

`findLocaleFacts(tag)` tries the exact tag, then each shorter one, cutting at the last `-`: `de-AT`, then `de`. This
is the walk a catalogue lookup makes (`MessageCatalogues::find`), so a region finds its language's facts as it finds
its language's messages. It answers null when no prefix has a row. Tags compare exactly, as catalogue locales do.

`localeFacts(tag)` is `findLocaleFacts` with `en`'s row where none matches, so a formatting function always has facts.

The client shell does not switch silently to `en`'s facts. It reports a locale with no facts as a problem, once per
locale tag for the shell's lifetime, whether the locale came from `ShellOptions::locale` or from a `setLocale`:
``no formatting facts for locale `pl`; numbers, money and dates use en's``. It then writes as `en` does.

## Who installs the facts

`Session::setLocale(tag)` installs `localeFacts(tag)` in the same `setFormatting` call that changes the locale, so no
expression sees the new locale beside the old facts:

- its `numeric` and its `currency`, always;
- its date patterns, only for the one the application declared none of. `Session::setDatePatterns` records, per
  pattern, that the application declared it (a non-empty one). The app document's `formatting.date` is the on-screen
  pattern of the whole application, in every locale; an application that regulates its date form, such as lims's
  `dd-MMM-yyyy`, keeps it under `de`. An application that declares only the date pattern keeps that one, and its
  date-and-time pattern comes from the locale (ISO under `en`). The locale's pattern is a default for what the
  application declared nothing about.

Date inputs read the session's pattern reactively, as `date` and `dateTime` do, so after `setLocale` a screen of an
application with no declared pattern shows one form of a date everywhere on it, inputs and texts alike, and an input
parses what the new pattern writes.

A `Session` constructed alone keeps `FormatContext`'s own defaults (the C spelling, money by code, ISO dates) until
`setLocale` is called, so interpreter tests and embedders that call `setFormatting` keep what they set. The client
shell always calls `setLocale` before the first mount, with `ShellOptions::locale` or `en`.

**Reach.** The locale's decimal separator applies wherever a number is shown or typed through the session's facts: the
display functions, plain text interpolation, `format`, a message's placeholders (`{$n}` and `{$n :number}`) and the
entry of a number input. The group separator applies in `number`, `quantity`, `money` and `decimal` (which write
through the same path) and in a message's `:number` (a catalogue text is prose, as `number` is); `parseDecimal`
accepts well-placed group separators. An integer shown through plain interpolation, `format`, a bare message
placeholder or an input stays ungrouped, `2026` and not `2,026`, in every locale: `Year {$year}` reads `Year 2026` in
`en` and in `de`, and `Year {$year :number}` reads `Year 2,026` and `Year 2.026`. An integer that is meant to be read as
a quantity is shown with `number`; one that is an identifier needs nothing.

`localeScope` (a scope inside a screen that writes in another locale) reads the same table; its own spec states
whether a scope's locale pattern wins over the application's inside the scope.

## Both frontends

Qt Quick and the terminal read the session's `FormatContext`. Neither consults `QLocale` or the C library's locale,
so the two write the same text; the date picker's month header takes its month names from the session, English as the
`MMM` rule says, and not from `Qt.locale()`; its weekday names are English and its week starts on Monday, the C
locale's, whatever the desktop's language. `QLocale` may not override the table: two frontends that disagree on `1.234,50` make a
screen test's expectation depend on the frontend.

## Money

`money(x, currency, form?)` writes the amount at the currency's minor digits through
`render::formatCanonicalNumber` with the context's `numeric`, and places the currency with
`placeCurrency(number, currency, facts, numeric, form)`. The third argument is `"symbol"` (the default) or `"code"`;
any other string is a mismatch (`money`'s form is "symbol" or "code").

- **`en`, `Before`.** The symbol stands before the amount: `€13,196.40`, `¥1,234,567`. A symbol ending in an ASCII
  letter is followed by a no-break space (U+00A0): `CHF 2,450.35`.
- **`de`, `After`.** The symbol follows the amount after a no-break space: `13.196,40 €`, `1.234.567 ¥`, `2.450,35 CHF`.
- **Negative.** The sign stands first in both: `-€13.00`, `-13,00 €`. `placeCurrency` takes the leading
  `numeric.negativeSign` (or `-` when it is empty) off the number and puts it before the symbol, so a symbol never
  sits between the sign and the digits.
- **Unlisted currency.** One the locale lists no symbol for is written by its code: `5.00 XTS`.
- **Code form.** `money(x, currency, "code")` writes `13,196.40 EUR` (`13.196,40 EUR` in `de`), with the code after
  the amount in every locale, separated by an ASCII space (`number + " " + code`), unlike the no-break space of the
  `de` symbol form. An invoice line or an export names the currency unambiguously where `$` does not.
  `"symbol"` is the default because the mockups and the production criteria show symbols.
- **Default context.** A `FormatContext` with no currency facts writes `13196.40 EUR`, so the expression
  corpus's expectations hold.

## Months

`MMM` stays the English abbreviation (`Nov`) under `de`, as [`expressions.md`](expressions.md)'s date patterns state: a
localized month name would read back differently per locale. An application that wants a German month form declares a
numeric pattern.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Source of the facts | A table morph owns, in a header | The terminal frontend has neither `QLocale` nor ICU; with either as the source the two frontends would write different text. |
| CLDR in full | Not used | A dependency and megabytes for two shipped locales. The table grows a row per locale an application ships. |
| Facts in the bundle | Not used | Each application would restate what a locale is. It stays possible later as an override of a row. |
| Locale date pattern against the application's | The application's wins | An application that regulates its date form would lose it under `de` otherwise. |
| `en` date pattern | None (ISO stays) | English has no one numeric convention, and ISO is what the client writes without a pattern. |
| Money default form | Symbol | The mockups and production criteria show symbols; the code form stays one argument away. |
| A locale with no row | `en`'s facts, reported once | Switching silently would hide a missing row; refusing would leave a person with no formatting. |
| Sign placement | Before the symbol | `-€13.00` reads as one number; a symbol between sign and digits does not. |
