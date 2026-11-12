# The shell's declared parts (`ui::ShellParts`)

`include/morph/ui/shell_parts.hpp`. An application declares, as data in its app document, the five parts of a shell that
no screen can draw: a scope switcher, a count per module, one search over every module, an inbox button and a menu for
the signed-in person. Each part names a model and a read action by id and the fields of its reply that it reads. The
client reads the action as a query of the app document, and the library's [`ShellFrame`](shell_frame.md) draws the
answer. The library knows no company, no role and no record kind: what a scope, a count or a hit means is the
application's, and a CRM declares the same five parts for its own models.

## Contents

- [Why parts and not screens](#why-parts-and-not-screens)
- [The declaration](#the-declaration)
- [Where it lives](#where-it-lives)
- [How a part is read](#how-a-part-is-read)
- [Typed values and the locale](#typed-values-and-the-locale)
- [The context](#the-context)
- [Changing the context](#changing-the-context)
- [Where each part is drawn](#where-each-part-is-drawn)
- [The search results panel](#the-search-results-panel)
- [The compact form](#the-compact-form)
- [The terminal frontend](#the-terminal-frontend)
- [Keyboard](#keyboard)
- [Loading, empty and error states](#loading-empty-and-error-states)
- [Accessibility names and texts](#accessibility-names-and-texts)
- [Failure and refusal](#failure-and-refusal)
- [Out of scope](#out-of-scope)
- [Alternatives considered](#alternatives-considered)
- [Design decisions](#design-decisions)

## Why parts and not screens

The frame (sidebar, top bar, navigator) is the client's, built from widgets, and it is on screen whatever tab is shown.
A screen is a document mounted on the page, so a screen that drew a switcher or a search would draw it once per tab and
inside the page, not in the chrome. The five parts are therefore declared beside the screens and read by the shell.

## The declaration

```cpp
struct ShellParts {
    std::optional<ContextSwitch> context;     // the scope switcher
    std::vector<ModuleCount>     modules;     // a count per module
    std::vector<SearchSource>    search;      // one source per module searched
    std::optional<InboxEntry>    inbox;       // the inbox button
    std::optional<PrincipalMenu> principal;   // the signed-in person's menu
    std::string                  searchHint;  // the search field's placeholder: a catalogue key or text
};
```

Every part is optional and a default `ShellParts` changes nothing: a frame without parts is the frame
[`shell_frame.md`](shell_frame.md) describes. Each member names a model by its registered type id and an action by its
registered id, and members of the action's reply by name. A reply member is read from the reply's JSON object by name.

Every read action may be given two **request fields**: `contextField`, the member of the action's request that the shell
fills with the chosen context (empty: the context is not sent), and `localeField`, the member it fills with the
session's locale tag (empty: not sent). The shell fills only the members a declaration names and never reads a schema:
what goes in the request is in the declaration. A request with neither is `{}`.

| Type | Members | The action answers |
|---|---|---|
| `ContextSwitch` | `model`, `action`, `localeField`, `optionsField`, `labelField`, `valueField`, `detailField`, `groupField` | a reply whose `optionsField` is a list of objects. Each has a `label` (`labelField`), a `value` (`valueField`, a string or an integer), and optionally a `detail` (`detailField`, a second line) and a `group` (`groupField`, a heading the option sits under; adjacent options with the same group share one heading). An option such as "all sites" is just an option the application returns |
| `ModuleCount` | `module`, `model`, `action`, `countField`, `contextField`, `localeField` | a reply whose `countField` is a non-negative integer: the module's open work. `module` is a `CatalogEntry::module`; one entry per module, because an action type belongs to one model |
| `SearchSource` | `module`, `title`, `model`, `action`, `queryField`, `resultsField`, `contextField`, `localeField` | a request with a string member `queryField` (the typed text) and a reply whose `resultsField` is a list of hits. `title` heads the source's hits: a catalogue key or text; empty uses the module's label |
| `InboxEntry` | `screen`, `sources` | `screen` is the screen id the button opens; `sources` is a list of `ModuleCount`; the button's count is the sum of the counts of the sources that answered |
| `PrincipalMenu` | `model`, `action`, `contextField`, `localeField`, `nameField`, `detailField`, `subtitleField`, `rolesField`, `locales`, `formats` | a reply with the person's name (`nameField`), optionally a detail line for the menu (`detailField`) and a line under the name in the trigger (`subtitleField`), and `rolesField`, a list of roles |

A **role** is a string, or an object with `label` and optionally `detail` (a scope line such as the limit it holds).
`locales` is the list of language tags the menu offers; empty derives it from the bundle's message catalogues
(`MessageCatalogues::locales()`). `formats` is a list of tags whose number, money and date facts the person may choose
separately from the language; empty hides that choice.

A search **hit** is an object with fixed members, because the library cannot know which of an application's fields opens
which screen: `screen` (a screen id), `params` (an object, the screen's params; absent for none), `title`, `detail`
(optional, muted, after the title) and `kind` (optional, the hit's type, a muted label before the title). A hit with no
`screen` or `title` is dropped and reported.

## Where it lives

The declaration is a member of the **app document**, `"shell": { … }`, whose JSON is the serialisation of `ShellParts`;
a screen that has the member is refused. `UiRegistry::setShellParts(ShellParts)` writes the member into the app document
the registry holds (an app document is created, empty, when the application registered none), so the bundle carries the
parts in the file every client already fetches, keeps and reloads: no new manifest kind, no new slot in `ui::Bundle`, no
fetch to gate and nothing a signed-out client reads beyond the app document it already reads. A development reload of
the app document takes effect at the next frame build, as every other member of it does.

The loader checks the declaration when the document loads and refuses it, naming the path: a `module` listed twice in
`modules` or `search`; an empty field name that must not be empty; a `locales` or `formats` tag listed twice; an inbox
with no sources. A catalog entry the declaration names (the inbox's `screen`, a `module`) that the catalog does not list
is reported by the client, since the catalog is known to the client and not to the loader. An action that is not
registered is the loader's usual action refusal; a reply that lacks a named field is the client's report
([Failure and refusal](#failure-and-refusal)).

## How a part is read

At load, `shell` is lowered to declarations of the app document's own scope, and the parts are then ordinary queries of
the app root, mounted headless on the session as `AppRoot` already is:

- one model alias per distinct model, `"instance": "shared"`, so a part and a screen reading the same model share one
  instance and one cache;
- one query per read: `shellContext`, `shellCount<i>` (`i` the place in `modules`), `shellSearch<i>`, `shellInbox<i>` and
  `shellPrincipal`; each body is an object of the request fields the declaration names, reading `{"env": "context"}`
  and `{"env": "locale"}` and, for a search, the state `shellSearchText`;
- the state `shellSearchText`, written by the search field after a pause, and a `when` on each search query that holds
  while the text is not empty.

These names are reserved: a document that declares one of them is refused. Because they are queries, a part refreshes
by the path a screen's query does and needs nothing the screens do not: on mount; when a record the reply holds changes
(on this client or another: an action's `BRIDGE_REGISTER_EFFECTS`, [`KEEPING-SCREENS-CURRENT`](../../KEEPING-SCREENS-CURRENT.md));
when the context or locale it reads changes; and when a stale entry is read again. A count action's reply type should
therefore be a registered record; if it is not, the shell reports it once from the catalog's facts for the action, and
the count is read on mount and on a context change only.

**Search.** The field writes `shellSearchText` after `kSearchPause` (200 ms) with no further edit; each search query has
`debounce` and `when`, so an edit before the pause ends replaces the pending request and a reply to an older text never
replaces the hits of the current one. All sources are asked at once and drawn as they arrive, in `search` order. An empty
text asks nothing and clears the hits. At most `kSearchHitsPerSource` (8) hits of a source are drawn. Hits are queries
too, so while the panel is open a change to a record they hold refreshes them.

**Authorisation.** A part whose action the principal may not run (the loader's `topQueries` rule) is not drawn and not
reported: that is the authoriser working. A frame with the inbox's sources all not permitted hides the button; a source
not permitted contributes nothing to the sum.

## Typed values and the locale

A string field of a reply is drawn as it is, unless it is a typed value: a label, detail, title or role field may carry a
money, number, date or quantity value, which the client writes in the session's locale exactly as a table cell does
(`textOf(value, session.formatting())`), and a message reference (`{"t": key, "args": {…}}`), which resolves through the
catalogue with its arguments formatted the same way. Every part's body reads `{"env": "locale"}` where it names a
`localeField`; so a part reads again when the session's locale changes and the server may localise its own strings, and
a typed value is re-written on a locale change with no new request. Counts are written with the locale's digit grouping
(`1.234` in `de`) and a count above `kCountCap` (99) is drawn `99+`.

## The context

The switcher's chosen option **value** is the **context**. It is held in the session's environment and read by every
screen as `{"env": "context"}`: `context` is a name `env` reads, a `ui/1` vocabulary addition ([`expressions.md`](expressions.md)).
A screen's query passes it into its body itself; the shell never edits a screen's body. For the shell's own reads the
declared `contextField` is the whole mechanism.

- **Start.** The shell mounts no tab until the switcher has answered (or failed): the page shows its loading state
  meanwhile (`shell_loading`, a busy widget), so no screen fetches unscoped. The first option is the context at start.
  With no switcher declared the context is `null` and nothing waits.
- **Equality** is by JSON value: the integer `1` and the string `"1"` are different options.
- **A vanished option.** When the options are refreshed and the chosen value is no longer among them, the first option
  becomes the context. With no options the context is `null` and the switcher draws disabled.
- **Held** for the session: not stored between runs, not shared between clients; a sign-in or sign-out clears it.

## Changing the context

Two ways, one rule. The person chooses an option in the switcher, or a screen runs the `ui/1` command
`{"setContext": e}`, `e` an expression whose value is an option's value. The command is handled by the shell
(`InterpreterHost::setContext`): it refuses a value that is not among the current options (reported, nothing changes),
and it is not a host command, so the shell registers nothing globally and nothing outlives the frame. `{"can":
"context"}` is true exactly while a switcher with at least one option is installed, and false otherwise, so a banner
offers the button only where it works. A refusal that came from the scope can therefore offer a button that runs it.

**Unsaved work.** A switch re-evaluates every open tab's bodies, so a tab that holds unsaved changes would submit its
draft into the new scope. When any open tab is dirty, the switch is not applied at once: the shell asks once, in a
dialog, "Switching scope discards unsaved changes in N tabs" with **Discard and switch** and **Cancel**. Cancel changes
nothing; the switcher keeps its choice. Discard closes those tabs without saving and applies the switch. The dialog is
the same one for the command and the switcher, and a tab that is not dirty is never asked about.

## Where each part is drawn

The screen's head is in the page: a bundle that declares `search`, `inbox` or `principal` mounts its workspace with
`HeadPlacement::InPage`, so the top bar holds no title or head actions and the three parts, in this order:

| Part | Expanded `ShellFrame` |
|---|---|
| Switcher | at the top of the sidebar, under the application's name: a button-style menu (`shell_context`). Its trigger is two lines, the chosen label and its detail; its entries are the options as radio-checked items, with the detail as the entry's description and a heading entry per group |
| Module groups | in the navigator ([`shell_frame.md`](shell_frame.md#module-groups)): one row per module, its count as a badge while the module is closed; only the open module lists its sections and screens |
| Search | the top bar's first item, a text input (`shell_search`) that stretches to at most `kShellSearchWidth` (48) units, with the hint `searchHint` and the shortcut "Ctrl K" beside it; its results are a [panel](#the-search-results-panel) under it |
| Inbox | the second item: a button (`shell_inbox`) with the screen's catalog title as its caption and an icon, and its count in a badge that is its sibling (`shell_inbox_count`, `Tone::Err` while above zero) |
| Principal | the last item: a button-style menu (`shell_principal_menu`) whose trigger is the name over the subtitle; its entries are the name (description: the detail line), a "Your roles" heading with each role (label, description: its detail), a separator, "Language" and, when `formats` is given, "Number and date format" as headings with radio-checked entries, and the sign-out entry when a handler exists |

With a `principal` part the principal is drawn by the menu in the top bar whatever `principalPlacement` says, and the
sidebar's principal text and sign-out button are hidden (sign-out is in the menu). With a `search` part the navigator's
own search field is hidden (the global search replaces it); the pins stay at the top of the navigator. The top bar's
line wraps as it does today when the window is narrow.

**Focus order** is the switcher, the module rows, the search, the inbox, the principal menu, then the page.

**Module groups.** A module's row label is the group heading of its first entry, through the navigator's `label`; the
entries' own groups show as section headings inside the open module. Opening a module closes the one open before it. The
open module at start, and after each navigation, is the shown screen's; the person's toggling of another is kept until
the next navigation. A closed module shows its count and a ▸ chevron; the open one shows ▾ and no count (its screens
carry their own `navigation` badges). A count of zero draws no badge, and a module with one entry is that entry's row.

## The search results panel

The results are a **non-modal panel**, a `PopoverWidget` (a backend container, [`backend_contract.md`](backend_contract.md)),
anchored under the search field. It floats over the main area on Qt Quick; on the terminal, which has no floating layer,
it is the rows under the field, in flow. It is not a dialog: it takes no focus and dims nothing, **focus stays in the
field**, and it closes on Escape, on choosing a hit, and when focus leaves the search.

Hits are grouped under a heading per source (its `title`), each hit a row of its `kind`, `title` and `detail`. The first
hit is highlighted when results arrive. Choosing a hit opens its screen with its params as a tab (`navigate`, the path a
navigator choice takes), clears the field and closes the panel. Compact, the same panel is the full-screen search
overlay.

## The compact form

Below `expanded` the sidebar is gone and the parts move:

- the navigation menu (`shell_nav_menu`) lists, in order: the switcher as a submenu titled with the chosen label,
  holding the options as radio-checked items; a separator; the modules as submenus, the count after the label, each
  submenu holding its sections as headings and its screens; a separator; the principal's menu as a submenu (the entries
  of the expanded menu); and the sign-out entry when a handler exists and there is no principal part. Choosing an entry
  is mapped by its path through the entry list the frame built, not by a position;
- the search and the inbox stay in the bar as icon buttons (`shell_search_open`, `shell_inbox`) with the count in the
  inbox's accessible name and a badge beside it;
- the search opens full-screen: the same field and results in an overlay over the whole window (`shell_search_overlay`),
  closed by Escape, by choosing a hit, or by its close button.

## The terminal frontend

The same widget tree is drawn by the terminal renderer, so each part is usable and unstyled: the switcher and the
principal's menu are its menus, the trigger's two lines on one row joined by a dot; a module row has its count in
brackets and `>` or `v` as its chevron; the search is a one-row input with the panel's rows under it; the inbox button's
caption ends with the count.

## Keyboard

- **Ctrl K** focuses the search from anywhere in the frame (compact: opens the overlay). It is bound through
  `Widget::setKeys` on the frame's root, so a screen that binds Ctrl K itself shadows it (the innermost chord wins), and
  with a dialog open, which is modal, it does not reach the frame.
- **Down** and **Up** in the field move the highlight over the hits, wrapping at the ends, while focus stays in the field.
  **Enter** opens the highlighted hit. **Shift Enter** opens every drawn hit as a tab, in order, at most
  `kSearchOpenAllLimit` (10); the first one opened is the active tab, and sources still in flight are not waited for. A
  terminal that sends Shift Enter as Enter uses **Alt Enter**. **Escape** clears the highlight and closes the panel,
  leaving the text; a second Escape on an empty field returns focus to the page.
- The switcher and the principal's menu are menus: their keys are the renderer's.

## Loading, empty and error states

| State | Search panel | A count (module or inbox) |
|---|---|---|
| Not yet answered | a busy row (`shell_search_busy`) while any source is pending | nothing is drawn |
| Answered, none | every source answered with no hits: "No results for “<text>”" (`morph.shell.searchEmpty`) | no badge for zero |
| One source failed | a muted line under its heading: "<title> could not be searched" (`morph.shell.searchFailed`); the other sources' hits are drawn | an unobtrusive error marker `!` (`shell_count_error_<module>`) whose accessible name is "<label>: count unavailable" (`morph.shell.countFailed`) |

The inbox follows the counts: a failed source adds nothing to the sum and puts the marker on the button; a button whose
every source is unanswered draws no badge.

## Accessibility names and texts

| Widget | Accessible name | Catalogue key |
|---|---|---|
| `shell_context` | "Scope: <label>, <detail>" | `morph.shell.context` |
| `shell_search` | "Search" | `morph.shell.search` |
| the panel | a list named "Search results"; a hit is "<kind>, <title>, <detail>" | `morph.shell.searchResults` |
| `shell_inbox` | "<title>, <n> waiting" (no count while zero or unanswered) | `morph.shell.inbox` |
| `shell_principal_menu` | "Signed in as <name>" | `morph.shell.principal` |
| a module row | "<label>, <n>, collapsed" or "<label>, expanded" (a button has no expanded state, so the name says it) | `morph.shell.moduleClosed`, `morph.shell.moduleOpen` |
| the dirty-switch dialog | "Switching scope discards unsaved changes in <n> tabs" | `morph.shell.contextDirty`, `morph.shell.contextDiscard`, `morph.shell.contextCancel` |
| headings of the principal menu | "Your roles", "Language", "Number and date format" | `morph.shell.roles`, `morph.shell.language`, `morph.shell.formats` |

Every key has a fallback in the client; English and German catalogues ship the same keys and an application's own
catalogues may override them. A heading in a menu is an entry with `MenuEntry::heading`: a label that is not a command,
which a renderer draws as muted text and exposes as static text, not as a disabled command.

## Failure and refusal

A part is a convenience, so a failure of one never blocks the rest of the frame, and every failure is reported through
`ShellOptions::onProblem`, once per part: a reply that lacks a named field, a count that is not a non-negative integer,
a hit that is dropped, an action the dispatcher refuses for any reason other than authorisation. A part that failed draws
as the [states](#loading-empty-and-error-states) say: an error marker, a failed-source line, the switcher disabled. The
start waits for the switcher only until it fails.

## Out of scope

The mockup's help button ("?") and its "Keyboard shortcuts" row are not declared parts: no part reads a model for them,
and an application that wants a help screen puts it in the navigator. The mockup's connection footer in the sidebar is
the client's own and not a declared part.

## Alternatives considered

- **A screen per part.** Each screen would redraw the shell inside the page, once per tab, and could not take the
  sidebar or the top bar. Rejected.
- **An application-specific shell.** Per-application client code, which the design forbids: one client opens every
  application.
- **Writing the counts as app-document state and `navigation` badges.** The app document can already hold a query and
  badge a navigator entry, but only per screen entry, and it can place nothing in the frame: no switcher, no search
  field, no inbox button, no menu in the top bar. The declared parts are the same app-document queries plus the places
  they are drawn.
- **A new manifest file for the parts.** A new kind needs a slot in `ui::Bundle`, an entry in the client's fetch rules, a
  gate for a signed-out fetch and a reload path; the app document has all four.
- **The context as a host command.** The host vocabulary is registered once for the shell's life, cannot refuse by value
  and cannot be withdrawn; a document command handled by the shell is rebuilt with the frame.
- **Adding `context` to a body by reading the action's schema.** It reserves a member name in every application's
  actions and silently stops when the schema is missing; a declared `contextField` is explicit.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| The parts' form | Data in the app document naming a model, an action and the fields read | The domain stays in the application; the library lowers it to queries and draws it |
| Reads are app-document queries | Shared model instance, the screens' staleness and effects path | Nothing new for an application to learn, and other clients' changes arrive the same way |
| One `ModuleCount` per module | The model is part of the entry | An action type belongs to one model, so one action cannot count several modules |
| `contextField`, `localeField` | Declared per read | The request is visible in the declaration; no schema sniffing |
| Hit members fixed | `screen`, `params`, `title`, `detail`, `kind` | A search has to open a screen; naming every member would only repeat the same five |
| `context` is an `env` name, `setContext` a command | `ui/1` gains both | A read needs no new form; a change needs a refusal by value and a rebuild per frame, which a host command cannot give |
| The head in the page | `HeadPlacement::InPage` once parts are declared | The mockup's top bar is search, inbox and person; the page holds the title |
| The panel | A backend container, non-modal | A dialog traps focus; a plain stack in a wrapping row would grow the bar |
| A pause before search | 200 ms, the last edit wins | Keystrokes then cost one request, and replies to an old text are discarded |
| Language and format | Two choices | `Session::setLocale` sets both; `Session::setFormatLocale` overrides the numeric, money and date facts, and the override stays through later language changes |
