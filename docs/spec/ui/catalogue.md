# Message catalogues — design

A bundle carries one message catalogue per locale. The client reads them, once the bundle is verified, into a
`TranslationProvider` that every `t` resolves through; `{"setLocale": …}` switches the locale and re-resolves every
`t`; a missing catalogue or key shows the `fallback`; and a navigator can find a title in every locale the bundle
has. This implements spec 6 §19 "Messages" (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`) and the
loading rule of spec 5 §4.

| Header | What it holds |
|---|---|
| `include/morph/ui/catalogue.hpp` | `catalogueLocale`, `parseCatalogue`, `Catalogue`, `MessageCatalogues`, `loadCatalogues`, `textInEveryLocale`, `matchesInAnyLocale`, `catalogTitle`, `catalogTitles`, `catalogTitlesIn`, `catalogLabel` |
| `include/morph/ui/service.hpp` | `UiRegistry::addFile` checks a catalogue at registration |
| `include/morph/ui/interpreter.hpp` | `Session::setTranslations` |
| `include/morph/client/source.hpp` | `Source::catalogues()`: the bundle's verified catalogues |

## Contents

- [The file](#the-file)
- [At registration](#at-registration)
- [At load](#at-load)
- [Resolution](#resolution)
- [Switching locale](#switching-locale)
- [Keys known at run time](#keys-known-at-run-time)
- [Search](#search)
- [Catalog titles](#catalog-titles)
- [Design decisions](#design-decisions)

## The file

A catalogue is a `messages` file of the bundle whose id is `i18n/<locale>.json`, the locale a BCP 47 tag of ASCII
letters, digits and `-` that starts with a letter (`catalogueLocale`). Its bytes are a flat JSON object of message
key to a pattern in the MessageFormat 2 subset of spec 5 §4:

```json
{ "lims.sample.title": "Rejestracja próbek",
  "lims.samples": ".input {$n :number}\n.match $n\none {{{$n} próbka}}\nfew {{{$n} próbki}}\n* {{{$n} próbek}}" }
```

Plural and select forms live in the pattern, so a locale with more plural categories adds variants in its own
catalogue without changing the document.

`parseCatalogue` refuses text that is not JSON or nests deeper than `kMaxJsonDepth`, anything but an object, a
must-understand (`!`) key, a value that is not a string, an empty key or one longer than `kMaxNameBytes` (256), and
more than `kMaxCatalogueKeys` (16384) keys. Each pattern is parsed with `parseMessage`; one outside the subset does
not refuse the catalogue: its key is left out and listed in `Catalogue::dropped`.

## At registration

`UiRegistry::addFile` refuses a `messages` file whose id is not `i18n/<locale>.json`, that `parseCatalogue` refuses,
or that has any dropped pattern. The server is where an author learns a catalogue is wrong, so it is strict there.

## At load

`loadCatalogues(manifest, files)` reads every `messages` file the verified manifest lists, from the verified files
(`LoadedBundle::manifest`, `LoadedBundle::files`). A catalogue whose id is not `i18n/<locale>.json`, whose bytes are
not among the files, or that `parseCatalogue` refuses, is left out; a pattern outside the subset is dropped. Each is
a line in `MessageCatalogues::problems()`, naming the file. The client is lenient where the server is strict: a
bundle a server built with an older check still opens, and what cannot be used shows its fallback. Nothing is
parsed for the first time at a `t`.

## Resolution

`MessageCatalogues::provider()` returns a `TranslationProvider` over the catalogues, shared and immutable, so it
stays valid after the `MessageCatalogues` is gone. A lookup tries the exact locale's catalogue, then each shorter
tag (`de-AT`, then `de`); a miss is `std::nullopt`, and `t` formats the `fallback`. A missing catalogue is a miss for
every key.

## Switching locale

`Session::setTranslations(provider)` installs the provider and keeps the other formatting facts. `{"setLocale": "pl"}`
calls `Session::setLocale`, which replaces the reactive formatting facts with the new locale, the locale's number, money and date facts ([`locale_table.md`](locale_table.md)) and the same
provider, so every `t` re-evaluates; then it calls `InterpreterHost::setLocale`, through which the application stores the
choice as a preference.

The client shell is what installs the provider, each time it loads a bundle: at start, after the reload a sign-in or
sign-out makes, and after a development poll that delivered a new bundle, including one whose only change is a
catalogue. It reads `Source::catalogues()` (the bundle's verified catalogues, `loadCatalogues`), reports each of
`problems()`, and calls `setTranslations(catalogues.provider())`; every `t` re-evaluates even when no document changed. It also calls `Session::setLocale` with the locale it
starts in, which installs the locale's number, money and date facts ([`locale_table.md`](locale_table.md)). The
navigator's titles and headings are read in the active locale and relabelled on every `setLocale`
([`workspace.md`](workspace.md#navigator-and-pins)).

## Keys known at run time

`t` accepts a key expression in place of a literal key (`{"t": {"expr": …}}`, `tAt`), for a message whose key arrives
with data, such as an action refusal's code. Only the `fallback`'s syntax is checked at load, for either kind of key; no check tests that a key, literal or
run-time, has an entry in a catalogue, and a missing literal key shows its fallback without a report. The interpreter
reports a run-time key the catalogue lacks at run time, as a problem once per expression site and key, and shows the
fallback. The application covers its keys, literal and run-time alike, with a test of its own
over its catalogues. See [`refusals.md`](refusals.md).

## Search

`textInEveryLocale(catalogues, key, fallback)` formats a key's message in every locale the catalogues hold, in the
order of `locales()`, the fallback where a locale has no entry. `matchesInAnyLocale(catalogues, key, fallback, query)`
is true when any of those texts, or the fallback, contains the query; ASCII letters compare without case, every
other byte exactly. A message is formatted with no arguments, as a title is. The navigator that uses it is the
workspace shell's (spec 6 §7).

## Catalog titles

A catalog entry's `title` is a string or a message reference (`{"t": key, "fallback": …}`). Two functions give a shell
of its own a title in one locale and in every locale:

- `catalogTitle(entry, catalogues, locale)`: a string title as is; a reference formatted in `locale` from
  `catalogues` (the exact locale's catalogue, then its language's), else its fallback; else the entry's id.
- `catalogTitles(entry, catalogues)`: the title in every locale the catalogues hold (`textInEveryLocale` of the key
  and the fallback, the id standing in for a missing fallback), for a search. With `catalogues` null it is one title:
  a string title as is, else a reference's fallback, else the entry's id — what the navigator shows without
  `NavigatorOptions::titles`.

Two functions give the navigator what it shows:

- `catalogTitlesIn(entry, catalogues, locale)`: the title in `locale` first, then the other locales' titles that differ
  from it, so the navigator shows the active locale's and a search matches every locale's.
- `catalogLabel(catalogues, text, locale)`: a heading or other text that may be a message key. It answers the message
  formatted with no arguments from the first of: the catalogue for `locale` (the exact tag, then each shorter tag), the
  `en` catalogue, any catalogue that holds the key; and `text` as written when none does. A heading has no fallback
  text, so under a locale with no catalogue the person reads the `en` words, not a raw key.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Where a catalogue's locale comes from | Its id, `i18n/<locale>.json` | The manifest lists ids; the locales are known without fetching a file. |
| A pattern outside the subset | Dropped at load, its key shows the fallback | Spec 5 §4; one bad message must not hide the rest of a locale. |
| Strict at registration, lenient at load | Both | An author sees the error; a user still sees a working screen. |
| Locale fallback | Exact tag, then shorter tags | A region usually shares its language's messages. |
| Case-insensitive search | ASCII only | Full Unicode case folding needs tables the client does not carry. |
