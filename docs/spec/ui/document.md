# The UI document — design

Design spec for the UI document's C++ model and its load-time validation
(`include/morph/ui/document.hpp`), and for the typed values every document expression evaluates to
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
| `subtitle`, `status`, `tools`, `actions`, `menu`, `menuLabel` | same | The screen's head beside its title, in the screen scope ([The head](#the-head)); a screen's only |
| `navigation` | `navigation` | The counts on the navigator's entries, in the app scope ([The app document's navigation](#the-app-documents-navigation)); the app document's only |
| `params` | `params` | Name to type, in declaration order |
| `scope` | `models`, `state`, `let`, `queries`, `mutations`, `watch`, `checks` | The screen scope's declarations |
| `lifecycle` | `onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange` | Command lists |
| `keys` | `keys` | Chord to command list |
| `showErrors` | `showErrors` | `touched`, `submit` or `always` |
| `prefetch` | `prefetch` | Screen queries started before the screen mounts |
| `components` | `components` | Name to `Component` |
| `customize` | `customize` | Name to `CustomizePoint` |
| `view` | `view` | The root `DocNode`; required on a screen |
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

A screen's head is what a shell shows above it beside the title: `subtitle` (an expression), `status` (parts
`headBadge`: `{tone, text, icon, visible}`), `tools` (parts `headTool`), `actions` (parts `actionButton`) and its own
`menu` (parts `headMenuItem`), with `menuLabel` (an expression, the menu trigger's accessible name; default `More
actions`). All are evaluated in the screen scope, and the app document, which has no view, refuses them. A shell
draws them in that order on one line: the subtitle, the badges, then at the trailing edge the tools, the actions and
the menu.

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

## Declarations

A scope's declarations are a `ScopeDecl`, each kind in declaration order.

| Declaration | JSON | Model |
|---|---|---|
| Model alias | `"alias": {"type": "T", "instance": "private" \| "shared" \| e \| {"from": alias}}` | `ModelDecl`, `InstanceMode` `Default`, `Private`, `Shared`, `Keyed` (a key expression) or `Borrowed`; `"shared"` on a model the catalog does not mark stateless is refused |
| State | `"name": {"type": "T"?, "init": e?}` | `StateDecl`; without a type, the type is the init's |
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
| `target` | The name acted on: `set`'s `name.path`; the state of `reset`; the mutation of `run`, `signIn`, `upload`; the query of `refetch`; the element of `open`, `close`, `focus`; the screen of `navigate`; the name of `host`, `switchBackend`, `setLocale`; the point of `resetPrefs`; the state a `download` to preview binds |
| `value` | `set`'s value; `run`'s body; `navigate`'s params; `return`'s value; `host`'s args; `notify`'s message; `if`'s condition; the file of `upload` and `download` |
| `extra` | `run`'s `each`; `navigate`'s `route`; `download`'s suggested `name`; `focus`'s `row` |
| `binding` | `run`'s and `upload`'s `as` |
| `option` | `navigate`'s `as`; `notify`'s `role`; `download`'s `dialog` or `preview` |
| `transfer` | `upload`'s `id`, whose `progress`, `pending` and `error` are element values |
| `bytes` | `download`'s `"from": "bytes"` and `upload`'s `"into": "bytes"` (default `ref`): the file moves inline, under the session's cap |
| `mediaType` | `download`'s `type`, the media type of inline bytes |
| `nameAs`, `typeAs` | `upload`'s body members for an inline file's name and media type |
| `commands`, `alternative` | `seq` and `all` steps; `if`'s branches; `navigate`'s `onResult` and `onCancel` |

`run`'s `as` names the element of `each` inside its body when `each` is given, and the reply for
the later steps of the enclosing `seq` otherwise. `if` takes two or three parts: a missing else
runs nothing.

## View nodes

**A bar that stays in view.** ui/1 has no screen footer: a `column` sized to stretch in height, holding a `scroll`
sized to stretch in height between its bars, keeps the bars at the column's ends while the content scrolls (the
scroll takes the column's spare height, not its content's). The same column gives a bar at the top, or both. As a
screen's root it fills the page; the author API writes it as `pinned`. A splitter pane may be pinned: a stretching
splitter gives each pane its height, and a pinned pane keeps its bars at the pane's ends.

A `DocNode` is a node or a structured part of one. Its properties stay keyed by their document
keys — `props` (expressions), `events` (command lists, a custom node's events as `on.<event>`),
`children` (node lists), `parts` (records such as columns and tabs), `names` (plain strings) and
`keys` — so the interpreter reads a kind's properties by name. One table in `document.hpp`
(`detail::kKinds`) says, per kind, which keys it has and what shape each takes; it is the only
place a kind is described.

**Every node** takes `id`, `visible`, `enabled` (conditions), `tooltip`, `sizing`, `span`,
`minWidth`, `maxWidth`, `minHeight`, `maxHeight` (literal integers of 0 or more, in backend units: the node's extent
on that axis is clamped into them, whatever its sizing), `alignSelf` (`start`, `center`, `end`, `stretch`: where the
node sits across its `row`'s, `column`'s, `grid` row's or `forEach`'s axis, in place of the container's `align`),
`autofocus`, `testId` (strings), `surface` (an expression of a surface's name, so it follows the data; a plain
string is a literal one: a value that names no surface draws as no surface does, `null` is no surface, and any other
value that is not a string is reported once at the node's `surface` path and drawn as no surface), `a11y`
(`{name, role}`), `drag` (`{payload}`), `drop` (`{accepts, onDrop}`) and `keys`. `sizing` is the width's —
`"content"`, `{"lit": {"fixed": n}}` or `{"lit": {"stretch": w}}` — or both dimensions as a record of them, one
left out being content: `"sizing": {"lit": {"width": {"stretch": 1}, "height": {"stretch": 1}}}`. A node's sizing means the
same inside a `switch` case, an overlay or a row as at the root: the content's stretch reaches the container it is
mounted in. **Text and the inputs** also take `readonly`, `required`, `stale`
(conditions) and `errors`. **The six inputs** also take `description`, a line drawn under the control and announced
as its accessible description, and `tone` (`ok`, `warn`, `err`, `info`; `neutral` or `null` for none), an evaluation
of the value that outlines the control and colours the description and, unlike `errors`, does not make the value
invalid; a tone of no known name is reported once and drawn as none. A `text` refuses both: a text is not described,
so the line goes in the text.

| Kind | Keys (a ⓘ key is in the node's own scope) | Element values beyond `visible`, `enabled`, `valid`, `errors`, `shownErrors` |
|---|---|---|
| `text` | `text`, `spans` (runs of emphasis, below), `role` (`normal`, the default, `muted`, `heading` (`title` is accepted as its alias), `section`, `error`, `success`, `mono`: an identifier, in the theme's fixed-pitch family at `font.size.mono`; `display`: a figure that stands out, such as a KPI's number, at `font.size.display` and the strong weight; `small`: a footnote or timestamp, at `font.size.small`) | |
| `button` | `label`, `icon`, `emphasis` (`normal`, `primary`, `danger`, `ghost`, `dangerOutline`: a destructive action that is not the main one, outlined with its label in the error colour), `style` (`push`, the default, or `item`: an entry of a list), `selected` (a condition: an item is the current entry; a push button is pressed in), `onClick`, `onActivate`, `children` (only with `style: item`: the entry's content, drawn in place of its label, which still names it; display kinds only at any depth — `text`, `badge`, `keyValue`, `progress`, `steps`, `busy`, `spacer`, `column`, `row`, `grid`, `forEach`, `switch` — and no `keys`, `drag`, `drop` or `autofocus` there: "an entry's content is read-only; put the control beside the entry") | |
| `textInput` | `value`, `placeholder`, `mode`, `icon` (an icon of the theme's icon set, drawn at the leading edge), `unit` (text, drawn muted at the trailing edge inside the field and announced after the field's description, such as `°C`), `emphasis` (`normal`, the default, or `prominent`: the screen's focal field, such as a scanner's, which the frontend draws so that it stands out) — the three refused beside a literal `mode: multiline`, and a computed mode that turns out multiline is reported once and its unit and icon are left out, `commitOn` (`change`, `blur`, `enter`), `label`, `showLabel` (a literal `bool`; default `false`), `onChange`, `onCommit`, `onSubmit` | `value`, `draft`, `dirty`, `focused`, `touched` |
| `checkbox` | `label`, `checked`, `onToggle`, `onChange` | `checked` |
| `select` | `options`, `selected`, `style` (`dropdown`, the default, `radio`, or `segmented`: every option one segment of a row, one marked; a constant is checked at load, and a computed one that names none is reported once and drawn as a dropdown), `label`, `showLabel`, `onStale` (`clear`, `keep`), `onChoose`, `onChange` | `selected`, `selectedOption` |
| `menu` | `items`: parts `{label, icon, keys, checked, enabled, onClick, items, description, visible}` (`description` a muted line of help with the entry; `visible` a condition, a hidden entry left out), or `{separator: true}` alone, a line between entries; `style` (`bar`, the default, or `button`: one button that opens the entries); for `style: button`, the trigger's `label`, `icon` and `emphasis` (as a button's) | |
| `group` | `label` (the legend), `required` (the legend's mark only: a group has no value, so no `Required` is derived from it), `description` (a line under the children), `errors` (as on an input), `gap`, `children` | |
| `column`, `row` | `children`, `gap`, `align` (across the axis: `start`, `center`, `end`, `stretch`, the default), `justify` (the spare length along the axis while no shown child stretches: `start`, the default, `center`, `end`, `between`, in equal gaps never less than `gap`); a `row` also takes `wrap` (a literal boolean, default `false`: the children continue on the next line when they do not fit, as the `StackWidget` row of the backend contract says; a `column` has no `wrap`, and the loader refuses it there); a `column` also takes `divided` (a literal boolean, default `false`: a 1 px rule in the theme's divider colour is drawn between each two shown children, centred in the gap and taking 1 px of its own, so a gap of 0 still separates them; the terminal draws none; the loader refuses it on a `row`) | |
| `grid` | `children`, `gap`, `columns` (columns sharing the width), `tracks` (in place of `columns`; parts `{width, align}`, one per column: `width` a sizing, default `{"stretch": 1}`, as a table column's; `align` `start`, the default, `center` or `end`, where a text's text and a label-width cell sit across the column, as a table column's `align`; a container cell fills it), `align` (each cell's content in its row, as a row's) | |
| `spacer` | | |
| `panel` | `title`, `header` (nodes, shown beside the title, usable while collapsed), `padding`, `gap` (the spacing step between several `children`, default 2, read once; a single child has none to space), `collapsible`, `collapsed`, `onToggle`, `children` | |
| `scroll` | `axis`, `children` | |
| `switch` | `on` (required), `cases`: parts `{when, scope, children ⓘ}`, `default` | |
| `tabs` | `tabs`: parts `{id, label, children}`, `selected`, `onChoose`; a tab's `label` follows its expression, and the list of tabs is fixed (a tab takes no `visible`). `selected` is the id of the tab shown, controlled as a select's: a pick runs `onChoose` with the tab's id as `event`, and the page follows `selected` alone; a literal `null` is no binding. Without it the node keeps the user's pick, so picking a tab mounts and shows its page. A relabel keeps the pick. A command chooses a tab by setting the state `selected` reads; `set` never names a tabs node | `selected` (the shown tab's id: the bound one, else the pick, else the first tab's) |
| `dialog`, `drawer` | `open`, `gap` (the spacing step between several `children`, default 3, read once), `title`, `description` (a line in the head under the title, ruled off from the body, which does not scroll with the content and is the overlay's accessible description), `showErrors`, `side`, `scope`, `children` ⓘ, `default` ⓘ, `cancel` ⓘ, `actions` ⓘ (parts `actionButton`, [The head](#the-head)), `onDismiss`; a dialog's `sheet` (a condition: while true the dialog is drawn as a sheet that fills the window, with a head of a dismiss control and the title, the document choosing by the width class, as `{"fn": "eq", "args": [{"env": "widthClass"}, "compact"]}`) and `dismissLabel` (the dismiss control's label, default `Close`, translated with `t`; only with `sheet`); the dismiss control dismisses exactly as Escape does, and a drawer takes neither | `open`, `ready`, `submitted` |
| `busy` | `active`, `label` | |
| `forEach` | `rows` (required), `key` ⓘ, `axis`, `gap`, `wrap` (a literal boolean, default `false`; the rows continue on the next line when they do not fit; the loader refuses `true` where a literal `axis` is not `horizontal`, and a computed `axis` that is not horizontal at lowering is reported and the rows do not wrap), `divided` (as a column's, for a vertical `forEach`; the loader refuses it where a literal `axis` is `horizontal`), `scope`, `children` ⓘ | |
| `table` | `rows`, or `model` and `action` with `body` and `mode`; `key` ⓘ, `columns` (parts `{id, label, width, role ⓘ, align, valign, sortable, sortBy ⓘ, cell ⓘ}`, or an expression of `{id, label, align, valign}` records — `role` styles the column's default cell and has no effect on one with its own `cell`; it takes a text's role names, and a literal one is checked the same way; `align` (`start`, the default, `center`, `end`) places a text cell's or a text input's text, a badge, check box or push button cell, and the header label across the column, while a container cell fills it; `valign` (`start`, `center`, the default, `end`) places every cell down its row; both are literal names, and a computed column's place of no known name is reported once and takes the default), `valign` (the default of every column that gives none), `selectionMode`, `selection` (a list of row keys: bound, it is controlled, as a select's `selected` is; refused without a `selectionMode` of `single` or `multiple`, and on a model-backed table), `onSelectionChange` (after each user change, with `event` the new keys; the same rules), `selectColumn` (a literal flag, only with a literal `selectionMode: multiple`: the table draws its selection as a leading column of check boxes under a select-all box), `selectAllLabel` (the select-all box's accessible name, only with `selectColumn`; default `Select all`, translated with `t`), `rowLabel` ⓘ (each row's accessible name, which its box carries; default the row's key as text), `sort` (a list of keys `{"column": <a sortable column's id>, "direction": "asc" or "desc"}`, most significant first, `[]` the source order: the table orders its rows by it; bound, it is controlled, as `selection` is, and unbound the table holds it from `[]`; a literal one names sortable columns, each once; a column's `sortable`, a literal flag that needs its `id`, makes a click on its header sort by it alone, ascending, or descending when it already is the first key ascending; its `sortBy`, only with `sortable`, is the value the rows order by, read per row as `key` is, default the row's field of the column's `id`), `onSortChange` (after a header click, with `event` the new sort; needs a sortable column), `pageSize` (a positive integer: the table shows one page of that many of its sorted rows; absent or `null`, every row; a literal below 1 is refused), `group` (a part `{by ⓘ, heading ⓘ}`: a heading row before each run of adjacent rows whose `by` values are equal, drawn from `heading` in a row scope whose `row` is the run's record `{key, count, first, last, items}` of the whole group; a heading spans every column, is never selected, marked or activated, and counts in no element value; with a sort, each group keeps its place and its rows are sorted within it, and a group split across pages gets its heading on both; refused on a model-backed table; a heading's key is reserved, `"\u001Fgroup:"` then the canonical JSON of its run's key, `":"` and its ordinal among the runs with that key, and a data row with a field `"\u001Fgroup"` is taken for a heading), `page` (the page shown, from 0, clamped to the last; absent or `null`, the first; needs `pageSize`; `pageSize` and `page` are refused on a model-backed table, whose engine pages), `filters` and `highlightKey` (a model-backed table's only), `rowTone` ⓘ, `onActivate` ⓘ, `empty` (nodes: shown instead of the table while `rows` is an empty list), `scope` | `selection`, `activeKey`, `sort`, `filters`, `viewCount` (the rows the table orders, every page), `sourceCount` (the rows `rows` gave), `selectedCount`, `pending`, `error`, `hoveredKey`, `page` (the page shown, clamped), `pageCount` (at least 1), `pageFirst` and `pageLast` (the 1-based positions of the page's first and last rows, both 0 when there are none; a table that does not page reads page 0 of 1 over every row) |
| `dateTimeInput` | `value`, `mode`, `label`, `showLabel`, `onChange`, `onCommit` | as `textInput` |
| `slider` | `value`, `min`, `max`, `step`, `label`, `onChange`, `onCommit` | as `textInput` |
| `filePicker` | `accept`, `multiple`, `mode`, `label`, `onPick` | |
| `banner` | `tone`, `text`, `spans` (as on `text`), `dismissible`, `action` (`{label, onClick}`), `onDismiss` | |
| `badge` | `tone`, `text`, `icon`, `style` (`filled`, the default, or `outline`: the surface's fill, with the tone's colour as border and text), `onRemove` (commands: the badge draws a remove control after its text and takes the focus, and removing it runs them; a removable badge is a filter chip), `removeLabel` (the control's accessible name, only with `onRemove`; default `Remove ` and the text, translated with `t`) | |
| `progress` | `value` (0 to 1; a decimal is read exactly, at whatever places it carries), `label` | |
| `steps` | `items`, `current` | |
| `keyValue` | `items` (a list of `{label, value, spans, tone}` records: `spans` as on `text`, shown in place of `value` by a client that knows them; `tone` (`ok`, `warn`, `err`, `info`; `neutral` or `null` for none) draws the value in the tone's colour and strong weight, and a name that is none of them is reported once and drawn as none), `fieldLayout` (`inline`, the default: each caption beside its value; `stacked`: each caption above it, as a `form`'s) | |
| `emptyState` | `title`, `text`, `icon`, `action` | |
| `chart` | `summary` (required: the text alternative), `series` (required, 1 to 4 parts `{points, y ⓘ, x ⓘ, xLabel ⓘ, tone ⓘ, note ⓘ, label, joined}`), `lines` (0 to 16 parts `{value, label, tone, style}`), `yMin`, `yMax`, `yPlaces` (a literal integer, 0 to 18), `yLabel`; see [Charts](#charts) | |
| `splitter` | `orientation`, `sizes` (a list of integers in units, one per pane; a list of another length shares the space equally), `children` | |
| `collapsible` | `title`, `gap` (as a panel's, default 2), `open`, `header`, `children`, `onToggle` | |
| `dropZone` | `accept`, `multiple`, `children`, `onDrop` | |
| `customize` | `point` (required) | |
| `boundary` | `loading`, `error`, `children` | |
| `form` | `model`, `action` (required), `prefill`, `key` (the identity of the record `prefill` holds), `submit` (`explicit` or `auto`; absent keeps the schema's mode), `showErrors`, `fields` (field name to overlay of expressions; an overlay may also name a `component` that draws the field, with `props`, a record of expressions), `onSubmit`, `onSuccess`, `onError`, `title`, `submitLabel`, `showStatus` (a literal `bool`; default `true`), `columns` (a literal integer, 1 to 12), `fieldLayout` (`stacked`, the default, or `inline`: each label beside its control) | `ready`, `body`, `submitted`, `pending`, and each field as a record of `value`, `draft`, `valid`, `required`, `visible`, `readonly` and `errors` (`form.field.value`) |
| `collection`, `wizard` | `model`, `action` (required), and `submitLabel`, `showStatus`, `columns` and `fieldLayout` as on `form`, reaching every form the view draws; `title` replaces its heading | |
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
a sentence: an identifier in the monospace face, a bold word. Each run is a record `{text, strong, mono}`, whose
`text` is shown as any value is (a number as its digits) and whose `strong` and `mono` are booleans that default to
`false`, or a plain value, a run with neither flag. The node's role gives every run its colour and size: a muted line
with a `mono` run is muted in the mono face. Data is only ever the text of a run and is never parsed, so it cannot
open a run of its own. A `spans` written as a `list` is checked at load: a literal run is a string, and an `obj` run
has a `text`, no keys but `text`, `strong` and `mono`, and a literal flag that is a boolean. A computed run of another
shape (a list, `null`, a record without `text`) is dropped and reported once at the node's `spans`. A node may give
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

### Charts

A `chart` plots points against a numeric axis, optionally joined, with horizontal reference lines: a control
chart, a trend, a stability plot. It is a palette node rather than a host component, so it reaches every client and
backend, and what it plots, marks and says is the interpreter's ([`interpreter.md`](interpreter.md#charts)).

- A **series** draws one point per element of `points`. `y` (required), `x`, `xLabel`, `tone` and `note` are read
  once per point, with `row` the point's element, as a table's `key` is; no row scope is made, so they read no name
  a row scope would declare. `y` is a number or a quantity, and `null` leaves a gap; `x` is a number, a date or an
  instant, and without `x` on any series a point sits at its index; `xLabel` labels the point on the horizontal
  axis; `tone` (`neutral`, `info`, `ok`, `warn`, `err`, or `null`) marks it; `note` is text beside it. `label` is
  the series' name in the legend, required when there are two or more series. `joined` (a literal `bool`, default
  `true`) joins the points.
- A **line** is drawn at `value` (required, a number or a quantity), in its `tone` (default `neutral`), with
  `style` `solid` (the default), `dashed` or `dotted`; `label` defaults to the value at `yPlaces`.
- `yMin` and `yMax` fix the ends of the value axis; `yPlaces` is the places its labels are shown at; `yLabel` names
  its unit.

The loader refuses, each at its path: a chart without `summary` or `series`; a series without `points` or `y`; two
or more series where one has no `label`; `x` on some series and not on others; more than 4 series or 16 lines; a
`yPlaces` that is not a literal integer from 0 to 18; a `style` other than the three; a literal `tone` that names no
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
  `progress`, `pending` or `error`. A mutation is read through `result`, `pending` and `error`, a
  model alias not at all.

## Load-time rules

`loadDocument` returns the first refusal; each rule has a case in
`tests/ui/corpus/documents.json`.

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
  There is also no bound on a `custom` node (which carries no common properties). A literal `sizing` is
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
  a fallback. A `customize` node names a declared point; a `columns` point names a table; a `choice`
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
