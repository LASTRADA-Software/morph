# The UI document — design

Design spec for the UI document's C++ model and its load-time validation
(`include/morph/ui/document.hpp`, loaded by `src/ui/document.cpp`), and for the typed values every document expression evaluates to
(`include/morph/ui/value.hpp`). A server defines a screen as a JSON document (spec 5,
`docs/superpowers/specs/2026-10-07-ui-document-design.md`); `ui::loadDocument` parses it into a
`ui::Document` and refuses it, with the JSON Pointer and the reason, when it breaks a rule of
its vocabulary, `ui/1`. The interpreter mounts only a document that loaded; the server runs the same function when
a screen is registered, so a refused document is a server defect found in the server's own tests.

Read this before changing what a document may say. The expression language inside it is
[`expressions.md`](expressions.md); the view tree a document becomes is
[`view_tree.md`](view_tree.md).

## Contents

- [Values and types](#values-and-types)
- [The document](#the-document)
- [Declarations](#declarations)
- [Commands](#commands)
- [View nodes](#view-nodes)
- [Scopes and names](#scopes-and-names)
- [Load-time rules](#load-time-rules)
- [The catalog](#the-catalog)
- [Refusals](#refusals)
- [Warnings](#warnings)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## Values and types

`ui::Value` is null, `bool`, `int` (int64), `string` (UTF-8), `Decimal`, `Quantity`, `timestamp`
(`time::DateTime`), a list (`ValueList`) or a record (`ValueRecord`, ordered by field name).

- **Exact numbers.** A `Decimal` is a `math::Rational` whose `DecimalPlaces` are the value's declared
  places; a `Quantity` adds a unit id. Places are part of equality (`1.5` at one place and `1.50` at
  two differ), so a reactive equality skip never hides a change of display; `expr/1`'s `eq` compares
  magnitudes.
- **Sharing.** A value is immutable; lists and records share their storage between copies, so the
  copy every reactive read makes costs a reference count.
- **JSON form** (`valueFromJson`, `valueToJson`). Structural: an object of exactly `num`, `den`,
  `dp` is a decimal, with `unit` a quantity — the wire form of `Rational`. A number with a fraction
  or an exponent is refused (an exact value is never a double), as is an integer beyond int64 or a
  zero denominator. A timestamp has no structural form; `valueFromJson(json, type)` reads it from
  ISO-8601 text when the type says `timestamp`, widens an `int` to a decimal when the type says
  `decimal`, and checks the result against the type.
- **Types** (`ui::Type`, `parseType`): `bool`, `int`, `string`, `decimal`, `quantity`, `timestamp`,
  `key` (an `int` or a `string`), `record`, `list<T>`, each optionally `T?`, and `any` for what a
  document cannot know — a query's result takes its type from the action schema, which the document
  never restates. `Type::admits` decides whether a value is of a type: `null` only for `T?` and
  `any`, an `int` for `decimal`.

## The document

`ui::Document` is a screen, or the app document (id `app`, spec 6 §5), which has no view and may
declare the app-only customization kinds and `formatting`: a literal record `{"date": "dd-MMM-yyyy", "dateTime":
"dd-MMM-yyyy HH:mm"}` of the patterns every date the client shows is written and read in ([`expressions.md`, Date
patterns](expressions.md)). Each pattern is checked where it is declared, and a screen that declares `formatting` is
refused: a lab writes dates one way everywhere, so the pattern is the application's, not a node's.

| Member | JSON key | Notes |
|---|---|---|
| `vocab` | `vocab` | `ui/1` (`kVocabulary`) |
| `id` | `id` | Non-empty |
| `title`, `identity`, `dirty` | same | Expressions; `dirty` is a condition |
| `titleSpans`, `eyebrow`, `eyebrowSpans`, `subtitle`, `subtitleSpans`, `status`, `tools`, `actions`, `menu`, `menuLabel` | same | The screen's head beside its title, in the screen scope ([The head](#the-head)); a screen's only |
| `leadPlacement`, `titleRole`, `headSurface`, `pageSurface` | same | Where the head's lead sits, the text role of its title (`heading`, or `headline` for a record's identity), the surface the shell draws its top bar on while the screen is shown, and the surface it draws the page area on, in the screen scope ([The head](#the-head)); a screen's only |
| `principalPlacement` | `principalPlacement` | Where the shell shows who is signed in: `sidebar` (the default, at the sidebar's foot) or `head` (at the top bar's trailing end); a literal name, the app document's only, and any other name is refused at load |
| `navigation` | `navigation` | The counts on the navigator's entries, in the app scope ([The app document's navigation](#the-app-documents-navigation)); the app document's only |
| `shell` | `shell` | The shell's declared parts ([`shell_parts.md`](shell_parts.md)), lowered to queries and state of the app scope named `shell…`; the app document's only, and a screen that has the key is refused |
| `params` | `params` | Name to type, in declaration order |
| `scope` | `models`, `state`, `let`, `queries`, `mutations`, `watch`, `checks` | The screen scope's declarations |
| `lifecycle` | `onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange` | Command lists |
| `keys` | `keys` | Chord to command list |
| `showErrors` | `showErrors` | `touched`, `submit` or `always` |
| `prefetch` | `prefetch` | Screen queries started before the screen mounts |
| `components` | `components` | Name to `Component` |
| `customize` | `customize` | Name to `CustomizePoint` |
| `view` | `view` | The root `DocNode`; required on a screen |
| — | `knownVersion` | A custom screen's: the base version it was last saved or updated against ([Element versions](#element-versions)); a version, or the document is refused at `/knownVersion` |
| `warnings` | — | What validation found that does not refuse the document ([Warnings](#warnings)) |

A key a client does not know is ignored, so additions within a vocabulary are compatible; a key whose
name begins with `!` is must-understand, and an unknown one refuses the document wherever it
appears (spec 5 §12).

**No refresh keys.** A document declares no `invalidates` or `patch` on a mutation and no `refreshOn` on a query. An
action declares what it changes in its effects, and every query holding a changed record refreshes, on every client,
so a document that restated it per screen could only drift from the action. A document that declares one is refused
rather than having it ignored: the author meant a refresh the client would silently not do. `optimistic`,
`refreshEvery` and the `refetch` command stay; `refetch` is how a document refetches for a reason no action declares.

### The head

A screen's head is what a shell shows above it beside the title: `titleSpans` (an expression of runs, as a `text`'s
`spans`: a shell draws them in its top bar in place of `title`, which stays the tab's caption, the navigator's entry,
the window's title and what an older shell draws; the runs are not compared with `title`), `eyebrow` (an expression, the line above the title) with `eyebrowSpans` (runs, drawn in place of `eyebrow` by a shell
that knows them; a head with only an eyebrow shows), `subtitle` (an expression), `subtitleSpans` (an
expression of runs, as a `text`'s `spans`: a shell that knows it draws the runs in place of `subtitle`, and `subtitle`
beside it is what an older shell draws), `status` (parts
`headBadge`: `{tone, text, icon, visible}`), `tools` (parts `headTool`), `actions` (parts `actionButton`) and its own
`menu` (parts `headMenuItem`), with `menuLabel` (an expression, the menu trigger's accessible name; default `More
actions`). All are evaluated in the screen scope, and the app document, which has no view, refuses them. A shell
draws them in that order on one line: the eyebrow above the title, the subtitle, the badges, then at the trailing edge the tools, the actions and
the menu.

`leadPlacement` (an expression of a name) says where the lead, the subtitle and the badges together, sits:

- `below`, the default: under the title.
- `inline`: on the title's line, right after the title; where it does not fit it wraps to a line of its own before the
  trail does. In the workspace's own head, which carries the title in the tab bar, it draws as `below`.
- `end`: at the trailing edge of the head's first line, after the tools and before the actions and the menu.

A literal other than these three is refused at load (`` `leadPlacement` is `below`, `inline` or `end` ``); a computed value
that names none is reported once at its path and taken as `below`. The placement may change at run time, so a width
class can pick one.

`titleRole` (an expression of a name) is the text role of the top bar's title: `heading`, the default, or `headline`,
which sets a record's identity a step larger (`font.size.headline`). A literal of any other name is refused at load
(`` `titleRole` is `heading` or `headline` ``); a computed value that names neither is reported once at its path and
drawn as `heading`. Only the top bar's title reads it: a tab's caption, the window's title and an overlay's title keep
their own. It holds for `titleSpans` too, whose runs sit inside the role. `text`'s `role` also accepts `headline`.

`headSurface` (an expression of a surface's name, `null` or absent for none) is the surface the shell draws its top bar
on while the screen is shown; a name that is no surface draws as none does. The built-in `toolbarJoined` surface wins
over it while the page continues the head's block.

`pageSurface` (an expression of a surface's name, `null` or absent for the built-in `page` surface) is the surface the
shell draws the page area on while the screen is shown, so a screen can sit on a white page at one width and on the
grey one at another (`cond(compact, "plainPage", null)`); a name that is no surface draws as `page` does. It is a
screen's member, as every head member is: the app document refuses it.

An `actionButton` part also takes `iconOnly` (a condition): while it holds, the button draws its `icon` alone and its
label is its accessible name and its tooltip. A part with `iconOnly` and no `icon` is refused (`` an icon-only action
names its `icon` ``). The same part in a dialog's or drawer's `actions` draws its `icon` too, and takes `iconOnly` alike.

### The app document's navigation

The app document's `navigation` is a list of `navigationBadge` parts, `{screen, text, tone, visible}`: a count a
shell puts on the navigator's entry of a screen, such as the worksheets waiting for review. `screen` is the screen id
the entry opens, a non-empty literal (`a navigation badge names the screen of its entry`); `text` and `tone` are
expressions and `visible` a condition, all evaluated in the app scope, so a count follows the app state and queries
it reads. A screen that declares `navigation` is refused (`` `navigation` is the app document's; a screen has none
``): the navigator lists the application's screens, and no screen owns its entries.
`AppRoot::navigationBadges()` answers the visible ones ([interpreter.md](interpreter.md#the-app-root-scope)).

A `headTool` part is a search field or a select the shell draws in the head, as data the screen evaluates: no node of
the screen is mounted in the shell's widgets.

| Key | Kinds | Shape | Notes |
|---|---|---|---|
| `id` | both | name, required | unique among the tools |
| `kind` | both | text, required | `search` or `select` |
| `label` | both | expression, required | the accessible name; the head draws no caption |
| `value`, `placeholder` | search | expressions | the text it shows, and what shows while it is empty |
| `onChange`, `onSubmit` | search | commands | after each user edit, and on Enter; `event` is the text |
| `options`, `selected` | select | expressions | as a `select`'s; `options` is required |
| `style` | select | text | `dropdown`, the default and the only one |
| `onChoose` | select | commands | after the user chooses; `event` is the chosen key |
| `width` | both | literal integer, 1 or more | in units; default 30 for a search, the content's for a select |
| `visible`, `enabled` | both | conditions | default `true` |

A tool is controlled: it shows `value` or `selected` and reports the user's edit through its commands, as a bound
input does; it changes only when the document's binding does. A key of the other kind is refused.

A `headMenuItem` part takes a `menu` entry's keys (`label`, `icon`, `keys`, `checked`, `enabled`, `onClick`,
`items`, `separator`, `description`, `visible`; `items` are head menu items too) and `id`, a name. An entry that is
chosen, neither a separator nor a submenu, has an `id`, unique across the whole menu, which the shell names it by
(`ScreenHead::choose`); a separator or a submenu takes none. The shell draws the menu after the actions as one
icon button (`more`) that opens the entries; the head's actions are never moved into it, so an author who wants an
action there on `compact` writes it in both with opposite `visible` conditions.

An `actionButton` part is one button of a list:

| Key | Shape | Notes |
|---|---|---|
| `id` | name, required | unique within its list |
| `label` | expression, required | |
| `icon` | expression | an icon name |
| `emphasis` | text | `normal`, `primary`, `danger`, `ghost`, as a `button`'s |
| `enabled`, `visible` | conditions | default `true` |
| `onClick` | commands | run in the scope the list belongs to |

### The app document's tours

The app document's `tours` is a list of guided tours ([tours.md](tours.md)). A tour is `{id, screen?, title,
summary?, steps}`: `id` is `[a-z0-9-]+`, at most 64 characters, unique among the tours; `screen` names the screen the
tour belongs to (without it the tour is general); `title` and `summary` are text expressions in the app scope. `steps`
holds one step or more and at most 50. A step is `{title, body?, screen?, params?, target?, advanceOn?}`: `title` and
`body` are text expressions in the app scope; `screen` is a screen id the step opens before it shows, with `params` a
record of literal values; `target` is a test id; `advanceOn` is `{"screen": id}`. Unknown members are ignored and a
member starting with `!` is refused, as everywhere in ui/1. A screen that declares `tours` is refused
(`` `tours` is the app document's; a screen has none ``). Whether the screens, targets and ids a tour names exist is
checked when the bundle is built ([service.md](service.md#the-registry)). `AppRoot::tours()` answers them
([interpreter.md](interpreter.md#the-app-root-scope)).

## Declarations

A scope's declarations are a `ScopeDecl`, each kind in declaration order.

| Declaration | JSON | Model |
|---|---|---|
| Model alias | `"alias": {"type": "T", "instance": "private" \| "shared" \| e \| {"from": alias}}` | `ModelDecl`, `InstanceMode` `Default`, `Private`, `Shared`, `Keyed` (a key expression) or `Borrowed`; `"shared"` on a model the catalog does not mark stateless is refused |
| State | `"name": {"type": "T"?, "init": e?}` | `StateDecl`; without a type, the type is the init's, and a declared `any` is a type: the state stays `any` beside its init (`StateDecl::typeDeclared`) |
| Derived | `"name": e` under `let` | `LetDecl` |
| Query | `{"model": alias, "action" \| "evaluate": id, "body", "when", "debounce", "refreshEvery", "staleTime"}` | `QueryDecl`; exactly one of `action` and `evaluate` |
| Mutation | `{"model", "action", "body", "concurrency", "serialKey", "optimistic", "onSuccess", "onError", "offline"}` | `MutationDecl`, `Concurrency` |
| Optimistic write | `{"query": q, "at": member, "key": e, "with": e}` under `optimistic`, read against the run's body bound as `body`; `at` is optional and not empty | `OptimisticDecl` |
| Watch | `{"on": e, "source": "any" \| "user", "do": commands}` under `watch` | `WatchDecl`, `WatchSource` |
| Check | `{"id", "assert": e, "message": e, "targets": [ids], "blocking": bool}` under `checks` | `CheckDecl` |

An `instance` string other than `private` and `shared` is a constant key. Durations are
non-negative integers of milliseconds. `serialKey` is valid only with `"concurrency": "serial"`, and reads the run's body as `body`
(`event` is not in scope there: a run started by a button and one started by a drop key the same).

## Commands

A command list is an array of commands, or one command object. `Command` carries every command of
`ui/1` (spec 5 §8 and spec 6) as a `CommandKind` and a few generic members:

| Member | Holds |
|---|---|
| `target` | The name acted on: `set`'s `name.path`; the state of `reset`; the mutation of `run`, `signIn`, `upload`; the query of `refetch`; the element of `open`, `close`, `focus`; the screen of `navigate`; the name of `host`, `switchBackend`, `setLocale`; none for `setContext`, whose `value` is the option's value; the point of `resetPrefs`; the state a `download` to preview binds |
| `value` | `set`'s value; `setContext`'s option value; `run`'s body; `navigate`'s params; `return`'s value; `host`'s args; `notify`'s message; `if`'s condition; the file of `upload` and `download` |
| `extra` | `run`'s `each`; `navigate`'s `route`; `download`'s suggested `name`; `focus`'s `row` |
| `binding` | `run`'s and `upload`'s `as` |
| `option` | `navigate`'s `as`; `notify`'s `role`; `download`'s `dialog` or `preview` |
| `transfer` | `upload`'s `id`, whose `progress`, `pending` and `error` are element values |
| `bytes` | `download`'s `"from": "bytes"` and `upload`'s `"into": "bytes"` (default `ref`): the file moves inline, under the session's cap |
| `mediaType` | `download`'s `type`, the media type of inline bytes |
| `nameAs`, `typeAs` | `upload`'s body members for an inline file's name and media type |
| `commands`, `alternative` | `seq` and `all` steps; `if`'s branches; `navigate`'s `onResult` and `onCancel` |

`setContext` is the one command the shell handles: `{"setContext": e}` asks the shell to make the value of `e` the scope
its switcher holds ([`shell_parts.md`](shell_parts.md#changing-the-context)). It runs when the value is one of the
switcher's current options, after the person agreed to discard the unsaved work of open tabs, and answers as a failed
command otherwise, reported as ``setContext was refused`` with its reason. `{"can": "context"}` is true exactly while a
switcher with at least one option is installed. The command is rebuilt with each session's frame: the shell installs the
handler when it builds the frame and takes it away with it, so nothing is registered for the process.

`run`'s `as` names the element of `each` inside its body when `each` is given, and the reply for
the later steps of the enclosing `seq` otherwise. `if` takes two or three parts: a missing else
runs nothing.

## View nodes

**A bar that stays in view.** ui/1 has no screen footer: a `column` sized to stretch in height, holding a `scroll`
sized to stretch in height between its bars, keeps the bars at the column's ends while the content scrolls (the
scroll takes the column's spare height, not its content's). The same column gives a bar at the top, or both. As a
screen's root it fills the page; the author API writes it as `pinned`, which draws each bar as a `column` of its nodes
padded by step 2 above and below and by the page's step (4) at its sides on the `bar` surface inside a `divided`
column that is marked `pageEdge`, with the scrolling body in a column padded by that step; the bars' surfaces and rules
span the column while their content lines up with the body's, and the shell, which follows the shown root's `pageEdge`,
does not inset the page around it. When the top bar always shows (it has nodes and none has a `visible` condition) the column also says it is `headJoined`: the bar continues the head's block, and the divided column's rule under the bar is the block's. A hand-made column draws its bars as written. A splitter pane may be pinned: a stretching
splitter gives each pane its height, and a pinned pane keeps its bars at the pane's ends.

**A document page.** `page`, `pageRegion` and `localeScope` lay a fixed-size sheet out in millimetres and format a subtree
in another locale. `page` takes `width`, `height` and `margins`, `pageRegion` takes `anchor`, `x`, `y`, `width` and
`height`, and `localeScope` takes `locale`; the common keys each honours and the refusals the loader makes (a non-region
child of a `page`, a region outside a page, a pinned region with an offset, a region leaving the sheet, an empty margin
box, any layout key a sheet sizes itself without) are in [`document_page.md`](document_page.md#the-document-form-and-the-author-api).

A `DocNode` is a node or a structured part of one. Its properties stay keyed by their document
keys — `props` (expressions), `events` (command lists, a custom node's events as `on.<event>`),
`children` (node lists), `parts` (records such as columns and tabs), `names` (plain strings) and
`keys` — so the interpreter reads a kind's properties by name. One table in `src/ui/document.cpp`
(`kKinds`, reached through `detail::findKind`) says, per kind, which keys it has and what shape each takes; it is the only
place a kind is described.

**Every node** takes `id`, `visible`, `enabled` (conditions), `tooltip`, `sizing`, `span`,
`minWidth`, `maxWidth`, `minHeight`, `maxHeight` (literal integers of 0 or more, in backend units: the node's extent
on that axis is clamped into them, whatever its sizing), `alignSelf` (`start`, `center`, `end`, `stretch`: where the
node sits across its `row`'s, `column`'s, `grid` row's or `forEach`'s axis, in place of the container's `align`),
`bleed` (a literal boolean, default `false`: the node's sides lie at its parent's edges, past the parent's horizontal padding, while its siblings stay inside it; only across, so its place along the parent is its siblings'; it reaches the padding of the parent the document names: a column the interpreter inserts around several children of a panel or a tab page is transparent to it, while a column the document wrote reaches only its own padding, and its natural width adds nothing to the parent's. A tab page's padding is its tabs' `pageInset`, else their `inset`; a bleeding child of a page whose literal padding step is 0 or absent is refused at load, as it has no padding to reach past (a computed step is not checked). The loader refuses it on the view root and on a child of anything but a column, a panel or a tab page: a row's or a grid's child has neighbours at its sides),
`autofocus`, `testId` (strings), `surface` (an expression of a surface's name, so it follows the data; a plain
string is a literal one: a value that names no surface draws as no surface does, `null` is no surface, and any other
value that is not a string is reported once at the node's `surface` path and drawn as no surface), `a11y`
(`{name, role}`), `drag` (`{payload}`), `drop` (`{accepts, onDrop}`) and `keys`. `sizing` is the width's —
`"content"`, `{"lit": {"fixed": n}}`, `{"lit": {"stretch": w}}` (in a row or a column the stretching children share the length left by weight, from zero, so two of weight 1 are equal whatever their content) or `{"lit": {"percent": n}}` (n from 1 to 100: that share of the width inside the padding of the row or horizontal splitter the node is in, gaps and handles not taken off first, so a pane is usually a percent with a `maxWidth`) — or both dimensions as a record of them, one
left out being content: `"sizing": {"lit": {"width": {"stretch": 1}, "height": {"stretch": 1}}}`. A node's sizing means the
same inside a `switch` case, an overlay or a row as at the root: the content's stretch reaches the container it is
mounted in, and several nodes in a case or a default are spaced as the vertical list around the switch spaces its nodes. **Text and the inputs** also take `readonly`, `required`, `stale`
(conditions) and `errors`. **The six inputs** also take `description`, a line drawn under the control and announced
as its accessible description, and `tone` (`ok`, `warn`, `err`, `info`; `neutral` or `null` for none), an evaluation
of the value that outlines the control and colours the description and, unlike `errors`, does not make the value
invalid; a tone of no known name is reported once and drawn as none. They take `labelHint` too, a hint drawn after the
caption and its required mark ("Temperature on arrival * · 2–8 °C"), only while the input shows a caption. A `text`
refuses `description` and `labelHint`: a text is not described and has no caption, so the line goes in the text. A `text`'s `tone` is its colour, whatever its role: the role keeps the size,
weight and face, so `{role: display, tone: err}` is a large red figure.

| Kind | Keys (a ⓘ key is in the node's own scope) | Element values beyond `visible`, `enabled`, `valid`, `errors`, `shownErrors` |
|---|---|---|
| `text` | `text`, `spans` (runs of emphasis, below), `tone`, `align` (a literal name: `start`, the default, `center` or `end`; it places each line of the text within the text's own width, so it shows on a text that stretches or fills a table column and nothing on a text as wide as its content; under a right-to-left layout `start` and `end` swap; a text in a table that gives `align`, `start` too, keeps it over its column's; a name that is none of the three, `stretch` included, and a computed one are refused at load), `role` (`normal`, the default, `muted`, `heading` (`title` is accepted as its alias), `section`, `error`, `success`, `mono`: an identifier, in the theme's fixed-pitch family at `font.size.mono`, always on one line and cut at its end with an ellipsis where it does not fit, its full text then the tooltip unless the node gives its own (a mono run inside `spans` is not a mono text: rich text wraps with its sentence); `display`: a figure that stands out, such as a KPI's number, at `font.size.display` and the strong weight; `small`: a footnote or timestamp, at `font.size.small`; `overline`: a short label over a group, drawn as `section` does (body size, strong, muted) unless the theme's `font.overline.case` is `uppercase`, which draws it at the small size, strong, muted, in letter-spaced capitals. Write the text as it reads, "Record type": the frontend draws the capitals and the accessible name stays as written; `caption`: a note under a control or a figure, at `font.size.caption`, regular and muted, which the terminal draws as `muted`; `label`: a field's or a group's caption, at `font.size.caption`, regular, in the text colour, which the terminal draws as `normal`. A table column's `role` takes them too) | |
| `button` | `label`, `icon`, `emphasis` (`normal`, `primary`, `danger`, `ghost`, `dangerOutline`: a destructive action that is not the main one, outlined with its label in the error colour), `style` (`push`, the default, or `item`: an entry of a list), `selected` (a condition: an item is the current entry; a push button is pressed in), `shape` (`default`, `round`: corners of half the height, at least as wide as tall and never drawn taller than wide, or `square`: a button for a glyph, with the theme's corners, no least width and little side padding, at least as wide as tall, and a strong caption at the body size; `square` with `style: item` is refused, and a badge has no `square`), `size` (`normal`, or `small`: a compact caption, the least side padding and a strong label in the small type step, at the theme's `button.smallHeight`, which may be above or below the control height; without it the control height), `mono` (a literal flag: the label in the `mono` text role's face; `shape`, `size` and `mono` are literal names and a literal flag), `weight` (a literal DTCG font weight name, such as `semi-bold` (600) or `bold`: the caption's weight in place of the theme's `font.weight.button`; a name that is none is refused), `onClick`, `onActivate`, `children` (only with `style: item`: the entry's content, drawn in place of its label, which still names it; display kinds only at any depth — `text`, `badge`, `keyValue`, `progress`, `steps`, `busy`, `spacer`, `column`, `row`, `grid`, `forEach`, `switch` — and no `keys`, `drag`, `drop` or `autofocus` there: "an entry's content is read-only; put the control beside the entry") | |
| `textInput` | `value`, `placeholder`, `mode`, `icon` (an icon of the theme's icon set, drawn at the leading edge), `unit` (text, drawn muted at the trailing edge inside the field and announced after the field's description, such as `°C`), `prefix` (text, drawn muted at the leading edge inside the field, after the icon, and announced before the field's description, such as `±`), `decimals` (an integer expression, 0 or more: the places a number is entered with, which the frontend reads, and a text with more fraction digits than the places is invalid with the error `Too many decimal places`, through the same translation key and fallback as a `form` field's (`morph.forms.error.too-precise`); it follows its expression, so a change of places with text already typed keeps the text and checks it again; a value that is not a non-negative integer means no places, -1; one is refused beside a literal `mode: multiline`, and a computed one on an input that turns out multiline is reported once and left out), `emphasis` (`normal`, the default, or `prominent`: the screen's focal field, such as a scanner's, which the frontend draws so that it stands out), `align` (`start`, the default, `center` or `end`: where the text sits in the field; a literal name, and an `end` input draws its digits at one width each) — `unit`, `icon`, `prefix`, `emphasis` and `align` refused beside a literal `mode: multiline`, and a computed mode that turns out multiline is reported once and its unit, icon and prefix are left out, `mono` (a literal flag: the text and the placeholder in the `mono` text role's face; also on a multiline input), `wrap` (a literal flag, default `true`, only on a multiline input: with `false` a line longer than the field stays whole and the field scrolls sideways, as source text is read; refused beside a literal `mode` that is not `multiline`, and left out of a computed mode that turns out not to be multiline), `commitOn` (`change`, `blur`, `enter`), `label`, `showLabel` (a literal `bool`; default `false`), `onChange`, `onCommit`, `onSubmit` | `value`, `draft`, `dirty`, `focused`, `touched` |
| `checkbox` | `label`, `checked`, `onToggle`, `onChange` | `checked` |
| `select` | `options`, `selected`, `style` (`dropdown`, the default, `radio`, or `segmented`: every option one segment of a row, one marked; a constant is checked at load, and a computed one that names none is reported once and drawn as a dropdown), `label`, `showLabel`, `onStale` (`clear`, `keep`), `onChoose`, `onChange` | `selected`, `selectedOption` |
| `menu` | `items`: parts `{label, icon, keys, checked, enabled, onClick, items, description, visible}` (`description` a muted line of help with the entry; `visible` a condition, a hidden entry left out), or `{separator: true}` alone, a line between entries; `style` (`bar`, the default, or `button`: one button that opens the entries); for `style: button`, the trigger's `label`, `icon` and `emphasis` (as a button's) | |
| `group` | `label` (the legend, drawn in the `label` text role), `required` (the legend's mark only: a group has no value, so no `Required` is derived from it), `description` (a line under the children), `errors` (as on an input), `gap`, `children` | |
| `column`, `row` | `children`, `gap`, `padding` (a spacing step, 0 to 5, that insets the children from every edge, default 0; or the constant list `{"lit": [vertical, horizontal]}` of two steps, each 0 to 5, for the ends and the sides apart; any other `lit` list is refused at load, a computed padding is one step, and one that yields a list is reported once and taken as 0), `pageEdge` (a literal boolean, default `false`: the stack insets its own content, so as the screen's shown root it lies at the page's edges and the shell does not inset the page; it means nothing anywhere else, and the loader does not refuse it there), `headJoined` (a literal boolean, default `false`, which needs `pageEdge` on the same stack and is refused without it: the stack's first child continues the head's block. As the shell's shown root the shell then draws its top bar on the `toolbarJoined` surface, with no rule under it, and the first child draws the rule under the whole block: `pinned`'s divided column draws it under its top bar, a `tabs` strip its own, and a hand-made root is `divided`), `selected` (a condition: this stack is the chosen entry of a list, as a check box inside it says. While true it draws the accent's 3 px bar at its leading edge, down its whole height, over its surface and under its children, clipped to its own surface's shape; there is no fill, so a `surface` chosen by condition and the bar compose, and no click: the control inside is the one target. The bar lies inside the stack's leading padding, so a stack with `selected` and no leading padding draws it over its first 3 px: give it padding. The stack becomes a `Grouping` for assistive technology unless it names a role, reports `selectable` and follows the condition as `selected`; name it, as an unnamed group is one screen readers pass over. A `grid` refuses it), `align` (across the axis: `start`, `center`, `end`, `stretch`, the default), `justify` (the spare length along the axis while no shown child stretches: `start`, the default, `center`, `end`, `between`, in equal gaps never less than `gap`); a `row` also takes `wrap` (a literal boolean, default `false`: the children continue on the next line when they do not fit, as the `StackWidget` row of the backend contract says; a `column` has no `wrap`, and the loader refuses it there); `divided` (a literal boolean, default `false`: a 1 px rule in the theme's divider colour is drawn between each two shown children, centred in the gap and taking 1 px of its own, so a gap of 0 still separates them; a column's rule is horizontal and a row's vertical, the line's height; the terminal draws none; the loader refuses it on a `row` that also has `wrap`, since a rule between lines and within them would be two rules) | |
| `grid` | `children`, `gap`, `columns` (that many equal columns; a content-sized grid is as wide as its widest cell times the columns), `tracks` (in place of `columns`; parts `{width, align}`, one per column: `width` a sizing, default `{"stretch": 1}`, as a table column's; `align` `start`, the default, `center` or `end`, where a text's text and a label-width cell sit across the column, as a table column's `align`; a container cell fills it), `align` (each cell's content in its row, as a row's), `minColumnWidth` (a literal integer, 1 to 64 units: the grid shows as many of its `columns` as fit that width, at least one, so its columns follow the room the grid is given, not the window; it needs `columns` (absent, or a literal 1, has nothing to drop) and refuses `tracks`, which size their own columns; a computed value is refused. The grid's height follows the columns shown) | |
| `spacer` | | |
| `panel` | `title`, `header` (nodes, shown beside the title, usable while collapsed), `padding` (one spacing step; a list is refused at load), `gap` (the spacing step between several `children`, default 2, read once; a single child has none to space), `collapsible`, `collapsed`, `onToggle`, `children` | |
| `scroll` | `axis`, `children` | |
| `switch` | `on` (required), `cases`: parts `{when, scope, children ⓘ}`, `default` | |
| `tabs` | `tabs`: parts `{id, label, children}`, `selected`, `onChoose`, `moreLabel` (an expression, default `More`, translated with `t` and in English: the label of the menu a frontend may draw for the tabs that do not fit in the bar), `inset` (a literal spacing step, 0 to 5, default 0: the strip and its rule span the node's width, the first tab starts this far in from the leading edge, and each page is padded by it on every side, and the page's inset is the whole gap under the strip's rule; a computed or out-of-range value is refused), `pageInset` (a literal spacing step, 0 to 5, default `inset`: the padding of each page, apart from the strip's; each page pads its own content inside its own scroll, so a `bleed` child of a page reaches the node's edges whether or not the page scrolls; a computed or out-of-range value is refused), `stripSurface` (a surface name: the strip, and nothing else, is drawn on that surface across the node's width; with a `pageEdge` column as the root, a tabs node with both is a page whose strip sits in the head's block and whose content is inset below its rule); a tab's `label` follows its expression, and the list of tabs is fixed (a tab takes no `visible`). `selected` is the id of the tab shown, controlled as a select's: a pick runs `onChoose` with the tab's id as `event`, and the page follows `selected` alone; a literal `null` is no binding. Without it the node keeps the user's pick, so picking a tab mounts and shows its page. A relabel keeps the pick. A command chooses a tab by setting the state `selected` reads; `set` never names a tabs node | `selected` (the shown tab's id: the bound one, else the pick, else the first tab's) |
| `dialog`, `drawer` | `open`, `gap` (the spacing step between several `children`, default 3, read once), `title`, `description` (a line in the head under the title, ruled off from the body, which does not scroll with the content and is the overlay's accessible description), `descriptionSpans` (the line as runs, as a `text`'s `spans`: a client that knows it draws them in place of `description`, and the accessible description is their characters joined), `showErrors`, `side`, `scope`, `children` ⓘ, `default` ⓘ, `cancel` ⓘ, `actions` ⓘ (parts `actionButton`, [The head](#the-head)), `onDismiss`; a dialog's `sheet` (a condition: while true the dialog is drawn as a sheet that fills the window, with a head of a dismiss control and the title centred on the sheet, ruled off from the body, the document choosing by the width class, as `{"fn": "eq", "args": [{"env": "widthClass"}, "compact"]}`) and `dismissLabel` (the dismiss control's label, default `Close`, translated with `t`; only with `sheet` or `closeButton`); `headNote` (a muted note on the title's row, against its trailing edge) and `footerNote` (a muted note leading in the footer's row, opposite the actions, which shows the footer while it is not empty), both expressions, empty showing none; `closeButton` (a condition: while true a dismiss control labelled `dismissLabel` is drawn on the title's row, against the trailing edge; a sheet's head holds one already, so it adds nothing while `sheet` holds); the dismiss control dismisses exactly as Escape does, and a drawer takes no `sheet` | `open`, `ready`, `submitted` |
| `busy` | `active`, `label` | |
| `forEach` | `rows` (required), `key` ⓘ, `axis`, `gap`, `wrap` (a literal boolean, default `false`; the rows continue on the next line when they do not fit; the loader refuses `true` where a literal `axis` is not `horizontal`, and a computed `axis` that is not horizontal at lowering is reported and the rows do not wrap), `divided` (as a column's, for a vertical `forEach`; the loader refuses it where a literal `axis` is `horizontal`), `scope`, `children` ⓘ | |
| `table` | `rows` (a list the table orders on the client: one of more than 20,000 rows, or of more than 1,000,000 cells, rows times the column parts, is still ordered and is reported once for the table, "table <id> holds <n> rows; a table over rows is ordered on the client up to 20,000 rows or 1,000,000 cells — serve it as a model's table"), with `status` and `persist` as a model-backed table takes them, except that its status line takes space only while it has a message unless `status` is given, and that one with no `id` keeps no state, a `persist` other than `false` on it being refused ("a table over `rows` needs an `id` to persist its arrangement"); or `model` and `table`, one of the model's tables (`Table.<table>` and `Table.<table>.describe` are in the catalog and belong to the alias's model type, else "the table `<table>` is not in the catalog"; `table` without `model`, and `table` with `action`, are refused), with `body` (the query's fixed part), `status` (a literal flag, default `true`: the table's status line takes space; `false` sends its messages to the host's notifications) and `persist` (a literal: `true`, the default, keeps the table's sort, filters and column layout in the session's table-state store under the screen's id, a name keeps them under that name, `false` keeps none; any other value is refused, "`persist` is a literal: true, false or a name"), `editMode` (`cell`, the default: each committed cell of an editable column is sent as its own edit; or `row`: a row's committed cells are collected while focus stays in the row and sent together when focus leaves it or on `Ctrl+Enter`; refused on a table that is not a model's table, "`editMode` chooses how a model's table commits its cells; this table is not a model's table", and any other value refused, "`editMode` is `cell` or `row`, not `<value>`") and `retainChanged` (a literal flag, default `false`: a row an action of this session changed stays in the view while it no longer matches the filter, at its sorted place, drawn muted with the tooltip "This row no longer matches the filter"; the rows retained are the keys of the `ChangedRows` such an action returned for the table, at most 10,000, and an action whose rows would pass that adds none of them and says so in the status line, "The changed rows are not kept in view: a table keeps at most 10,000"; while they show, the status line stands with "3 rows shown that no longer match" and the entry "Show only matching rows", which drops them, as a change of the filter does, and ends a selection of the view; while such an action runs, the table holds back the refetches its record's change events cause, for at most the session's `retainHold` (2 s), and sends one when the action settles; retained rows are not part of the table's stored state); or `form` and `field`, a grid of a form ([`../table/editing.md`](../table/editing.md#a-grid-of-a-form)): `form` the id of a `form` node of the document and `field` the path of one of its lists of records, in place of `rows` (`form` with `rows` is refused, "a table takes `rows` or a `form`, not both"; a `form` that names no form node, or a `field` that names no list of records of the form's schema, is refused), each column part's `field` naming the member of the row type the column edits (one that names no member is refused when the document loads, "`<member>` names no member of `<field>`"), or an expression read in the row's scope (`row` the row's record) naming the member each row edits, so one column holds a choice in one row and an amount in the next (a name that is no member is reported once and leaves that cell read-only), with the overlays per row `decimals` (the entry places of the row's member, also its wire `dp`, as a form field's `decimals` overlay; an expression here, where a table over `rows` takes only a literal), `unit` (the unit the row's member is entered in, an id of its units, applied with the draft converted when the row's value of it first appears or changes, so a unit the user then picks in the editor stays), `readonly` (a condition: the row's cell is read-only), `options` (a list narrowing the options of the row's choice to those values) and `placeholder`, each an expression in the row's scope; `readonly`, `options`, `placeholder`, and `field` or `unit` as an expression, on a column of any other table are refused, "`<key>` is a grid column's, read per row; this table is not a grid of a form", or its `value` (read with `row` the row's record) a display column the walk skips and no edit reaches (a part with neither is refused, "a grid's column edits the member its `field` names, or shows its `value`"; with no column parts, a column per member in schema order), with `onActivate` run with `event` the row's key on `Enter` or a double click on a read-only cell, `layout` arranging the columns and the walk following the visible ones in layout order, an edit column sorting by its header when its part says `sortable` and filtering as a table's column does (its filter button, its values, `onFiltersChange`, the table menu's "Clear all filters") unless it says `filterable: false`, a display column doing neither, and a cell menu of "Copy", "Copy with headers", "Filter by this value" and "Exclude this value", then "Revert cell" and "Revert row", then a separator and the `cellMenu` parts, with `enterMoves` (`down`, the default, `right` or `none`: where `Enter` goes after it commits a cell) and `walk` (`rows`, the default, or `columns`: the order `Tab` visits the editable cells in), `selectionMode` `cells`, the default and only mode of a grid (any other is refused, "a grid of a form selects cells: its `selectionMode` is `cells`"; `cells` on a model's table is refused, "`selectionMode` `cells` selects cells of a table held on the client; a model's table selects rows", and on a table over `rows`, "`selectionMode` `cells` is a grid's; this table is not a grid of a form"): a rectangle of cells extended with `Shift` and an arrow or a drag, the whole grid with `Ctrl+A`, a row's cells by its header and a column's by its header's selection gesture, whose value `selection` and `onSelectionChange`'s `event` are `{"anchor": {"key", "column"}, "focus": {"key", "column"}}` or `null` (a bound `selection` is shown; `onSelectionChange` runs after each change of the selection), copied by `Ctrl+C` as tab-separated display text, pasted into by `Ctrl+V` from the focused cell (values on read-only cells, past the last column and past the last row dropped and counted in the status line, "Pasted 4 values; 2 on read-only cells were dropped"; a block of more than the session's `maxPasteCells` (10,000) values is refused, "A paste holds at most 10,000 values"), filled down by `Ctrl+D` and blanked by `Delete`, each one change of the form; or `layoutRows`, a fixed grid ([`../table/editing.md`](../table/editing.md#fixed-grids)), in place of `rows`: parts each a row `{key, label, cells}` or a block `{forEach, rows}`, with `form` the id of the form node whose fields its cells edit (absent for a grid of `value` and `text` cells only); a row's `cells` map column ids of the table's column parts to a cell part that names one of `field` (a path of the form's fields, `limits.density.min`; in a block, of its collection's row type, `mass`), `value` (an expression, read in a block with `row` the collection row's record) or `text` (a string), with `rows` and `columns` (literal counts from 1, default 1: the cell spans that many rows down and columns to the right), and a grid column's overlays, `decimals`, `unit`, `readonly`, `options` and `placeholder`, read as a grid column's (all but `readonly` only on a `field` cell, "`<key>` is an overlay of the field a cell edits; this cell edits no field"), a `unit` given when the cell is first shown, or after a load, as the field's loaded unit, and two literal units given one form field by overlays anywhere in the document (cells, or grid columns over the same member) refused ("`<field>` is given the units `<a>` and `<b>` by two overlays; a field is entered in one unit"); a row's `label` is read once, and in a block reads no `row`; a block's rows repeat for each row of the form's collection `forEach` names, each keyed `"<row key>/<key>"`, so a key holding a `/` is refused ("the row key `<key>` holds a `/`, which joins a block row's keys; give it another"); keys are unique across the grid, a block's rows included ("the row `<key>` is declared twice in the grid"); `layoutRows` with `rows` ("a fixed grid's `layoutRows` are in place of `rows`"), with `field` ("a table with `layoutRows` takes no `field`"), on a model's table, with `sort`, `group`, `summaries`, `rowSpans`, `appendRows`, `rowsFixed` or `key` ("a fixed grid's rows are fixed …; it takes no `<key>`"), with column parts that name a `field` or a `value`, a cell under an id the column parts lack, a cell naming none or more than one of `field`, `value` and `text`, a `field` cell or a block without `form`, a `field` that is no field of the form's schema, a `forEach` that is no list of records of it, and a block inside a block are refused; a fixed grid walks, selects cells, copies, pastes and validates as a grid of a form, with `enterMoves`, `walk`, `onActivate`, `selectionMode` `cells`, `selection` and `onSelectionChange` as it takes them, is never sorted or filtered, and names each row by its `label`; `appendRows` (a literal flag: `Tab` past the last cell and `Enter` below the last row add a row, and an "Add row" button under the grid, test id `<testId>_addRow`, does, disabled at the field's `maxItems` with the reason as its tooltip) and `rowsFixed` (a literal flag: no row is inserted, deleted or appended, and no entry offers it; with `appendRows` refused, "a grid with `rowsFixed` adds no rows; drop `appendRows`"), both refused on any other table ("`appendRows` is a grid's; this table is not a grid of a form"); `bands` (on any table and a field overlay's `grid` part: parts `{label, unit, columns, bands}`, `columns` a literal list of column ids and `bands` the parts one level down, drawn above the column headers, a band's unit after its label and not repeated by a column of the same unit; nested past 3 levels, a band naming no column or an id the table's column parts lack, a child over a column its parent lacks, or a column in two bands of one level is refused, naming the band's label; a model's table draws its description's bands unless it declares its own, and bands that do not fit its columns are reported and not drawn; a declared or user-formed column group draws as its one-level group line); a `rowSpans` part's `columns` (an integer from 1, default 1: the cell covers that many columns to the right in the layout, cut at a column the user moved away or hid); `summaries` (on a table over `rows`, a grid of a form and their `group` part: parts `{key, label, position, cells}`, `key` required and unique in its list, `position` `bottom` (the default) or `top`, refused otherwise, `cells` a record of expressions by column id, each column one of the table's, read with `rows` the records of the rows summarised, in view order, and `column` the column's id; a table's summaries are drawn before or after its rows (a paged table's on every page, over every row of the view) and a group's after each run, outside the order, the filter and the selection; a summary's `label` names its row; a copy whose rectangle spans a summary row copies its cells as drawn; a summary row's key is `"summary:<key>"`, a group's `"summary:<run ordinal>/<key>"`, and a summary cell's test id `summary_<key>_<column>`, a group's `summary_<run ordinal>_<key>_<column>`; a model-backed table refuses `summaries` on itself and on its `group`, "a summary row summarises rows the client holds; a model-backed table refuses `summaries`, as it refuses `group`"; a grid of a form takes a `group` too, its runs over the array's order); unless `rowsFixed`, the cell menu offers, after "Revert row" and a separator, "Insert row above", "Insert row below" and "Delete row" ("Delete rows" when the cell selection spans more rows, deleting them all), and the table menu the same for the focused row, after its own entries and a separator; `Ctrl+Shift+=` inserts a row below and `Ctrl+-` deletes; an insertion is disabled at `maxItems` ("A grid holds at most 5 rows") and a deletion where it would leave fewer than `minItems` ("A grid holds at least 1 rows"); a deletion happens at once and, when a deleted row held a value, the status line says "Deleted 2 rows" ("Deleted 1 row") with "Undo", which puts them back until the grid's next change or for 6 s, whichever ends first; `selectionMode` `cells` on a table over `rows` is refused, its lowering there deferred; or `model` and `action`, a typed list action that takes a `TableQuery` and returns a `Page<Row>`, with `body` (the request's fixed part), `mode`, `status` and `persist` as a model's table takes them: the action's request schema marks one top-level member `x-table`, the member the query is written into (else "a table over an action needs a query member marked x-table", and two marked are refused too), and the table has its `columns` parts or the action has a view document (else "a table over an action needs its columns, or a view document for the action"), since its description is built on the client from them, its key from `key`, the view document's `v-rowKey` or `id`, and each column's kind from the part's `kind`, the view document's or the first reply's rows (each rule runs when the catalog supplies `schema` and `view`); `key` ⓘ, `columns` (parts `{id, label, kind, width, role ⓘ, align, valign, sortable, locked, filterable, wrap, shrink, sortBy ⓘ, cell ⓘ, decimals, rounding, padPlaces, grouping, empty, unit, date, labels, tone ⓘ, tooltip ⓘ}`, or an expression of `{id, label, align, valign}` records; `locked` (a literal flag) keeps its column where it is declared: the user cannot move, hide, pin, unpin or resize it, and each such gesture is refused in the status line, "{label} is locked"; `filterable` (a literal flag, default `true`): `false` draws no filter button on the column's header and leaves "Filter…" out of its menu; a column part with an id is otherwise filterable (on a model's table, while its description marks it so), and its menu offers its values and the conditions of its kind; a column's display parts override the description's display member by member on a model-backed table and are its only source on a table over `rows`, and change what a cell shows, never what a sort, a filter or a selection uses: `decimals` (a literal integer 0 to 18) the places a number shows, rounded by `rounding` (`halfEven`, the default, or `halfUp`, half away from zero), padded with zeros to them unless `padPlaces` (a literal flag, default `true`) is `false`; `grouping` (a literal flag, default `false`) groups digits by the locale; `empty` the text of an empty cell (an unloaded one draws its placeholder, an invalid one an error mark); `unit` the unit the header shows after its label, `Mass [kg]`; `decimals` and `unit` on a `Measure` column are reported and ignored, since its cells show their own units; `date` a `Date` or `DateTime` column's pattern, checked as `formatting`'s are; `labels` a literal list of `{operand, text}`, the text a cell shows for the value its operand names; on a table over `rows` a cell's kind is its value's, and a quantity keeps its unit after the formatted amount; `tone` and `tooltip`, read in the row's scope on a model-backed table (a table over `rows` reports them and draws neither), the cell's tone (a tone name; any other value shows none and is reported once for the column) and its tooltip, which win over the description's companion columns, whose columns are then not fetched for them; a description's companion column is never one of the table's columns, and a part that names one is reported; `kind` is a literal, one of `text`, `integer`, `decimal`, `number`, `date`, `dateTime` and `bool` (any other is refused, "`kind` is one of text, integer, decimal, number, date, dateTime, bool"): the kind a `rows` table orders the column's cells as, a cell of another kind being invalid and sorted last, and the kind a typed list action's reply is read as; absent, the kind is inferred from the cells — `role` styles the column's default cell and has no other effect on one with its own `cell`; it takes a text's role names, and a literal one is checked the same way; a `mono` role also sets the width unit of the column's `fixed(n)`: n mono characters, plus the cell padding, with or without a `cell`, so a column of identifiers says `mono` to hold them whole; `wrap` (a literal boolean, default `true`) with `false` draws the column's text cells on one line, cut at the column's width with an ellipsis, such as a value with its unit; `shrink` (a literal flag, default off, for a column whose `width` stretches; on any other column it is reported once and ignored) lets the column go narrower than its header when the table is narrower than its columns need, down to a floor of 8 units, its header and cells then cut, where otherwise no column is narrower than its header and the table scrolls sideways, and makes it give up width before the other stretch columns (the two meanings are one flag; a column that may go under its header but keeps its content late has no spelling); a stretch `width` starts from the column's content (its header and its widest shown cell) and its weight shares only the width the content leaves, so proportions that ignore the content cannot be said, a literal `percent` column stays refused, and a width the user sets would be a fixed column; `align` (`start`, the default, `center`, `end`) places a text cell's or a text input's text, a badge, check box or push button cell, and the header label across the column, while a container cell fills it; `valign` (`start`, `center`, the default, `end`) places every cell down its row; both are literal names, and a computed column's place of no known name is reported once and takes the default), `valign` (the default of every column that gives none), `selectionMode`, `selection` (a list of row keys: bound, it is controlled, as a select's `selected` is; refused without a `selectionMode` of `single` or `multiple`, and on a model-backed table), `onSelectionChange` (after each user change, with `event` the new keys; the same rules), `selectColumn` (a literal flag, only with a literal `selectionMode: multiple`: the table draws its selection as a leading column of check boxes under a select-all box), `selectAllLabel` (the select-all box's accessible name, only with `selectColumn`; default `Select all`, translated with `t`; on a model-backed table with `selectionMode: multiple`, `Ctrl+A` and the select-all box select the view, fetching no row: a `Ctrl`+click then leaves a row out of it or takes it back, a `Shift` range does so for the range by its anchor, a plain click selects that row alone, a change of the filter ends it and a sort keeps it; it is refused in the status line while rows are undecided, "Select all is available once 40 rows are decided", or a filtered column's level is awaited, "Select all is available once recipe data has loaded"; an action refused with `morph.table.viewChanged` over such a selection is offered again in the status line, "The rows changed since you selected them: n rows now. Run on these?", with the entry "Run on these" that sends it again on the view as it is), `rowLabel` ⓘ (each row's accessible name, which its box carries; default the row's key as text), `sort` (a list of keys `{"column": <a sortable column's id>, "direction": "asc" or "desc"}`, most significant first, `[]` the source order: the table orders its rows by it; bound, it is controlled, as `selection` is, and unbound the table holds it from `[]`; a literal one names sortable columns, each once; a column's `sortable`, a literal flag that needs its `id`, lets its header build the sort chain: a click sorts by it alone, ascending, or descending when it already is the first key ascending; `Shift` with the click adds it as the last key, ascending, or flips its key's direction in place when the chain holds it; its header menu's "Sort ascending", "Sort descending", "Add to sort" and "Remove from sort" do the same without a modifier; on a model-backed table the description says which columns sort; its `sortBy`, only with `sortable`, is the value the rows order by, read per row as `key` is, default the row's field of the column's `id`), `onSortChange` (after a sort gesture, with `event` the new sort; on a table over `rows` it needs a sortable column), `maxSortKeys` (a literal integer 1 to 32, a `rows` table's only, default 8: the most keys the sort chain holds, which the header and menu gestures are held to; a key past it is refused in the status line, "A table sorts by at most {n} columns", and the chain is kept; on a model-backed table it is refused, since a model's table carries its bound in its description, from its `limits`), `layout` (the column layout `{"order": [id…], "hidden": [id…], "pinned": n, "widths": {id: units}}`: bound, any expression but a literal `null`, it is controlled, as `sort` is, and the stored layout is neither applied nor saved; unbound the table holds it from its declared columns, and a model-backed table's columns the description does not show start hidden; on a table over `rows` a derived `columns` expression is read once, when the table is made, for the layout's columns, and a stored layout is fitted against the column parts alone, so a derived column it places is kept as stored rather than applied; a column with no `id` is named `#<n>` in `layout`, `visibleColumns` and the stored layout, `n` its place among the table's columns from 0, derived columns first, and it is locked: the name follows the place, so a column inserted before it gives a stored `#<n>` to another column), `onLayoutChange` (after a user's change of the layout, with `event` the new layout), copying (`Ctrl+C` and the table menu's "Copy" and "Copy with headers" copy the selected rows, or with none the focused cell, as tab-separated text of what each cell shows, for at most the session's `maxCopyRows` (10,000) rows; a larger selection is refused in the status line, "A copy holds at most 10,000 rows", and copies nothing), saved views (a table that keeps a state has `Views` in its table menu: the user's own views, each applied with its layout, sort and filters and marked `{name} (modified)` once the arrangement differs from it, "Save view…" (a name the user already has replaces that view), "Rename view…" (onto a taken name refused, "A view named {name} exists"), "Remove view", "Set as default" and "Start from the default view"; with no stored arrangement the default view is applied at mount, and with "Start from the default view" its sort and filters are applied at every mount over the stored arrangement's; over a store that shares, the shared views under "Shared views", "Save as my view" and "Publish view…"), `configurable` (a literal: `true`, the default, `false`, or a list of `"reorder"`, `"hide"`, `"pin"` and `"resize"`, what the user may change about the columns; any other value is refused, "`configurable` names `<name>`; it lists reorder, hide, pin and resize"; `false` offers no header or table menu; a gesture the list does not allow is refused in the status line, "This table's columns cannot be moved" (`hidden`, `pinned`, `resized`), and a table that does not allow `resize` has no header edge to drag), `onFiltersChange` (after a user's change of the filters from the header's filter menus or "Clear all filters", with `event` the new filters; the table holds the change and keeps it in its persisted state unless the document binds `filters`; a table with `configurable: false` has no menus but keeps its filter buttons), `groups` (a table over `rows`'s only, refused on a model-backed table, whose description declares its groups: parts `{id, label, columns}`, `columns` a constant list (`{"lit": [...]}`) of 2 to 16 of the table's column part ids of one literal `kind`, each column in at most one group, else "a group has 2 to 16 columns", "a group's columns are the ids of the table's column parts", "the column `<id>` is in another group" or "a group's columns hold one kind of value"; a group's values list is pooled over its columns, its filter names it by `id`, and its header stands over its columns while they stand side by side), `pageSize` (a positive integer: the table shows one page of that many of its sorted rows; absent or `null`, every row; a literal below 1 is refused), `group` (a part `{by ⓘ, heading ⓘ}`: a heading row before each run of adjacent rows whose `by` values are equal, drawn from `heading` in a row scope whose `row` is the run's record `{key, count, first, last, items}` of the whole group; a heading spans every column, is never selected, marked or activated, and counts in no element value; with a sort, each group keeps its place and its rows are sorted within it, and a group split across pages gets its heading on both; refused on a model-backed table; a heading's key is reserved, `"\u001Fgroup:"` then the canonical JSON of its run's key, `":"` and its ordinal among the runs with that key, and a data row with a field `"\u001Fgroup"` is taken for a heading), `page` (the page shown, from 0, clamped to the last; absent or `null`, the first; needs `pageSize`; `pageSize` and `page` are refused on a model-backed table, whose engine pages), `filters` and `highlightKey` (a model-backed table's only; `filters` written as an object of `columns` and `groups` alone is the literal filter, read as a `table::FilterSpec` and refused when it does not read, "`filters` is not a filter: <reason>", the default a stored filter replaces; any other value is an expression that binds the filters), `rowTone` ⓘ, `rowSpans` ⓘ (each row's spans, read in the row scope: a list of `{column, rows}` records, `column` the id of a column and `rows` an integer from 1 counting the row itself, whose cell in that column covers the rows below; `null` or `[]` none; the cells it covers are not shown and their content is ignored, though a client that does not know the key draws them, so a document leaves them empty; a table that is sorted, or above its virtualization threshold, draws no spans, and a span that names no column, repeats a column of its row, runs past the last row of the page the table holds, runs over a group heading, or starts in a column an earlier span covers, is refused and reported, the cell then standing in its own row (the interpreter's reports carry a path; the mount's for a span past the last row, onto a covered column, or above the virtualization threshold are bare and name none, so look at the rows' order); a spanned table should not page, or should page by groups, since a page boundary through a span refuses it; [`view_tree.md`](view_tree.md#row-spans)), `onActivate` ⓘ, `cellMenu` (parts in the `menu` node's entry shape, `{label, icon, keys, checked, enabled, onClick, items, description, visible}` or `{separator: true}`, checked and reported as a `menu`'s entries are. A right click, a long press, or `Shift+F10` or the context-menu key on the focused cell opens the cell's menu: it focuses the cell and selects its row alone when the row is not selected, a selected row keeping the selection. It shows "Copy", "Copy with headers", "Filter by this value" (the column's filter becomes `{include: [{oneOf: [<the cell's operand>]}]}`, replacing the column's entries), "Exclude this value" (the operand joins the column's `exclude` `oneOf` entry, made when absent) and, on a column of a declared group, "Filter by this value in any <group label>" (the group's filter becomes `{group: <id>, include: [{oneOf: [<operand>]}]}`, replacing that group's); on an empty cell they use `{isEmpty: true}`, and on a cell not loaded or not readable, in a column that does not filter, or while `headerEnabled` is false, they are disabled; one chosen anyway is refused as a header gesture is. On a table over `rows` a cell is read as the column's filter reads it, by the kind its cells infer. Then a separator and the parts, read in the row's scope with `row` (the row's record, as `{"ref": "row.id"}`), `column` (the column's id) and `value` (the cell, as the record holds it) bound, each as it was when the menu opened; the menu has a row scope of its own, so state the table's row scope declares reads its initial value there. An entry's `keys` work while the table has the focus, on the focused cell: only a key that is some entry's `keys` asks for the focused cell's entries, and asking moves no focus. What the parts read of the row is fetched with every row, as `rowTone`'s is), `headerMenu` (parts in the same shape: a header's menu shows its own entries, a separator, then these, read with `column` bound to the header's column id, so `"visible": {"fn": "eq", "args": [{"ref": "column"}, "mass"]}` shows an entry on one header; a header whose parts are all hidden adds nothing, separator included; a table with `configurable: false` has no header menu, so none shows), `headerEnabled` (a condition, default `true`, read at each gesture: while it is false a header click, a filter button, a column's move or resize and a group's filter button change nothing and send no query, each refused in the status line, "Sorting, filtering and arranging are unavailable right now"; every entry of a header's own menu, every entry of the table's menu but "Copy" and "Copy with headers", and the cell menu's filter entries are disabled, and a menu entry opened before and chosen after the condition turned false is refused the same way. Selecting, scrolling, copying and the `cellMenu` and `headerMenu` entries stay, so a screen that runs a long operation over the rows sets it to keep them from reordering or leaving the view), `empty` (nodes: shown instead of the table while `rows` is an empty list; on a model-backed table, below it once a reply said the view has exactly no row and no row is undecided), `scope` | `selection`, `activeKey`, `sort`, `filters` (the filters the rows hold, `{"columns": {id: filter}, "groups": [group filter…]}`, each group filter naming its group or its columns, never an empty one of the two), `viewCount` (the rows the table orders, every page; on a model-backed table the reply's total, `null` before the first), `viewCountKind` (a model-backed table's: `exact`, or `atLeast` when the total is a lower bound), `undecided`, `awaiting` and `failed` (a model-backed table's: rows its filter cannot decide yet, and the levels its order waits for or failed to load), `sourceCount` (the rows `rows` gave), `selectedCount` (on a model-backed table holding a selection of the view, the view's total less the keys of `except` the window holds), `selectedCountKind` (`exact`, or `approximate` while the total is a lower bound or a key of `except` is not accounted for; the status line then says "about n of m selected"), `selectionSet` (every table's selection as an action's body takes it: `{"keys": […]}`, or on a model-backed table after select-all `{"view": {"filters", "retained", "revision"}, "except": […]}`, the view's filter with its default members left out; `selection` is then `[]`), `busyCount` (a model-backed table's: how many rows an action of this session is working on, those its input names as a `RowSelection` or a `RowRef` of the table's record, until it settles whatever its outcome; with a selection of the view in flight, the view's selected count at the run's start; such a row draws busy and takes no cell edit, and the status line stands with "Saving 40 rows"; a row the action's `ChangedRows::failed` names draws `err` with its message as its tooltip until the next action on it, a change event naming it after the action settled, or "Clear marks", and the status line stands with "12 rows could not be saved" and "Select them", which selects them; a row whose `rowLock` companion holds text draws locked with that text as its tooltip and takes no cell edit; a `ChangedRow` with `from` moves the selection, the focus, the retained rows and the busy and failed states the session holds by `from` to the new key, and while such an action is in flight a selected key that leaves the view is kept until it settles), `modifiedCount`, `modifiedRows` and `errorCount` (a grid of a form's: its cells whose value differs from the one the form was loaded with, its rows with such a cell or added since, and its cells whose draft does not encode or that the last submission refused; 0 on any other table), `editing` (a grid of a form's open cell editor, `{"key", "column"}`, or `null`), a grid's `selection` (its cell selection, `{"anchor": {"key", "column"}, "focus": {"key", "column"}}` or `null`), `pending`, `error`, `hoveredKey`, `layout` (the layout the table holds or the document binds, fitted to the table's columns; `groups` and `units` only while they hold anything), `visibleColumns` (the ids of the visible columns, in order), `hiddenCount` (how many columns are hidden), `page` (the page shown, clamped), `pageCount` (at least 1), `pageFirst` and `pageLast` (the 1-based positions of the page's first and last rows, both 0 when there are none; a table that does not page reads page 0 of 1 over every row) |
| `dateTimeInput` | `value`, `mode`, `label`, `showLabel`, `onChange`, `onCommit` | as `textInput` |
| `slider` | `value`, `min`, `max`, `step`, `label`, `onChange`, `onCommit` | as `textInput` |
| `filePicker` | `accept`, `multiple`, `mode`, `label`, `onPick` | |
| `banner` | `tone`, `text`, `spans` (as on `text`; a run may also carry `action`, below), `dismissible`, `action` (`{label, onClick}`), `actionStyle` (a literal name: `ghost`, the default, an unframed button at the trailing edge; `button`, a framed normal button there; `link`, the action drawn inside the message, underlined in the accent, wrapping with the sentence; to assistive technology a link stays a button named by the action's label, and runs the action rather than navigating), `actionStretch` (a literal boolean, default `false`: the action takes a line of its own under the message, as wide as the banner; the loader refuses it beside `actionStyle: "link"` and without an `action`), `onDismiss`. In a `link` banner's `spans` at most one run may carry `"action": true`: its text is the link, at that place in the sentence; without such a run the link follows the message, its text the action's label. The loader refuses an `action` run in any other kind's spans, in a banner whose style is not `link`, and a second one in the same spans | |
| `badge` | `tone`, `text`, `icon`, `style` (`filled`, the default; `outline`: the surface's fill, with the tone's outline colour as border and its colour as text; or `quiet`: the surface's fill with the control border and the muted text colour, whatever the tone), `shape` (`default` or `round`) and `mono` (a literal flag), as on `button`, `onRemove` (commands: the badge draws a remove control after its text and takes the focus, and removing it runs them; a removable badge is a filter chip), `removeLabel` (the control's accessible name, only with `onRemove`; default `Remove ` and the text, translated with `t`) | |
| `progress` | `value` (0 to 1; a decimal is read exactly, at whatever places it carries), `label` | |
| `steps` | `items` (an expression evaluating to a list of records `{label, state, detail}`; `detail` is an optional field of each record, as `label` is, shown as text the same way: a number in the locale's format, a timestamp as ISO 8601, `null` or absent as nothing, so a screen formats a date it wants shown otherwise itself, with the expression language's `date` or `dateTime`; a step with no detail draws none; a client that does not know `detail` shows the step without it), `current`, `labelled` (a condition, default true: false draws only the marks and the connectors; labelled or not, each step keeps its label, its detail and its state as its accessible name, joined by `, `) | |
| `keyValue` | `items` (a list of `{label, value, spans, tone}` records: `spans` as on `text`, shown in place of `value` by a client that knows them; `tone` (`ok`, `warn`, `err`, `info`; `neutral` or `null` for none) draws the value in the tone's colour and strong weight, and a name that is none of them is reported once and drawn as none), `fieldLayout` (`inline`, the default: each caption beside its value; `stacked`: each caption above it, as a `form`'s), `captionWidth` (the width of the caption column in units, a literal integer from 1 to 64, refused with `stacked`: absent, the column is as wide as the widest caption; a longer caption wraps on Qt Quick and is cut with an ellipsis in the terminal) | |
| `emptyState` | `title`, `text`, `icon`, `action` | |
| `chart` | `summary` (required: the text alternative), `series` (required, 1 to 4 parts `{points, y ⓘ, x ⓘ, xLabel ⓘ, tone ⓘ, note ⓘ, label, joined}`), `lines` (0 to 16 parts `{value, label, tone, style}`), `yMin`, `yMax`, `yPlaces` (a literal integer, 0 to 18), `yLabel`; see [Charts](#charts) | |
| `splitter` | `orientation`, `sizes` (a list of integers in units, one per pane; a list of another length places each pane of a horizontal splitter by its own `sizing` and a vertical splitter's equally), `children` (a pane's `minWidth`/`maxWidth` bound it; a pane is usually `percent(n)` with a `maxWidth`; a literal `sizes` that matches the panes places them, and a pane sized `fixed` or `percent` is then reported once as not used) | |
| `collapsible` | `title`, `gap` (as a panel's, default 2), `open`, `header`, `children`, `onToggle` | |
| `dropZone` | `accept`, `multiple`, `children`, `onDrop` | |
| `actionBar` | `children`, `align`, `justify` (as a `row`'s). An action bar holds the actions of what holds it: the footer of the dialog or drawer it is inside, at any depth, else the place it is written, as a row. The buttons are the theme's `actions.gap` apart (spacing step 2 while the theme sets none), so it takes no `gap`, which the loader refuses; `wrap` (a literal flag, as a `row`'s) lets actions that do not fit continue on the next line, and is refused at mount in an overlay's footer. A horizontal `forEach` that is a direct child of the bar is part of its row of actions and takes the theme's gap too, so a `gap` on it is refused at `/…/gap`: at load when its `axis` is the literal `"horizontal"`, at lowering when a computed `axis` evaluates to horizontal. A vertical `forEach`, or one inside a `row` in the bar, is unchanged | |
| `customize` | `point` (required) | |
| `boundary` | `loading`, `error`, `children` | |
| `form` | `model`, `action` (required), `prefill`, `key` (the identity of the record `prefill` holds), `submit` (`explicit` or `auto`; absent keeps the schema's mode), `showErrors`, `fields` (field name to overlay of expressions: `omit`, `hidden`, `label`, `unit`, `decimals`, `widget`, `blankAs`; an overlay may also name a `component` that draws the field, with `props`, a record of expressions. An overlay whose `widget` is the literal `grid` may give a `grid` part, the keys a `table` over a `form` takes beside its `form` and `field` (`columns`, `enterMoves`, `walk`, `onActivate`, `layout`, `onLayoutChange`, `configurable`, `onFiltersChange`, `cellMenu`, `headerMenu`, `headerEnabled`, `testId`), read as such a table's are: the grid that draws the field is that table; a `grid` part beside any other `widget` is refused, "a field's `grid` gives a grid widget its columns; give it `widget: \"grid\"`". An overlay's `decimals` overrides the schema's `x-decimalPlaces` for that field and means exactly what the key means in `InstanceConstraints`: the canonical unit's entry places (the most fraction digits it accepts) and the wire `dp`. Another unit of a Quantity keeps its own places, so a unit switch never rewrites or rejects a draft by places that belong to another unit; a literal overlay and a served key mean the same. On a Decimal, a Quantity and a plain Number it bounds what is accepted and is the places a value is shown at (on a Number, in place of `x-displayDecimals`); on an Integer it does nothing. A prefill is shown at the places the expression gives when it loads (the canonical unit's, as a served key would show it; the schema's when it gives none), so a Decimal or Quantity record with fewer places than the schema loads without trailing zeros, a Number record with fewer places is padded with zeros to them, and one with more places is not rounded. It follows its expression: a change re-checks the draft the field holds, a prefilled one included, which stays as typed and shows the field's `too-precise` error when it no longer fits. An expression that gives no places (null, negative, not an integer) restores the schema's places, whatever it gave before. The form builds from the catalog's static schema for the action, so the overlay is how a screen gives a field its record's places; the schema a form builds from is not decorated per instance, which is also why an instance's `minimum` and `maximum` have no client half here), `onSubmit`, `onSuccess`, `onError`, `title`, `submitLabel`, `showStatus` (a literal `bool`; default `true`), `columns` (a literal integer, 1 to 12), `fieldLayout` (`stacked`, the default, or `inline`: each label beside its control), `captionWidth` (the width of the inline label column in units, a literal integer from 1 to 64, refused without `fieldLayout: inline`; absent keeps the engine's 16; it is the document spelling of `inlineLabelWidth`, and means what it means on `keyValue`), `derive` (an action of the form's model, checked against the catalog as the form's `action` is, that the form calls with `{"body": <the body as far as it encodes>}` after a grid's commit to load its derived members, [`../table/editing.md`](../table/editing.md#derived-values)), `deriveDebounce` (a literal integer, milliseconds of no commit before a round of derived values, default 150: a round evaluates the action, for its computed members, and calls `derive` when the form names one, so the debounce times both and is taken without `derive`; refused when it is not a literal of 0 or more, "`deriveDebounce` is a literal count of milliseconds, 0 or more") | `ready`, `body`, `submitted`, `pending`, and each field as a record of `value`, `draft`, `valid`, `required`, `visible`, `readonly` and `errors` (`form.field.value`) |
| `collection`, `wizard` | `model`, `action` (required), and `submitLabel`, `showStatus`, `columns`, `fieldLayout` and `captionWidth` as on `form`, reaching every form the view draws; `title` replaces its heading | |
| `custom` | `name` (required), `props` (record of expressions), `on` (record of command lists) | |

`gap` and `padding` are spacing steps, 0 to 5 ([`view_tree.md`](view_tree.md)); the mount clamps any other value.

A `dateTimeInput`'s `value` is a `timestamp` or an ISO-8601 date or date-time string (`yyyy-MM-dd` or
`yyyy-MM-ddTHH:MM:SS[.fff][Z]`); the loader refuses any other type at `value`. The field keeps the type the
document declares and writes it back: a string field writes `yyyy-MM-dd` in `date` mode and the ISO-8601 instant
in `date-time` mode, and `""` on clearing; a timestamp field writes the timestamp, or `null` on clearing. A value
whose type the document leaves undecided is written as the type it holds at the edit. The input reads its value as
the expression language's date functions do ([`expressions.md`](expressions.md#semantics), "Dates and instants"):
a `yyyy-MM-dd` string is a date, which it shows as midnight in the display zone, and `""` is a date not given yet.

**Spans.** A `text`'s or a `banner`'s `spans` is an expression of a list of runs, the one way to emphasise part of
a sentence: an identifier in the monospace face, a bold word, a figure in a tone. Each run is a record
`{text, strong, mono, tone}`, whose `text` is shown as any value is (a number as its digits), whose `strong` and
`mono` are booleans that default to `false` and whose `tone` (`ok`, `warn`, `err`, `info`; `neutral` or `null` for
none) colours the run, or a plain value, a run with no flag and no tone. The node's role gives every run its size,
and its colour unless the run has a tone: a muted line with a `mono` run is muted in the mono face. Data is only ever the text of a run and is never parsed, so it cannot
open a run of its own. A `spans` written as a `list` is checked at load: a literal run is a string, and an `obj` run
has a `text`, no keys but `text`, `strong`, `mono` and `tone`, a literal flag that is a boolean and a literal tone
that is one of the names. A computed run of another shape (a list, `null`, a record without `text`) is dropped and
reported once at the node's `spans`, and a computed tone of no known name is reported once there and the run is
drawn untoned. A node may give
`text`, `spans` or both: a client that knows spans draws them and does not read `text`, and `text` beside `spans` is
what a client older than spans draws, so a document that may reach one gives both.

```json
{"kind": "text", "role": "muted",
 "spans": {"list": ["Signing as ", {"obj": {"text": {"ref": "meaningLabel"}, "strong": true}}, "."]}}
```

**Groups.** A `group` is a labelled set of inputs, HTML's `fieldset` and `legend`: its legend is drawn with the
required mark when `required`, its description and its shown errors under its children. Which of its children are
required is the application's rule, which a check targeting the group says: the check's message shows on the group
and keeps a dialog from `ready`. A group counts as touched once any element inside it is, so under the `touched`
policy its message shows as soon as the user has acted inside it.

A node list is an array of nodes or one node. `nodeKinds()` lists the kinds.

### Element versions

An element (a node or a part with an `id`) may carry `since`, the version it first ships in: a dotted numeric version
such as `"6.11.0"`, ordered as a field's `FieldMeta::since` is ([`../forms/forms.md`](../forms/forms.md#field-versions--since)),
and kept as `DocNode::since`. An element without one belongs to the first version. The loader refuses, at the
`since`'s pointer, a `since` that is not a string, one that is not a version (with the version's own reason), and one
on an object with no `id`. A form's `fields` record is a map of field names, so a field named `since` is a field there.

**Element ids are permanent**, as wire field names are: an element is removed and a new one added with a new id, never
renamed, and a removed id is not used again. A person's copy of a screen finds the base's elements by their ids, so a
renamed element would read as one removed and another added. How a copy is compared with its base is in
[`custom_screens.md`](custom_screens.md#versions).

### Charts

A `chart` plots points against a numeric axis, joined or as bars, with horizontal reference lines: a control
chart, a trend, a stability plot. It is a palette node rather than a host component, so it reaches every client and
backend, and what it plots, marks and says is the interpreter's ([`interpreter.md`](interpreter.md#charts)).

- A **series** draws one point per element of `points`. `y` (required), `x`, `xLabel`, `tone` and `note` are read
  once per point, with `row` the point's element, as a table's `key` is; no row scope is made, so they read no name
  a row scope would declare. `y` is a number or a quantity, and `null` leaves a gap; `x` is a number, a date or an
  instant, and without `x` on any series a point sits at its index; `xLabel` labels the point on the horizontal
  axis; `tone` (`neutral`, `info`, `ok`, `warn`, `err`, or `null`) marks it; `note` is text beside it. `label` is
  the series' name in the legend, required when there are two or more series. `joined` (a literal `bool`, default
  `true`) joins the points. `style` (a literal name, `line` by default, or `bars`) draws the series as a line of
  points or as bars (`view_tree.md`, Bars): a bar stands at each point's position from the baseline to its `y`, and a
  series with `style: bars` has no joining line, so `joined: true` beside it is refused (`joined: false` is accepted). A client that predates `style`
  ignores it and draws the series as a joined line; a document that must not be drawn so writes `!style`, which such a client refuses.
- A **line** is drawn at `value` (required, a number or a quantity), in its `tone` (default `neutral`), with
  `style` `solid` (the default), `dashed` or `dotted`; `label` defaults to the value at `yPlaces`.
- `yMin` and `yMax` fix the ends of the value axis; `yPlaces` is the places its labels are shown at; `yLabel` names
  its unit.

The loader refuses, each at its path: a chart without `summary` or `series`; a series without `points` or `y`; two
or more series where one has no `label`; `x` on some series and not on others; more than 4 series or 16 lines; a
`yPlaces` that is not a literal integer from 0 to 18; a line `style` other than the three; a series `style` other than `line` and `bars`; `joined: true` beside `style: bars`; a literal `tone` that names no
tone; literal `yMin` above literal `yMax`. A computed value that breaks one of these is reported when it is
evaluated.

## Scopes and names

- **Scopes.** The screen is a scope; so is every `dialog` and `drawer`, every `switch` case, and
  every row of a `forEach` or `table`. A scope-creating node writes its declarations under
  `"scope"`. A component's fallback is a scope of its own with no parent: it sees its `props` and
  nothing of the screen.
- **What a scope declares.** Its params (the screen), model aliases, state, `let`, queries,
  mutations, the ids of the elements inside it that belong to no nested scope, and the transfer ids
  of the `upload` commands in it. All share one namespace per scope; a name declared twice is
  refused, and `event`, `app`, `boundary`, `props`, `result` and `body` are reserved.
- **Resolution.** A name resolves in the innermost scope first, then outward. Bound names come
  before declarations: a comprehension's variable, a `seq` step's reply, an `each` element, `body`
  in an `optimistic`, `props` in a fallback. `event` is valid inside a command
  list, `boundary.error` and `retry` inside a boundary's `error`, `row` inside a row scope and any
  scope nested in one, `cancelClose` inside `onClose`. `app.name` reads the app document and is not
  checked against it.
- **What a reference reads.** A param or state reads its type; a `let` the type of its expression;
  a query its value (`any`); an element one of its values, which the reference must name; a transfer
  `progress`, `pending` or `error`. A mutation is read through `result`, `pending`, `error` and `refusal`, a
  model alias not at all.
- **Refusals in words.** `{"refusal": "m"}` reads a mutation's or a query's last failure when it was an action refusal
  (`null`, or `{code, args}`); `refusalText(refusal)` is its sentence from the catalogue in the user's locale; `t`
  takes `{"expr": e}` as its key, and as its `args` an expression evaluating to a list of `{name, value}` records.
  A mutation or query that is not in scope refuses `refusal` at load as it does `error`. See
  [`refusals.md`](refusals.md).

## Load-time rules

`loadDocument` returns the first refusal; each rule has a case in
`tests/ui/corpus/documents.json`.

- **Versions.** An element's `since` and a document's `knownVersion` are versions ([Element versions](#element-versions)); an
  element's `since` sits next to its `id`.
- **Vocabulary.** `vocab` is `ui/1`; node kinds, commands, expression forms and functions are those of `ui/1`;
  no unknown `!` key. A mutation declares no `invalidates` or `patch` and a query no `refreshOn`; the refusal
  points at the key and says that effects replace it.
- **Shape.** Each key has the shape its kind gives it; required keys are present; names match
  `[A-Za-z_][A-Za-z0-9_]*`; closed choices (`showErrors`, `concurrency`, `source`, `offline`,
  `commitOn`, `onStale`, `navigate`'s `as`, `notify`'s `role`, `align`, `justify`, `alignSelf`, a button's
  `emphasis` and `style`) take one of their values; chords are
  modifiers (`Ctrl`, `Shift`, `Alt`, `Meta`, once each) and a key name, and are not the workspace
  shell's (`Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+W`, `Ctrl+K`). A flag (`showLabel`) is a literal
  `true` or `false`, and `showLabel` needs a `label` to show. A size bound is a literal integer of 0 or more, a
  minimum is no more than its maximum, and no bound is given on a `dialog` or `drawer` (an overlay is sized by the
  frontend, not by its container; a sheet is the window's size). On Qt Quick, that sizing caps the overlay at the window less a margin on each
  axis, and scrolls whatever content does not fit between the title and the footer, which both stay reachable.
  There is also no bound on a `custom` node (which carries no common properties). A `percent` sizes a width along a row that does not wrap
  or a horizontal splitter's pane only: the loader refuses a literal one as a height, on a node whose parent is a column, a grid, a wrapping
  row, a panel, a scroll, a tab page or a vertical splitter, on the root, on a `dialog` or `drawer`, as a table column's or a grid track's `width`,
  and with an amount outside 1 to 100 or not a whole number. A computed one that yields such an amount or a height is reported once and lowers
  to content. A lone node in a row's holder (a `forEach` body) takes its share for the holder and fills it; a percent inside a `switch` case or an
  overlay's content is reported ("size the `switch` or the overlay itself") and fills, and a percent among several nodes of one holder is reported
  and takes its content size. A literal `sizing` is
  one of its forms, and a literal fixed amount lies inside its dimension's bounds (a fixed size has no room to
  bound); a `sizing` that is not literal (a `record`, `cond` or `obj` expression) is read at mount, and one that
  evaluates to none of the forms is reported once and lowers to content. A literal text `role` is one of its names; a computed one is read at mount, and one that evaluates to no role is
  reported once for its path and drawn `normal`. A `menu`'s `label`, `icon` and `emphasis` need `style: button`, and a
  button-style menu has a `label` or an `a11y` name (its trigger's accessible name); a menu entry's `separator` is
  `true` and takes no other key. A `grid` takes no `justify` (its columns
  are sized by their count or tracks; there is no spare length to justify), and `columns` or `tracks`, not both, with
  at least one track; a track's `align` is `start`, `center` or `end`; `alignSelf` is given only on a child of a `row`, `column`,
  `grid` or `forEach`, not on the view root, a component's fallback root or the content of another kind (a `panel`,
  a `dialog`); and the `column` or `row` an `arrange` point orders takes no `align` or `justify`, since it is drawn as
  a list. An `actionButton` has an `id` and a `label`, and its
  id is used once in its list (a screen's `actions`, a dialog's or drawer's); `subtitle`, `status` and `actions` are
  refused on the app document. A tab's `id` is used once among its node's tabs; every tab has one when the node
  gives `selected` or `onChoose`; and a literal `selected` names one of them.
- **Expressions.** Each parses and has at most 256 nodes; each name resolves as above; a function's
  statically known argument kinds are ones it takes; `visible`, `enabled`, `readonly`, `required`,
  `stale`, `open`, `when`, `dirty`, a check's `assert` and every `if` condition are `bool` where
  their type is known; a literal `t` fallback is in the message subset.
- **Types.** A state's `init` has its declared type; a `set` of typed state has a value of that type.
- **Cycles.** The `let` graph, the state inits and the watch graph are acyclic. A watch's edges run
  from what it writes (`set`, `reset`) to the watches that read it in `on`, directly or through
  `let`; a watch that writes what it reads is a cycle of one. The refusal names the cycle in order.
- **Targets.** `set` names state, an input (`textInput`, `checkbox`, `select`, `dateTimeInput`,
  `slider`) or `app.name`, never a table: a table's selection changes through state its `selection` binds, and the
  refusal says so; `reset` state or a dialog; `run`, `signIn` and `upload` a mutation;
  `refetch` a query; `open` and `close` a dialog or drawer (`close` also `self`: the innermost dialog or drawer whose
  content holds the command, or the screen when there is none; inside a component, only a dialog or drawer of the
  component's own); `focus` an element of the command's scope chain, or else one declared in the `switch` cases
  reachable below that chain (down through cases only, never into a row, dialog, drawer or component): the
  declarations there must all be cases of one switch, since at most one case is mounted, and two switches declaring
  the name are refused with both paths. `focus` with `row` (an expression of a row key) names an element of a
  `forEach`'s or table's row scope: the one row scope declaring it among the command's own and those below its scope
  chain through switch cases; none, or two, is refused. Only `focus` reaches into cases: reading an element's value
  from outside its case stays refused;
  `resetPrefs` a point or `all`; a download to preview a state. `from` and `into` are `ref` or `bytes`; a download
  from bytes gives a `name`; `type` is given only with `"from": "bytes"`, and `nameAs` and `typeAs` only with
  `"into": "bytes"`. A check's targets are elements;
  `optimistic` names a query of the document.
- **Models.** Queries, mutations and model-backed nodes name a model alias in scope. An alias in a
  row scope names its `instance`; only a screen's own aliases borrow (`{"from": alias}`).
- **Components and points.** A custom node names a declared component and only its props and
  events; a component names exactly one of `qml` (`sha256:` and 64 lower-case hex digits) and `host`
  with an integer `version`, prop types from `string`, `bool`, `int`, `exact`, `number`, `json`, and
  a fallback. A `customize` node names a declared point; a `columns` point names a table, and a `saved` point that gives `of` names one; a `choice`
  lists its options and defaults to one of them; a `value` point declares its type and its default
  has it; an `arrange` point's `of` names a `column` or `row` of the same screen whose children
  all have ids, no other `arrange` point names the same container, and the app document, which has no view,
  may not declare one; `pins` belong to the app document.
- **Prefetch.** Each name is a screen query whose body reads nothing but the screen's params.
- **Field components.** A form field overlay's `component` is a name of a declared component that declares a prop
  `value` and an event `change`; its `props` name only props the component declares, and an overlay with a
  `component` gives no `widget`, since both choose the field's control.

## The catalog

`ui::Catalog` holds two lookups the caller supplies: `model(typeId)` → `ModelInfo{stateless,
keyed}` and `action(actionId)` → `ActionInfo{modelType, calculatedFields, secretFields,
credential, listFields, effects, records}`. In local mode the caller answers them in-process from the model registry; on the
server, registration answers them from the same registry; a remote client answers them from the UI
service's `ui-catalog` reply. With them, a document is also refused when a model type or action is
unknown, an `instance` expression names a model that is not keyed, an action belongs to another
model type, an `optimistic` `with` writes a field
the server calculates, or a mutation that sends a credential or returns a secret is queued offline.
`listFields`, when present, says the query's action answers a record and names the record's members
that hold a list; an `optimistic` write to such a query is then refused without an `at` (it could
never find a list) or with an `at` that is not one of them. When it is absent, the catalog does not
know the result's shape, and a write that finds no list is reported by the interpreter when it runs.
`effects` and `records` are what the core registries hold for the action: `effects` is the list of
`{kind, record?}` the action declared, `kind` one of `updates`, `changes`, `changesInstance` and `none`, `record` the
record's wire name for the first two; it is absent when the action declared nothing, which makes the action a read.
A record derived (`derivedFrom`) from one the action `updates` or `changes`, directly or through others, is listed as
`changes` too, once. The derivation itself is never sent: a client knows only that the derived record changes, which
lets the caller's own reply mark entries holding it, in process as well as remotely.
`records` lists the records the action's result holds as `{record, keyField?, paths}`, empty when it holds none. They
are read from the registries where `CatalogFacts::action` records the action, so an invalid effects and records
configuration (`EffectsRegistrationError`) fails at that call, at startup. They are what a client's watch is derived
from, and the loader reads them for one refusal and three warnings:

- **A query must read.** A query whose action declares effects, `none` included, is refused: a fetch then never
  produces a change, so two clients cannot refetch each other through their queries. An `evaluate` query is exempt,
  because an evaluation runs no handler.
- **The warnings** are in [Warnings](#warnings).

An action the catalog does not answer is refused before either is asked, so effects registered for an action id that
no action registers are never seen by a document: the catalog describes registered actions only.
A lookup left empty skips its rules; the document is otherwise checked in full. `stateless` decides
an alias's default instance, which is the interpreter's. A third lookup, `schema(actionId)`, answers
an action's schema JSON; it is not a load-time rule but what the interpreter builds a `form` node's
form from. Two more do the same for the forms engine's screens: `view(actionId)` answers the `v-*`
view document whose `v-query` is the action (a `collection` node's), and `wizard(actionId)` the
`w-*` wizard document whose first step is the action (a `wizard` node's).

## Refusals

`ui::Refusal` is the document id, a JSON Pointer (RFC 6901) into the document, and the reason;
`message()` prints `document <id> at <path>: <reason>`. A refusal from inside an expression points
at the expression node. Text that is not JSON (`loadDocumentText`) is refused at the root, and so is text nested deeper than 64
objects and arrays (`kMaxJsonDepth`), which is checked before the JSON parser runs.

## Warnings

`Document::warnings` holds the `LoadWarning{kind, path, text}` findings that do not refuse the document, in the order
found; `message(documentId)` prints them as a refusal is printed. They need the catalog's `action` lookup, and a
document loaded without it has none.

| Kind | When |
|---|---|
| `Uncovered` | A mutation whose action declares nothing, not even `none`, and that has no `refetch` anywhere in its `onSuccess`. After it succeeds nothing on any screen updates. `effects::none` silences it for an action that changes nothing, such as one that opens or attaches. |
| `WatchFeedback` | A `watch` declaration whose `source` is `any` (the default), whose `on` reads a query's value (`{"query": q}` or a `ref` to it, directly or through a `let`), and whose `do` runs (`run`, `upload`, `signIn`, at any depth) a mutation whose action declares an effect other than `none`. Another client's change refetches the query, the watch runs the mutation, and its change reaches the other client. `"source": "user"` silences it. |

`UiRegistry::addScreen`, `UiRegistry::addFile` for the app document, and `author::ScreenRegistry::add` log each warning at `warn`
(`morph::log`) when they register a document; the document registers as it would without them.

## JSON Patch and structural diff

`json_patch.hpp` holds two generic helpers over a JSON value (`Json`); neither knows what a screen is.

- **`applyJsonPatch(document, patch)`** applies an RFC 6902 patch, an array of `add`, `remove`, `replace`, `move`, `copy`
  and `test` operations, to a copy of `document`. The path `""` is the whole document, and a path that does not exist is
  refused rather than made. The operations are checked for their shape first (`op N is not a JSON object`, `op N: `op`
  must be one of ...`, `op N: `path` must be a string ...`, `op N: `add` needs a `value``, `op N: `move` needs a
  `from``) and then applied one at a time, so a refusal names the operation that failed, counted from 0, with its name
  and path (`op 1 (remove /missing): the path does not exist in the document`). Either every operation applies and the
  patched document is returned, or none does. `propose_screen` applies the assistant's patch through it.
- **`diffJson(before, after)`** answers the patch that turns `before` into `after`; it cannot fail. Objects are compared
  member by member and arrays element by element, so a change is one operation at its own pointer: a member only in
  `before` is a `remove`, one only in `after` an `add`, and a value that differs and is not a container on both sides a
  `replace`. An array that grew gets an `add` for each element past the old end, in order; one that shrank gets a
  `remove` for each element past the new end, last first, so each index is the one the operation finds. A moved
  element or member is a removal and an addition: no `move` or `copy` is produced. Tokens escape `~` as `~0` and `/` as
  `~1`. Equal documents give `[]`, and `applyJsonPatch(before, diffJson(before, after))` equals `after`.

## Design decisions

- **One generic node, one kind table.** A typed struct per kind would double the description of
  the palette: once in the view tree, once here. Keeping properties keyed by name, with the shapes in
  one table, makes the document model a faithful image of the JSON and leaves lowering to the
  interpreter, which knows the view tree.
- **The loader throws inside, returns outside.** Every rule is a throw of an internal
  `DocumentRefused`; `loadDocument` turns the first into its `std::expected`. Threading an error
  return through every parser and checker step would double the branches without telling anyone
  more.
- **First refusal only.** A document is a server artefact checked in the server's tests; the first
  refusal, with its path, is what a developer fixes next.
- **Types are coarse.** The document does not restate action schemas, so most types it sees are
  `any`. The checks catch what a document can get wrong on its own: a string where a condition is
  expected, a field of an `int` param, an init of the wrong type.

## Out of scope

- Mounting a document, control, relations and commands at run time: the interpreter,
  [`interpreter.md`](interpreter.md).
- The accessibility rules of spec 5 §11: they guard authoring, run at registration with the
  server-side builders ([`author.md`](author.md)), and a client does not repeat them.
- Delivery: the `ui-*` envelope kinds, the catalog and bundles are the UI service's, [`service.md`](service.md).
