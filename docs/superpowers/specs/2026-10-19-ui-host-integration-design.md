# Hosting `ui/1` screens in an application's own Qt Quick shell and kit — design

An application that already has a hand-written Qt Quick GUI (its own shell with a navigation rail, tab workspace, page
header and status bar, and its own component kit) moves its screens onto `ui/1` documents rendered by
`morph::qt_quick`. The documents say what each screen shows and sends; the application's kit draws it; the shell, the
tabs, the page chrome and the dialogs look and behave as before. This design closes the twenty gaps such a port hits
at `e707ba30`, as one initiative on one branch.

## Contents

[1 Intent](#1-intent) · [2 Embedding](#2-embedding-the-engine-hook-and-several-mounts) ·
[3 Restyle kinds](#3-restyle-kinds-text-area-table-row-scroll-bar-splitter-handle) ·
[4 Field chrome](#4-field-chrome-required-caption-unit) · [5 Row tone](#5-row-tone) · [6 Busy](#6-busy) ·
[7 Theme](#7-theme-tone-variants-font-family-icon-set) · [8 Named parts](#8-named-parts-the-screen-head-and-overlay-actions) ·
[9 Field components](#9-a-form-field-drawn-by-a-host-component) · [10 File bytes](#10-file-bytes-in-a-document) ·
[11 Options from metadata](#11-options-from-field-metadata) · [12 List concatenation](#12-list-concatenation) ·
[13 Result fields](#13-a-mutation-results-fields-at-compile-time) · [14 Host helpers](#14-host-helpers-identity-key-and-catalog-title) ·
[15 Workspace model](#15-the-workspace-model) · [16 Client component](#16-the-client-component) · [17 Tests](#17-tests) ·
[18 Delivery](#18-delivery) · [19 Traceability](#19-traceability) · [20 Out of scope](#20-out-of-scope)

## 1. Intent

**Today**, the port re-implements framework code and still loses look and behaviour:

- `qt_quick::Frontend` owns its `QQmlEngine`, so `Components::restyle` and `applyTheme` cannot run before the backend
  exists; the application copies `src/qt_quick/frontend.cpp`. A bundle's theme (or the defaults, for a bundle without
  one) replaces whatever the application applied.
- The client source and shell are example code (`examples/common/app`, ~1.8k lines), with the sign-in reply fixed to
  `{ok, principal, token}`.
- Table rows, multi-line inputs, scroll bars and splitter handles keep MorphUi's look whatever the kit. A required
  field shows its marker twice (`" *"` in the label, `*` from `MorphField`); a unit is a separate muted text beside the
  input; a standalone input has no visible caption; `rowTone` is ignored; an inactive `busy` still says "Loading…".
- The theme has no font family, the tone `.bg`/`.border`/`.dot` tokens reach no `Theme` property, and the `icons`
  index field is not read.
- A screen's head is a title only, a dialog has no footer, a form field cannot be drawn by the kit's grid, a plain
  member cannot become a lookup select, an expression cannot prepend `"(none)"` to a list, `upload`/`download` need the
  server file store, and `m.result().at(&R::x)` compiles and then refuses the screen at registration.
- Identity comparison and catalog titles are internal, the several-mounts-per-window pattern is unstated, and the tab
  logic of `ui::Workspace` cannot be used under an application's own tab strip.

**With this design**, the application keeps its `QQmlEngine` setup in a `FrontendConfig::onEngine` hook (or builds
`qt_quick::Backend` itself, a pattern now specified and tested), maps its kit over every drawn part, lets the theme
set the font, and gets the head parts, footers, field components, inline file bytes, metadata options, list
concatenation and a model-only workspace from the framework. The client source and shell are an installed component,
`morph::client`. Every misconfiguration this adds — an unknown or incomplete restyle, a malformed part, an oversize
file, conflicting options — is refused or reported, never ignored.

## 2. Embedding: the engine hook and several mounts

### 2.1 `FrontendConfig::onEngine` (#1002-1)

```cpp
// include/morph/qt_quick/frontend.hpp
enum class BundleTheme : std::uint8_t { Apply, Ignore };

struct FrontendConfig {
    // … mountShell, components, title, onWidthClass, devTools as today …
    /// Called once per run with the engine and the window, after both exist and before the Backend is built.
    std::function<void(QQmlEngine& engine, QQuickWindow& window)> onEngine;
    /// Whether the bundle's theme is applied when the source opens.
    BundleTheme bundleTheme = BundleTheme::Apply;
};
```

- `run` step 5 becomes: the engine, the window (size, title), `onEngine(engine, window)`, then the `Backend`. Restyles
  and `applyTheme` made there reach every widget the shell creates, since `Context` resolves a kind's component on
  its first use.
- `bundleTheme == Apply` keeps today's rule (the bundle's theme index, or the defaults for a bundle without one).
  `Ignore` leaves the theme as `onEngine` set it; a bundle that carries a theme index is then reported once on
  `std::cerr` (`the bundle's theme is not applied: FrontendConfig::bundleTheme is Ignore`), the channel `run` already
  uses for theme problems (`frontend.cpp:146`). With `Apply`, `run` snapshots the `Theme` singleton's writable
  properties before and after the hook (all but its `var` properties, the schemes a theme declares: their JavaScript
  values never compare equal, even to themselves); when the hook changed any and the open then applies the bundle's
  theme (or the defaults, for a bundle without one, `theme.cpp:186-188`), it reports once on `std::cerr`
  (`the theme onEngine applied is replaced by the bundle's; set FrontendConfig::bundleTheme to Ignore to keep it`).
- An `onEngine` that throws: `run` destroys the window, the engine, the scheduler, the runtime and the executor in
  the documented order and rethrows. Nothing is mounted.
- There is no variant taking an application-owned engine. An application that must own those objects builds
  `qt_quick::Backend` itself (§2.2).

**Specs.** `qt_quick/frontend.md`: Shape (the two members), Run (the hook between window and backend; the bundle
theme rule), Quit (a throwing hook). `ui/theme.md` "In the client": the bundle theme is applied unless the frontend
is told to ignore it. **Terminal:** no counterpart; `tui::FrontendConfig` is unchanged.

### 2.2 Several mounts in one window (#1003-4)

The supported pattern, stated in `qt_quick/backend.md` (a new section, *Several mounts*) and `ui/host.md` (a new
section, *A shell of its own*):

| Shared by every mount | Per mount |
|---|---|
| `ui::Session` (cache, locale, translations), `reactive::Runtime`, `Dispatcher`, `QQmlEngine`, `qt_quick::Components` (restyles, host and bundle components), the `Theme` singleton (one per engine) | `qt_quick::Backend` and its `Context` (the per-type component cache), `DocumentScreenHost` (its interpreter, close guard, application host and `diagnostics` callback), the screens and their handles |

- A restyle is resolved per `Backend` when the type is first made, so a restyle mapped after a `Backend` made that
  type reaches only backends made later. Restyles are mapped before the first `Backend` (in `onEngine`, or before the
  application builds its backends).
- Every `Backend` is destroyed before the engine; every `DocumentScreenHost` before the `Session`; destroying one
  mount leaves the others working.
- The sentence "every root a mount makes is appended to one column the backend adds to `host`" stays; the new
  section adds that a page item of the application's own shell is such a `host`, one `Backend` per page.

**Misuse.** Nothing new to refuse: the existing rules (a `DocumentScreenHost` needs a backend and a document lookup)
cover it.

## 3. Restyle kinds: text area, table row, scroll bar, splitter handle

`kRestylable` (`src/qt_quick/components.cpp`) grows from 22 to 26 entries. The two kinds of restyle are stated in
`qt_quick/backend.md` "Components":

- **A kind restyle** replaces a kind's whole component (today's 22, plus `textArea`).
- **A part restyle** replaces one drawn part inside a component C++ creates, never its layout. The container's
  component declares the part as a `Component` property; C++ passes `Context::component(<part type>)`, restyled or
  not, as an initial property when it makes the container. The layout (placement, the flickable, the split view)
  stays in MorphUi.

Both are checked by `declaredNames` against the part's or kind's MorphUi base, so a candidate that does not declare
every property and function of the base is refused at `restyle`, and the kind keeps its MorphUi component (reported
through the `Components` callback and `failures()`, as today).

| Restyle kind | MorphUi base | What it replaces | Base API (beyond its Qt type's) |
|---|---|---|---|
| `textArea` (#1003-6) | `MorphTextArea` | the multi-line text input | `MorphField`'s field API (`readOnly`, `required`, `errors`, `stale`, `caption`, `labelShown`, `control`), `placeholder`, `edited`, `assigning`, `assign(value)` |
| `tableRow` (#1002-7a) | `MorphTableRowBackground` (new) | a table row's background: selection, hover, focus, zebra, tone, and the row's minimum height | `selected`, `hovered`, `current`, `alternate`, `tone` (−1 none, else `ui::Tone`), `scheme` |
| `scrollBar` (#1002-8) | `MorphScrollBar` (new, a `ScrollBar`) | the bar `MorphScroll` attaches on its axis | `scheme` |
| `splitterHandle` (#1002-8) | `MorphSplitterHandle` (new) | the `SplitView.handle` between panes | `axis`, `scheme` (the handle reads `SplitHandle.pressed` and `.hovered`) |

- `MorphTableRow` keeps its placement (`place()`, `widths`, `content`) and gains `property Component background`,
  `hovered` (a `HoverHandler`), `current` (active focus within the row), `alternate` and `tone`. C++
  (`TableImpl`) writes `alternate` (odd position) on each row after every append, destroy and move. The row's
  `implicitHeight` is the larger of its cells' and the background's `implicitHeight`; the stock background's is
  today's dense height (`Theme.controlHeight - 8`), so the stock look is unchanged.
- `MorphScroll` gains `property Component bar`, instantiated for its axis; `MorphSplitter` gains
  `property Component handle` bound to its `SplitView.handle`. The stock parts reproduce today's Basic bar and handle.
- A `scrollBar` restyle must be a `ScrollBar` subclass: `declaredNames` lists every property and method beyond
  `QQuickItem`'s (`components.cpp:64-74`), so the base's are all of `ScrollBar`'s and only a subclass declares them.
- `MorphSplitter` replaces its constant `handleExtent: 6` with the extent of the instantiated handle along the axis
  (its `implicitWidth` or `implicitHeight`), so a restyled handle of another thickness sizes the splitter correctly.
- The comment over `kRestylable` and the spec sentence "the layout kinds … are not restyled" become: the layout kinds
  are not restyled as kinds; a scroll area's bar and a splitter's handle are restyled as parts. `scroll`, `splitter`,
  `column`, `row`, `grid`, `spacer`, `slot` and `dropZone` remain refused as kinds, with a reason naming the part kind
  where there is one (`` `scroll` is a layout kind; restyle its bar as `scrollBar` ``).

**Terminal.** Restyling is a `qt_quick::Components` call; the terminal has none and draws as before.

**Specs.** `qt_quick/backend.md`: Components (kind and part restyles, the table above), Kinds (Table: rows with a
background part; Scroll and Splitter: their parts), Tests.

## 4. Field chrome: required, caption, unit

One rule, stated in `forms/engine.md` "Rendering" and `ui/view_tree.md`: **inside a form, the field's cell (the
column `formView` draws per field) owns the caption; the input owns the required marker and the unit.** Outside a
form, a standalone input draws its caption only when the document asks for it.

### 4.1 One required marker (#1002-5, defect)

`detail::engine::fieldCell` drops `+ (required ? " *" : "")`: the label text is the field's label. The marker is the
input's: `MorphField` draws `*` from `required` (already a `FieldWidget::setRequired` property, set from
`fieldState`), and the terminal's `TuiField` paints `*` in its marks. Both backends therefore showed it twice; they
now show it once. `statusText`'s "fill the required (*) fields" still names the mark both draw.

**Specs.** `forms/engine.md`: "A cell is the label (with ` *` while required, live)" becomes "the label, the control
(which marks a required field) and the help". **Misuse:** none.

### 4.2 A visible caption on standalone inputs (#1002-6)

- **Document.** `textInput`, `select` and `dateTimeInput` gain `showLabel` (a literal `bool`, set once; default
  `false`; a literal-flag key, not an expression). With `true`, the `label` is also drawn as a visible caption.
  `label` stays the accessible name either way. A non-bool `showLabel`, or `showLabel` without a `label`, is refused
  at load.
- **View tree.** `ui::TextInput`, `ui::Select` and `ui::DateTimeInput` gain `Prop<std::string> label` and
  `bool showLabel = false`. `formView` sets `label` to the field's label and leaves `showLabel` false: the cell draws
  the caption, so a kit never doubles it.
- **Backend contract.** `TextInputWidget`, `SelectWidget` and `DateTimeInputWidget` each gain
  `virtual void setCaption(std::string_view label, bool shown) = 0;` — the field's label, and whether the field draws
  it as a visible caption. `FieldWidget`'s other subclasses (text, checkbox, slider, file picker) are unchanged. The
  mount calls it for the three kinds only when the input has a `label` (bound or non-empty) or `showLabel`, so an input
  without a label is told nothing (`RecordingBackend` stores the properties `caption` and `captionShown`).
- **Qt Quick.** `MorphField` gains `property string caption` and `property bool labelShown`, and lays the caption out
  above its control while `labelShown`. (Not `label`: `MorphCheckbox` already declares `label` for its own text.) A
  kit restyle reads `caption` with `labelShown` false to know its label is drawn elsewhere.
- **Terminal.** A field with `shown` draws the caption on its own row above the control; without it, nothing changes.
- **Room for #996 §3.** A form's future field `layout` (`stacked`, `inline`, `columns`) changes how the cell places
  its caption beside or above the control; the input API above does not change with it.

**Specs.** `ui/document.md` (the node table), `ui/interpreter.md` Lowering ("A field's `label`" gains `showLabel`),
`ui/view_tree.md`, `ui/backend_contract.md` (`setCaption`), `qt_quick/backend.md`, `tui/frontend.md`.

### 4.3 The unit belongs to the input (#1002-4)

- **View tree.** `ui::TextInput` gains `Prop<std::string> unit` (empty: none) and `int decimals = -1` (set once; −1:
  not a number field).
- **Backend contract.** `TextInputWidget` gains `setUnit(std::string_view unit)` and `setDecimals(int decimals)`.
  The mount calls `setUnit` only for an input with a unit (bound or non-empty) and `setDecimals` only for
  `decimals ≥ 0`, so a backend sees only what a field declares. A `Multiline` input with either is refused with
  `std::logic_error` (a multi-line input holds no number).
- **Forms engine.** `fieldControl` sets `unit` from `spec.unit` (or the overlay's unit) and `decimals` from the entry
  unit's places for Integer (0), Number, Decimal and single-unit Quantity fields; `unitSuffix` and its muted text go
  away. A Quantity with alternatives keeps its unit `Select` beside the input, and its input gets no `unit`.
- **Qt Quick.** `MorphTextInput` gains `unit` and `decimals`; it draws the unit as a muted trailing label inside the
  field (the `TextField`'s right padding grows by the label's width). `decimals` is carried for a kit to read; the
  stock input draws nothing from it.
- **Terminal.** The text input draws the unit, muted, at its right edge before the field marks; the editable width
  shrinks by the unit's display width plus one cell.
- The test id `unit_<path>` stays on the Quantity unit `Select` only.

**Specs.** `forms/engine.md` (the control table: "TextInput, with the unit beside it" becomes "TextInput showing its
unit"), `ui/view_tree.md`, `ui/backend_contract.md`, `qt_quick/backend.md`, `tui/frontend.md`.

**Restyle compatibility (4.2, 4.3).** `declaredNames` asks a restyle to declare every name of its base, so a
`textInput`, `select` or `dateTimeInput` restyle written from scratch is refused until it declares `caption`,
`labelShown` (and, for `textInput`, `unit` and `decimals`); one that extends the MorphUi component passes. The refusal
names the missing properties. `qt_quick/backend.md` states that a base's new properties are part of its restyle API.

## 5. Row tone (#1002-7b)

- **Document.** `rowTone` (already in `ui/1`, in the row scope) is lowered: it evaluates to `"neutral"`, `"info"`,
  `"ok"`, `"warn"`, `"err"` or `null` (no tone). Another value is reported once per path (`rowTone is <v>; a tone is
  neutral, info, ok, warn or err`) and shows no tone.
- **View tree.** `ui::table` gains an overload whose row builder returns
  `TableRowView{std::vector<Node> cells; std::optional<Prop<std::optional<Tone>>> tone;}`, so the tone binds in the
  row's own scope. `ui::Row` gains `std::optional<Prop<std::optional<Tone>>> tone`, which only that overload's row view
  sets. Disengaged means no tone was given; an engaged `Prop` (constant or bound) whose value is `nullopt` means no
  tone at the moment. The refusal is therefore well defined: the mount throws `std::logic_error` for a `Row` whose
  `tone` is engaged outside a table, like `setRowKey` of a non-row. (A `Prop<std::optional<Tone>>` alone cannot tell
  "not given" from "given as none", `view.hpp:104-110`.)
- **Backend contract.** `TableWidget::setRowTone(Widget& row, std::optional<Tone> tone)`, called after `setRowKey`
  and on every change. The mount binds it through the table's row hook, whose signature takes the row's scope and
  view node as well as its widget and key (`RowHook = std::function<void(reactive::Scope&, Node const&, Widget&,
  Key const&)>`), so the tone binds in the row's own scope.
- **Overloads.** With both `ui::table` overloads taking a `std::function`, an empty `{}` row builder is ambiguous; a
  caller passing an empty one names its type.
- **Qt Quick.** `TableImpl` writes the row's `tone`; the stock row background fills with the tone's `.bg` colour
  (§7.1) and draws a 3 px leading bar in its `.dot` colour; a selected row shows the selection colour and keeps the
  bar.
- **Terminal.** A toned row's cells draw in the tone's style (`toneStyle`); a selected row keeps its inverse.

**Specs.** `ui/interpreter.md` ("`rowTone`, `sort`, `filters` and `hoveredKey` are not lowered" loses `rowTone`),
`ui/view_tree.md`, `ui/backend_contract.md`, `qt_quick/backend.md` (Tables), `tui/frontend.md`, `ui/author.md` (the
`rowTone` option now has an effect).

## 6. Busy (#1002-9, defect)

`MorphBusy` hides its label as well as its indicator while inactive, and its implicit size is 0×0 then, as its own
comment ("shown while active") and the terminal (`BusyImpl::naturalSize`) already say. **Specs:** `qt_quick/backend.md`
Kinds: "a `BusyIndicator` and a label, both shown only while active". **Misuse:** none.

## 7. Theme: tone variants, font family, icon set

### 7.1 Tone variants on `Theme` (#1002-10a)

- `Theme` and `MorphScheme` gain twelve colour properties: `successBg`, `successBorder`, `successDot`, `warnBg`,
  `warnBorder`, `warnDot`, `errorBg`, `errorBorder`, `errorDot`, `infoBg`, `infoBorder`, `infoDot`, defaulting to the
  `kThemeTokens` values. `applyTheme` maps `color.<ok|warn|err|info>.<bg|border|dot>` onto them (and onto each
  surface's scheme through `kSchemeProperties`), and reports a property it could not set, as today.
- `MorphWidget` gains `toneBgOf(index)`, `toneBorderOf(index)` and `toneDotOf(index)` beside `toneOf`. `MorphBadge`
  and `MorphBanner` fill with `toneBgOf` and outline with `toneBorderOf` (the defaults are the tints they approximate
  today with `Qt.alpha(tone, 0.15)`); the row tone (§5) uses them.
- **Terminal.** The terminal does not map theme tokens (`ui/theme.md` "Not yet"), so nothing changes there.
- **Specs.** `ui/theme.md` "On Qt Quick": the table gains the twelve rows; the sentence "The semantic `bg`, `border` and
  `dot` colours have no `Theme` property yet" is removed.

### 7.2 `font.family` and units from the theme's font (#1002-10b)

- **Token.** `font.family` (DTCG `fontFamily`: a name or a list of names; default: none) joins the closed set.
  `ThemeTokenKind` gains `FontFamily`; `ThemeTokenSpec` a family default (empty); `ThemeValues` gains
  `std::map<std::string, std::vector<std::string>, std::less<>> families`.
- **Qt Quick.** `Theme` gains `property list<string> fontFamilies` (empty by default) and `readonly property font
  bodyFont` (the families, else the platform font's family, at `bodyPixels`). Every MorphUi text and control sets
  `font: Theme.bodyFont` (a heading: the same families at `headingPixels`, bold). **Units are opt-in:** `Theme` keeps a
  separate `platformMetrics: FontMetrics {}` over the platform font, from which `bodyPixels` derives its size when
  `font.size.body` is 0, so the size never depends on the metrics it feeds. `metrics.font` is
  `fontFamilies.length > 0 ? Qt.font({families: fontFamilies, pixelSize: bodyPixels}) : platformMetrics.font`. While
  `fontFamilies` is empty the units, and every `fixed(n)`, are unchanged; once a theme sets `font.family` they follow
  the theme's font. Binding `metrics.font` to `bodyFont` instead would loop (`bodyFont` → `bodyPixels` →
  `metrics.font`, `Theme.qml:11-13, 33-34`). A family Qt cannot resolve falls back to Qt's own substitute, and
  `applyTheme` reports it (`font.family: none of <names> is installed`), comparing `QFontInfo(font).family()` against
  the list.
- **Terminal.** A terminal draws in its own font; `font.family` has no effect there.
- **Specs.** `ui/theme.md` (closed set, On Qt Quick, "Not yet" loses "Units from the scale tokens…" for the font),
  `qt_quick/backend.md` Layout and theme ("A unit is the default font's…" gains the theme-font rule; "Not yet" loses the
  density sentence's font part).

### 7.3 Icon set (#1002-10c — the last slice, droppable)

- **Index.** `icons` (today "accepted and not read") is an object of icon name → bundle file id, at most 256 names,
  each matching `[a-z][a-z0-9.-]{0,63}`. `ThemeIndex` gains `std::map<std::string, std::string, std::less<>> icons`.
- **Bundle.** A new file kind `icon` (`kIconFile`): SVG text of at most 64 KiB, digest-verified like every file.
  `UiRegistry::build` refuses an index whose `icons` names a file the bundle lacks or one that is not an `icon` file,
  naming the index and the icon. `ui::Bundle` gains `icons` (name → SVG text), filled by the client loader.
- **Framework names.** The built-in components draw these names: `empty` (an empty state without its own icon),
  `tone.info`, `tone.ok`, `tone.warn`, `tone.err` (banners), `nav.back`, `nav.forward`, `nav.expand`, `nav.collapse`
  (collapsible, panel and navigator chevrons) and `close` (dismissible banners, drawers). MorphUi ships an SVG for each,
  so a bundle without an icon set keeps today's look; a bundle's set replaces any of them by name.
- **Qt Quick.** `applyBundleTheme` hands the set to `Theme.icons` (name → `data:image/svg+xml` URL); a new
  `MorphIcon` draws `Theme.iconSource(name)`. `button`, `badge`, `emptyState` and menu entries draw an icon from the
  set when it names one, and today's text otherwise. When the bundle declares a set, a node icon it does not name is
  reported once per name through the `Components` callback (`icon <name> is not in the theme's icon set`) and drawn
  as text; without a set, nothing is reported.
- **Terminal.** Icons stay text; the framework names map to fixed glyphs (`tone.ok` ✓, `tone.warn` !, `tone.err` ✗,
  `tone.info` i, `nav.expand` ▸, `nav.collapse` ▾, `close` ×).
- **Specs.** `ui/theme.md` (the index, a new section *Icons*), `ui/service.md` (the `icon` file kind, `build`'s
  refusal), `ui/frontend.md` (`Bundle::icons`), `qt_quick/backend.md`, `tui/frontend.md`.

## 8. Named parts: the screen head and overlay actions

One mechanism, typed parts rather than generic slots (#1003-2, #1003-5). Both use one new part kind, `actionButton`:

| Key | Shape | Notes |
|---|---|---|
| `id` | name, required | unique within its list; `[A-Za-z_][A-Za-z0-9_]*` |
| `label` | expression, required | |
| `icon` | expression | an icon name (§7.3) |
| `emphasis` | text | `normal`, `primary`, `danger`, as a `button`'s |
| `enabled`, `visible` | conditions | default `true` |
| `onClick` | commands | run in the scope the list belongs to |

One lowering, `ScreenCore::lowerActionParts(parts, ctx) -> ActionModels`, turns a list into reactive models (label,
icon, emphasis, enabled, visible as bindings in `ctx`'s scope, and a run function). The screen head presents them
through its handle; a dialog or drawer presents them as buttons in its footer.

### 8.1 The screen head (#1003-2)

- **Document.** A screen gains three top-level keys beside `title`: `subtitle` (expression), `status` (parts
  `headBadge`: `{tone, text, icon, visible}`) and `actions` (parts `actionButton`). They are evaluated in the screen
  scope. The app document (no view) refuses them.
- **Host API** (`include/morph/ui/host.hpp`):

```cpp
struct HeadBadge  { std::string text; Tone tone = Tone::Neutral; std::string icon;
                    bool operator==(HeadBadge const&) const = default; };
struct HeadAction { std::string id; std::string label; std::string icon;
                    ButtonEmphasis emphasis = ButtonEmphasis::Normal; bool enabled = true;
                    bool operator==(HeadAction const&) const = default; };
class ScreenHead {
public:
    virtual ~ScreenHead() = default;
    [[nodiscard]] virtual reactive::Computed<std::string> const& subtitle() const = 0;
    [[nodiscard]] virtual reactive::Computed<std::vector<HeadBadge>> const& status() const = 0;   // visible only
    [[nodiscard]] virtual reactive::Computed<std::vector<HeadAction>> const& actions() const = 0; // visible only
    virtual bool run(std::string_view actionId) = 0;
};
// ScreenHandle:
[[nodiscard]] virtual ScreenHead* head() noexcept { return nullptr; }
```

  A document screen's handle answers its head (empty values when the document declares none); a host screen's
  answers null unless it overrides `head()`. `run` runs the action's `onClick` as a user event in the screen scope,
  in one batch; it answers `false` and reports a diagnostic (`head action <id> is unknown`, `… is disabled`, `… is
  hidden`) for an id the list does not hold enabled and visible. After a `remount`, the head follows the new screen,
  as `title` does.
- **Icons.** `HeadAction::icon` and a `headBadge`'s `icon` are carried from this commit on; nothing draws them as
  icons until the icon set (§7.3) lands, so the head's buttons and badges show their text only, and this part stands
  without §7.3.
- **Default shell.** `ui::Workspace` mounts a head row above each tab's screen in its page: the subtitle (muted), the
  badges, a spacer and the actions as buttons, test ids `head_subtitle`, `head_status_<n>`, `head_action_<id>`. The
  row is hidden while the head has no subtitle, badges or actions, so a document without them looks as today.
- **Terminal.** The same row, mounted through the same view tree: one line above the body.

### 8.2 Dialog and drawer actions (#1003-5)

- **Document.** `dialog` and `drawer` gain `actions` (parts `actionButton`, ⓘ: in the overlay's own scope, beside
  `default` and `cancel`). The Enter and Escape bindings of `default` and `cancel` are unchanged.
- **View tree.** A new kind `ui::ActionBar{std::vector<Node> children; Common common;}`. The interpreter lowers the
  overlay's actions as an `ActionBar` at the end of the overlay's scoped content, so its bindings hold the overlay's
  scope as the content does. The mount mounts an `ActionBar` into the footer of the dialog or drawer whose content it
  is in, as a horizontal stack, and refuses it anywhere else (`std::logic_error`: an action bar mounts only in a
  dialog's or drawer's content).
- **Backend contract.** `DialogWidget` and `DrawerWidget` gain `virtual ContainerWidget& footer() = 0;` — a container
  the overlay lays out after its content and that shows nothing while it is empty.
- **Qt Quick.** `MorphDialog` and `MorphDrawer` gain `readonly property Item footer`, laid out under the content,
  right-aligned, with `Theme.spaceOf(2)` between buttons. `footer` is part of their restyle API, so a kit's dialog lays
  the footer out where it wants; a restyle without it is refused by `declaredNames`.
- **Terminal.** The footer is the overlay's bottom row, right-aligned, one cell between buttons.

**Specs (8.1, 8.2).** `ui/document.md` (the document table, the node table, the new part kinds, load-time rules:
unique action ids, required `id` and `label`, the app document refusal), `ui/interpreter.md` (Lowering: the head, the
overlay actions), `ui/host.md` (the contract table: `head()`, `ScreenHead`), `ui/view_tree.md` (`ActionBar`),
`ui/backend_contract.md` (`footer()`), `ui/workspace.md` (the head row), `qt_quick/backend.md`, `tui/frontend.md`,
`ui/author.md` (`screen.subtitle`, `screen.status`, `screen.action`, and `actions` on the dialog and drawer builders).

## 9. A form field drawn by a host component (#1002-3)

- **Document.** A `form` node's `fields` entry gains `component` (a name) and `props` (a record of expressions, as a
  `custom` node's). `component` names a declared `components` entry, which must declare a prop `value` and an event
  `change`; it may also declare `errors` (`json`), `readonly`, `required` (`bool`) and `label` (`string`), which the
  form fills. Load refuses: an undeclared component, one without `value` or `change`, a `props` key the component
  does not declare, and `component` together with `widget` on the same field (both choose the control).
- **Lowering.** `wireForm` builds a `forms::Overrides::byField(name, render)` per such field and passes it in
  `FormViewOptions::overrides`. `render` returns the `ui::custom` node that `lowerCustom` builds (the same prop
  conversion, `minVersion`, and failure reporting), with:
  - `value`: the field's current wire value, as JSON text for a `json` prop (an Object or ObjectArray field's object
    or array), or converted by the prop's declared type; `null` while the field is blank;
  - `errors`, `readonly`, `required`, `label`: from the field's state;
  - the fallback: the field's built-in control (`FieldView::defaultControl()`).
  A `change` event's payload is read as the `value` prop's declared type and written with the new
  `FieldState::edit(Json const&)` (a `load` that counts as a user edit: the field is touched, dirty, and validated),
  so the form keeps owning the draft, validation and submit. A payload that does not decode for the field is reported
  (`field <name>: the component's change does not decode: <reason>`) and changes nothing.
- The cell around the control (caption, help, visibility) is drawn as for every override. Only top-level fields take
  a component, as every overlay does.
- **Qt Quick.** A host or bundle component, loaded as a `custom` node's is.
- **Terminal.** `createCustom` answers null, so the field's built-in control is drawn: an ObjectArray its row panels.
- **Specs.** `ui/document.md` (`form`'s `fields` overlay keys and refusals), `ui/interpreter.md` (Lowering `form`;
  "Not yet lowered": "A `form` node's `overrides`" narrows to subtree overrides), `forms/engine.md` (The field overlay;
  "Not here yet": the grid is reachable through a host component; `FieldState::edit`).

## 10. File bytes in a document (#1002-13)

The server file store stays the path for large files. A document may also move bytes inline, under a hard cap.

- **Cap.** `SessionOptions::maxInlineFileBytes`, default 1 MiB (`kDefaultInlineFileBytes`). The largest admissible
  cap is `kMaxInlineFileBytes = ⌊(wire::kMaxEnvelopeBytes − 64 KiB) · 3 / 4⌋` (`core/wire.hpp:36`: 8 MiB, so
  6 242 304 bytes): base64 grows bytes by 4/3, and 64 KiB is left for the rest of the body and the envelope.
  `Session` construction throws `std::invalid_argument` for 0 and for a cap above `kMaxInlineFileBytes`, naming both.
- **Download.** `download` gains `from` (`"ref"`, the default, or `"bytes"`) and `type` (an expression, the media
  type). With `"from": "bytes"` the operand evaluates to the file's content (a base64 string, RFC 4648 with or without
  padding, or a list of integers 0–255) and `name` is required. The interpreter decodes it; content over the cap, or
  that does not decode, fails the command with a diagnostic (`download: 2.4 MiB is over the 1 MiB inline cap; serve
  it through the file store`) and the host is not called. Otherwise it calls the new
  `InterpreterHost::saveFile(InlineFile const& file, std::string const& option, done)`; `option` is `dialog` or
  `preview`, as for a reference.
- **Upload.** `upload` gains `into` (`"ref"`, the default, or `"bytes"`), and with `bytes` the optional `nameAs` and
  `typeAs` (body member names). With `"into": "bytes"` the interpreter calls the new
  `InterpreterHost::readFile(Value const& file, std::size_t maxBytes, done)`, refuses a reply over the cap even when
  the host did not, and runs the mutation with `as` (default `file`) set to the base64 of the bytes, and `nameAs` /
  `typeAs` set to the name and media type when given. The transfer's `pending`, `progress` and `error` behave as for
  a reference upload.

```cpp
struct InlineFile { std::string name; std::string mediaType; std::string bytes; };
virtual bool readFile(Value const& file, std::size_t maxBytes,
                      std::function<void(std::expected<InlineFile, std::string>)> done);   // default: false
virtual bool saveFile(InlineFile const& file, std::string const& option,
                      std::function<void(std::expected<Value, std::string>)> done);      // default: false
```

  A default answers "not supported", which the interpreter reports, as for `upload` and `download`. A host reads a
  file's size before its bytes and refuses one over `maxBytes` without reading it.
- **Field components (reuse of §9).** The cap applies to a field component's `change` only for a field whose wire
  type is bytes: a `string` member with `contentEncoding: base64` in its schema, which `FormModel::fromSchema` reads into a new `FieldSpec::bytes` flag. A kit's attach
  control writing into such a member is refused above the cap with
  `field <name>: <n> bytes is over the <cap> inline cap`. Any other field's `change` is not held to it.
- **Client.** `client::ApplicationHost` (§16) implements both: `readFile` from the picked path,
  `saveFile` into the downloads directory (`dialog`), `preview` not supported, as for references. It runs under both
  frontends.
- **Load-time rules.** `from` and `into` are closed choices; `from: "bytes"` without `name`, and `nameAs`/`typeAs`
  without `into: "bytes"`, are refused.
- **Specs.** `ui/document.md` (the command table and its targets), `ui/interpreter.md` (Commands: `upload`,
  `download`), `ui/service.md` (Files: the inline path and when to use the store), `ui/frontend.md` (`SessionOptions`).

## 11. Options from field metadata (#1002-11)

- **API.** `forms::FieldMeta` gains, at its end (designated initialisers stay valid), `std::string_view
  optionsAction{}`, `std::string_view valueField{}` and `std::string_view labelField{}`. A member whose meta sets
  `optionsAction` is emitted with the same `x-optionsAction`, `x-optionValue`, `x-optionLabel` as a `Choice<…>`, so the
  forms engine reads it as a `Choice` field with no client change.
- **Refusals,** thrown from schema generation as `UnsatisfiableFormError` is (`std::logic_error`, at registration):
  a new `ConflictingOptionsError` when the member is a `Choice<…>` and its meta also sets `optionsAction`; and an
  `IncompleteOptionsError` when `optionsAction` is set without both `valueField` and `labelField`, or either of those
  without `optionsAction`.
- `x-optionsDependsOn` stays `Choice`-only; a document-side field overlay for options is out of scope.
- **Specs.** `forms/forms.md` (FieldMeta), `forms/choice.md` (the second source of the annotations, the refusals).
- **Terminal and Qt Quick.** Unchanged: the field is a `Choice` field on both.

## 12. List concatenation (#1002-12)

- **Function.** `concatLists(list, ...)`, one to 255 arguments, in the Lists group of `ui/expressions.md`. Each
  argument must be a list where its type is known (checked like `count`); the static type is `list<T>` when every
  known element type is `T`, else `list<any>`. Evaluation follows "absence propagates": a `null` argument makes the
  result `null` (an author writes `coalesce(q, [])` for a query that is still loading); any other non-list argument is
  a mismatch, reported (`` `concatLists` takes lists ``). `concat` stays strings-only.
- **Author API.** `ui::author::concatLists(std::vector<AnyHandle> const&)` and a typed
  `template <typename T> Handle<std::vector<T>> concatLists(std::vector<Handle<std::vector<T>>> const&)`.
- **Specs.** `ui/expressions.md` (Functions table and a bullet), `ui/author.md` (the helper).

## 13. A mutation result's fields at compile time (#1002-14, defect)

- **API.** `MutationHandle<A>::result()` returns `ResultHandle<Result>`, a `Handle<Result>` whose `at` overloads are
  deleted:

```cpp
template <typename T>
class ResultHandle : public Handle<T> {
public:
    explicit ResultHandle(Handle<T> const& handle) : Handle<T>{handle} {}
    // A result is not a named read: bind it, `auto r = screen.let("r", m.result());`, then read `r.at(...)`.
    template <typename Class, typename Member> Handle<Member> at(Member Class::* member) const = delete;
    Handle<Any> at(std::string_view field) const = delete;
};
```

  `result()` builds it as `ResultHandle<Result>{Handle<Result>::raw(detail::single("result", _name))}`: `raw` is
  static and answers a `Handle<T>`, and `_json` is private to `Handle` (`author.hpp:636-643, 698`). It converts to
  `Handle<T>` everywhere a handle is taken, so `screen.let(name, m.result())`, comparisons and
  `orElse` compile as before. The compiler's error points at the deleted declaration, whose comment names the fix.
- The runtime refusal in `detail::fieldOf` (`AuthoringError`) stays the second line, for a `{"result": …}` expression
  built through `Handle<Any>::raw`. Its test reaches `at` by converting the result handle first,
  `Handle<ResultView>{capture.result()}.at(&ResultView::id)`, since `result().at` no longer compiles.
- **Specs.** `ui/author.md` (the handle table; "throws `AuthoringError` on any other expression (a call, a
  `result`)" becomes: a `result()` handle has no `at` at compile time; a call is refused at registration).

## 14. Host helpers: identity key and catalog title (#1003-3)

```cpp
// include/morph/ui/host.hpp
[[nodiscard]] std::optional<std::string> identityKey(Value const& identity);   // canonical JSON; nullopt for null
[[nodiscard]] bool sameIdentity(Value const& a, Value const& b);               // equal keys, null equals nothing
// include/morph/ui/catalogue.hpp
[[nodiscard]] std::string catalogTitle(CatalogEntry const& entry, MessageCatalogues const* catalogues,
                                       std::string_view locale);
[[nodiscard]] std::vector<std::string> catalogTitles(CatalogEntry const& entry, MessageCatalogues const* catalogues);
```

- `identityKey` wraps `detail::canonicalJson` and is the text `DocumentScreenHost::identityOf` answers, so
  `identityOf(id, params) == identityKey(handle.identity().peek())` is the documented comparison.
- `catalogTitle` is a string title as is; a message reference `{"t", "fallback"}` formatted in `locale` from
  `catalogues` (the exact locale, then its language), else its `fallback`; else the entry id. `catalogTitles` is the
  title in every locale the catalogues hold (`textInEveryLocale`), for a search. With `catalogues` null,
  `catalogTitles` is one title: a string title as is, else a message reference's `fallback`, else the entry id —
  exactly what `Navigator::titlesOf` answers today without `titles` (`navigator.hpp:144-162`).
- `Navigator` uses `catalogTitles(entry, nullptr)` where it reads titles itself today; `Workspace` uses `identityKey`.
- **Specs.** `ui/host.md` (DocumentScreenHost: `identityOf` and the comparison), `ui/workspace.md` (Navigator: the
  fallback is `catalogTitles`), `ui/catalogue.md` (the two functions).

## 15. The workspace model (#1003-1)

`ui::WorkspaceModel` (`include/morph/ui/workspace_model.hpp`) holds the tab logic with no widgets; `ui::Workspace`
becomes the tab-bar view over it, its constructor, behaviour and spec unchanged.

```cpp
struct WorkspaceTabId { std::uint64_t value = 0; auto operator<=>(WorkspaceTabId const&) const = default; };
struct WorkspaceTabState { WorkspaceTabId id; std::string screenId; Value identity; std::string title; bool dirty;
                           bool operator==(WorkspaceTabState const&) const = default; };
struct WorkspaceModelOptions : WorkspaceOptions {          // screens, hosts, onRecord, scheduler, recordDelay
    /// Makes a tab's page and mounts the screen in it; the model owns the handle it answers.
    std::function<std::expected<std::unique_ptr<ScreenHandle>, MountError>(
        WorkspaceTabId tab, std::string_view screenId, ParamMap const& params)> mountPage;
    /// Called after a tab's handle is destroyed, so the application destroys its page.
    std::function<void(WorkspaceTabId tab)> unmountPage;
};
class WorkspaceModel {
public:
    WorkspaceModel(reactive::Runtime& runtime, DocumentScreenHost& host, WorkspaceModelOptions options);
    std::expected<WorkspaceTabId, MountError> open(std::string_view screenId, ParamMap const& params);
    std::size_t start(WorkspaceStart const& how);
    void activate(WorkspaceTabId tab);  void next();  void previous();
    void close(WorkspaceTabId tab, std::function<void(bool)> done);
    void closeOthers(WorkspaceTabId tab, std::function<void(bool)> done);
    void closeAll(std::function<void(bool)> done);  void closeSaved(std::function<void(bool)> done);
    void forceCloseAll();  void flushRecord();  [[nodiscard]] std::vector<std::string> record() const;
    [[nodiscard]] reactive::Computed<std::vector<WorkspaceTabState>> const& tabs() const;  // in order
    [[nodiscard]] reactive::Computed<std::optional<WorkspaceTabId>> const& active() const;
    [[nodiscard]] ScreenHandle* handleOf(WorkspaceTabId tab) const noexcept;
};
```

- `open`, `singleton`, `reuseEmpty`, the close protocol with the guard, the most-recently-used walk, the record and
  `resume` are the existing rules, moved; they are keyed by tab id, as `Workspace` already keys them internally.
  `DocumentScreenHost` is borrowed for `identityOf`, `routeOf` and `parseRoute`; mounting is `mountPage`'s.
- `tabs()` and `active()` are observable: an application's tab strip binds to them. A tab's `title`, `dirty` and
  `identity` follow its handle.
- Host screens: the model passes their ids to `mountPage` like any other; `Workspace`'s own `mountPage` mounts them
  from `hosts`, as today.
- `Workspace` builds a `WorkspaceModel` whose `mountPage` makes a page slot of its bar (and the head row of §8.1) and
  mounts into it, and drives `setTabs`/`setSelected` from `tabs()`/`active()`. It keeps the three chords, `root()`,
  index-based `close`/`activate`/`handleOf` over the model's order, and exposes `model()`.
- **Misuse.** An empty `mountPage` is `std::invalid_argument` at construction; a `mountPage` that throws is reported
  as the `MountError` it would have answered, and opens nothing; a `WorkspaceTabId` the model does not hold (never
  issued, or closed) is `std::out_of_range` from every member that takes one except `handleOf`, which answers null.
- **Terminal.** The model has no widgets; `Workspace` runs on both backends as today.
- **Specs.** `ui/workspace.md`: a new first section *Model* with the API, the existing sections restated as the
  model's rules, and *Shape* describing `Workspace` as the bar over it.

## 16. The client component (#1002-2)

- **Target.** `morph::client` (`morph_client`, a compiled STATIC library like `morph::tui`), behind
  `MORPH_BUILD_CLIENT` (default `ON`: it needs only `morph`). Its sources are `src/client/`; its headers
  `include/morph/client/{args,source,shell,transport}.hpp`, namespace `morph::client`. The Qt WebSocket transport is
  compiled in when `morph_qt_impl` exists, the raw-socket one when `morph::net` does, as in the example library today.
  It installs and exports as component `client` (`EXPORT_NAME client`, `MORPH_INSTALLED_COMPONENTS`), and
  `scripts/check_install_export.sh` checks it. `MORPH_BUILD_EXAMPLES` with `MORPH_BUILD_CLIENT=OFF` fails at configure,
  naming the option.
- **API** (moved from `morph::examples`, renamed into the namespace): `client::Args` and `readArgs`;
  `client::Source` (was `ClientSource`), `RemoteSource`, `LocalSource`, `LocalApp`, `LocalSetup`;
  `client::RemoteTransport` and `makeTransport`; `client::ApplicationHost`; `client::ShellOptions` and `mountShell`.
- **Sign-in mapping.**

```cpp
struct SignedIn { std::string principal; std::string token; };
/// Maps a sign-in mutation's reply to the principal and token to install; nullopt refuses the sign-in.
using SignInMapper = std::function<std::optional<SignedIn>(ui::Value const& reply)>;
[[nodiscard]] std::optional<SignedIn> defaultSignIn(ui::Value const& reply);   // {ok, principal, token}
// ShellOptions:
SignInMapper signIn = defaultSignIn;
```

  An empty `signIn` is `std::invalid_argument` at `mountShell`. A mapper that answers a `SignedIn` with an empty
  principal refuses the sign-in and reports it through `onProblem` (`sign-in mapper answered no principal`).
- **The application host.** Sign-in and the file commands live today in the example shell's private
  `InterpreterHost` (`examples/common/app/client_shell.cpp:78-169`), so a shell of the application's own would get
  neither. The component exports it:

```cpp
class ApplicationHost final : public ui::InterpreterHost {
public:
    ApplicationHost(Source& source, SignInMapper signIn, std::filesystem::path downloads,
                    std::function<void(SignedIn)> signedIn, std::function<void()> signedOut);
    bool signIn(ui::Value const& reply) override;          // through the mapper
    void signOut() override;
    bool uploadFor(ui::UploadRequest const& request, …) override;   // the file store, as today
    bool download(ui::Value const& file, …) override;
    bool readFile(ui::Value const& file, std::size_t maxBytes, …) override;   // §10
    bool saveFile(ui::InlineFile const& file, …) override;                  // §10
};
```

  `mountShell` builds one from `ShellOptions` (`signIn` is forwarded to it; `downloads` as today) and passes it as
  `DocumentScreenHostOptions::application`; an application with its own shell constructs one and passes it there
  itself. An empty mapper is `std::invalid_argument` at its constructor.
- **Document lookup.** `client::Source::documents()` answers the lookup `DocumentScreenHostOptions::documents` takes,
  over the loaded bundle, so an application that builds its own shell mounts with the same source.
- **Examples.** `examples/common/app` is removed; the client and the rungs' screen tests link `morph::client`; its
  tests move to `tests/client/` (with their probe application).
- **Specs.** A new `docs/spec/client/client.md` (packaging, the sources, sign-in mapping, the shell, transports, the
  application host's file commands, tests); `ui/service.md` "The generic client" points at it; `docs/spec/README.md`
  lists it.

## 17. Tests

Each test names the change that must make it fail, and is run once against that mutation before the item's commit.

- **#1002-9 busy.** Qt: an inactive `busy` labelled "Loading…" has an invisible label and a 0×0 implicit size; active,
  both visible. *Mutation: restore `visible: true` on the label.*
- **#1002-5 marker.** A form with one required field: on Qt the cell's label reads `Name` and exactly one visible `*`
  item is in the cell; on the terminal the cell's rows contain one `*`. *Mutation: restore the suffix.* Commit 2 also
  updates every test and example asserting a `"… *"` label (among them `tests/test_forms_engine_view.cpp:123,170`
  and `tests/tui/test_tui_forms.cpp:97-98`).
- **#1002-14.** A unit test with `static_assert(!requires(MutationHandle<A> m) { m.result().at(&R::x); })`, the same
  for `at("x")`, and `static_assert(requires { screen.let("r", m.result()); })`; the existing runtime-refusal test
  stays. *Mutation: delete the deleted overloads; the first assertion stops compiling the test.*
- **#1002-7a / #1002-8 / #1003-6 restyles.** For each new kind: a restyle extending the base is accepted and its item
  is the one made (a marker property read back from the row background, the scroll bar, the handle, the text area);
  one missing a base property is refused, naming it; `scroll` as a kind is refused naming `scrollBar`. A row
  background with `implicitHeight: 40` makes the row 40 px tall; `alternate` is true on the second row and follows a
  move. *Mutation: drop the `kRestylable` entry, or pass the stock part instead of `Context::component`.*
- **#1002-10a.** A theme setting `color.ok.bg` to `#123456` sets `Theme.successBg`, and an ok badge's fill reads it;
  `applyTheme` reports nothing unset. *Mutation: drop the mapping row.*
- **#1002-1.** Frontend test: inside `onEngine`, `window.contentItem()->childItems().isEmpty()` (the backend has
  not added its column yet); a throwing `onEngine` ends `run` after teardown with the exception, and no column was
  added to the content item; with `bundleTheme == Ignore` a bundle theme leaves `Theme.accent` at what `onEngine`
  set and reports once; with `Apply`, a hook that changed `Theme.accent` is reported once when the open replaces it.
  *Mutation: call the hook after the backend; the empty-content assertion fails. A restyle check alone would pass
  that mutation, since `Context` resolves components lazily (`context.cpp:108-126`).*
- **#1003-3.** `identityKey` of equal records with fields in another order is equal; `identityOf(id, params)` equals
  `identityKey` of the mounted handle's identity; `catalogTitle` picks `de` for `de-AT`, the fallback for a missing
  key and the id for no title. *Mutation: return `textOf` instead of the canonical form.*
- **#1002-11.** A plain `std::optional<int>` member with `optionsAction` emits the three annotations and reads as a
  `Choice` field; a `Choice` member with `optionsAction` in its meta throws `ConflictingOptionsError`; `optionsAction`
  without `labelField` throws `IncompleteOptionsError`. *Mutation: skip the emission; the first case fails.*
- **#1002-12.** `concatLists([{"k":0}], q)` evaluates to both lists in order; a `null` argument is `null`; a string
  argument is a reported mismatch; the static type of two `list<int>` is `list<int>`; the author helper emits
  `{"fn":"concatLists"}`. *Mutation: alias it to `concat`.*
- **#1002-4.** Qt: a Decimal field with unit `kg` draws `kg` inside its `TextField` and no sibling text; the terminal
  draws `kg` at the input's right edge; `RecordingBackend` logs `;unit=kg;decimals=2`; a Quantity with alternatives
  keeps its `unit_<path>` select. *Mutation: restore `unitSuffix`.*
- **#1002-6.** A standalone `textInput`, `select` and `dateTimeInput` with `showLabel: true` each show the caption
  text above the control on both backends (Qt: a visible item with the caption's text in the field; terminal: the row
  above the control); this is the test that fails without the feature. Without `showLabel`, none. Guard (passes
  today, must keep passing): inside a form the input's `labelShown` is false and the cell's `label_<path>` is the only
  caption. A non-bool `showLabel` is refused at load (corpus case). *Mutation: ignore `shown`.*
- **#1002-7b.** Rows toned `ok` and `err` draw `Theme.successBg` and `errorBg` behind their cells (Qt) and the tone
  styles (terminal); changing a row's field re-tones it; `"purple"` is reported once; a toned `Row` outside a table
  throws. *Mutation: never call `setRowTone`.*
- **#1003-4.** Two `Backend`s on two items of one window over one engine, one `Components` with a `textInput`
  restyle, one `Session`: two documents mount, both inputs are the restyle, a mutation on one screen refreshes a
  query the other shows, destroying the first mount leaves the second's input editable, and each host's diagnostics
  hold only its screen's. *Control: the same test with the restyle mapped after the first `Backend` shows the first
  mount unrestyled, as the spec says.*
- **#1003-2 head.** A document with `subtitle`, two `status` badges (one hidden) and two `actions` (one disabled):
  `head()` shows the subtitle following state, one badge, both actions with `enabled` false on the second; `run`
  of the first sets state; `run` of the disabled and of an unknown id answer `false` and report; the `Workspace` head
  row's `head_action_save` click runs it; a document without head parts leaves the row hidden; a duplicate action id
  is refused (corpus). *Mutation: return null from `head()`.*
- **#1003-5 footer.** On `RecordingBackend` a dialog's actions are children of its footer, not of its content; on Qt
  the buttons are under `MorphDialog.footer`; Enter still runs `default`; an `ActionBar` outside an overlay throws; a
  dialog restyle without `footer` is refused. *Mutation: mount the bar in place.*
- **#1002-3.** A form whose ObjectArray field names a host component: the component receives the rows as JSON, a
  `change` with two rows makes the form's `body` hold two rows and marks the form dirty, a `change` that does not
  decode is reported and changes nothing, submit sends the component's rows; on the terminal the field's row panels
  show. Load refuses a component without `value`. *Mutation: skip the override.*
- **#1002-13.** A download of 3 bytes of base64 reaches `saveFile` with the decoded bytes, name and type; one over the
  cap never reaches the host and is reported; an upload into bytes runs the mutation with the base64 and `nameAs`;
  a host answering more than `maxBytes` is refused; `Session` with a cap of `kMaxInlineFileBytes + 1` throws and one of
  `kMaxInlineFileBytes` does not; a field component `change` over the cap into a base64 member is refused, and the
  same payload into a plain `json` field is accepted. *Mutation: skip the cap check.*
- **#1003-1.** The existing `Workspace` tests pass unchanged. New model tests with a counting `mountPage`: identity
  focus, singleton, `reuseEmpty`, a guard veto stopping `closeAll`, `forceCloseAll`, record and resume; an Effect over
  `tabs()` runs once per title change and once per open; `unmountPage` is called after the handle is destroyed.
  *Mutation: keep `Workspace`'s logic and leave the model a stub; the model tests fail.*
- **#1002-10b.** A theme with `font.family: ["DejaVu Sans"]` (shipped in the CI image) sets `Theme.bodyFont.family`
  and changes `unitWidth` to that font's average width; without it `unitWidth` is the platform font's; an unknown
  family is reported. *Mutation: bind `metrics.font` to the platform font always.*
- **#1002-2.** The moved source and shell tests pass from `tests/client/`; a custom `SignInMapper` reading
  `{user, jwt}` signs in; an empty principal is reported; an `ApplicationHost` passed to a hand-built
  `DocumentScreenHost` runs a document's `signIn` and `upload` (control: the same host without it reports both as not
  supported); `check_install_export.sh` finds `morph::client` in an
  install. *Control: the install check fails on an install built without the component.*
- **#1002-10c.** A bundle icon set naming `tone.ok` makes an ok banner draw that SVG's URL; a node icon outside a
  declared set is reported once; `build` refuses an index naming a missing file; on the terminal `tone.ok` is `✓`.
  *Mutation: never read `icons`.*

## 18. Delivery

One branch (`feat/ui-host-integration`), one PR, one commit per item, in four slices, cheap first. Slice 4 is
droppable from its end: each of its commits stands alone except 17 (needs 16) and 19's field-component cap (needs 18).

| # | Item | Slice |
|---|---|---|
| 1 | #1002-9 busy label | 1 defects |
| 2 | #1002-5 one required marker | 1 |
| 3 | #1002-14 `result().at` rejected at compile time | 1 |
| 4 | #1002-7a `tableRow` part restyle | 2 cheap |
| 5 | #1003-6 `textArea` restyle | 2 |
| 6 | #1002-10a tone variants on `Theme` | 2 |
| 7 | #1002-1 `onEngine`, `bundleTheme` | 2 |
| 8 | #1003-3 identity key, catalog title | 2 |
| 9 | #1002-11 options in `FieldMeta` | 3 contained |
| 10 | #1002-12 `concatLists` | 3 |
| 11 | #1002-4 unit on the input | 3 |
| 12 | #1002-6 caption on standalone inputs | 3 |
| 13 | #1002-7b `rowTone` | 3 |
| 14 | #1003-4 several mounts: spec and test | 3 |
| 15 | #1002-8 `scrollBar`, `splitterHandle` | 4 large, droppable |
| 16 | #1003-2 screen head parts (introduces `actionButton` and its lowering) | 4 |
| 17 | #1003-5 dialog and drawer actions | 4 |
| 18 | #1002-3 field components | 4 |
| 19 | #1002-13 inline file bytes | 4 |
| 20 | #1003-1 `WorkspaceModel` | 4 |
| 21 | #1002-10b `font.family` | 4 |
| 22 | #1002-2 `morph::client` | 4 |
| 23 | #1002-10c icon set | 4, last |

The coordinator's order holds. Commits 4 and 6 precede 13, which draws the tone on the row background (4) with the
tone colours (6). #1003-2 and #1003-5 are two commits because they are two changes over one lowering. #1002-7 and
#1002-10 are split as the issue splits them. Commit 19 edits the example host that commit 22 then moves.

**Before every push** (each slice ends with one), every build and test wrapped in the shared build lock, `-j10`:

1. GCC and clang builds, each with `MORPH_BUILD_QT=ON MORPH_BUILD_QT_QUICK=ON MORPH_BUILD_TUI=ON MORPH_BUILD_NET=ON
   MORPH_BUILD_EXAMPLES=ON MORPH_BUILD_TESTS=ON`, warnings as errors.
2. The full `ctest --output-on-failure` in both builds (it includes the `qt_quick` label, offscreen, and qmllint over
   MorphUi).
3. clang-tidy on the changed lines, headers through a translation unit that includes them (`clang-tidy-diff -p1`
   against the clang build's compile database).
4. `git clang-format --diff master` clean, QML formatted as the module's lint target requires.
5. Doxygen: `cmake -S . -B build-doc -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF
   -DMORPH_BUILD_EXAMPLES=OFF && cmake --build build-doc --target doc` (WARN_AS_ERROR): every new public symbol has
   complete `@param`/`@tparam`/`@return`.
6. Header self-containment: `cmake --build <build> --target all_verify_interface_header_sets`.
7. `git diff master -- include/morph | grep -nE '^\+.*\b(slots|signals|emit|foreach)\b'` finds nothing.
8. Slice 4 also: `scripts/check_install_export.sh` (commit 22) and the corpus tests for every new load-time rule.

## 19. Traceability

| Item | Section | Commit |
|---|---|---|
| #1002-1 engine hook | 2.1 | 7 |
| #1002-2 client component | 16 | 22 |
| #1002-3 field → host component | 9 | 18 |
| #1002-4 unit on the input | 4.3 | 11 |
| #1002-5 required marker | 4.1 | 2 |
| #1002-6 standalone caption | 4.2 | 12 |
| #1002-7 table row restyle / `rowTone` | 3, 5 | 4, 13 |
| #1002-8 scroll bar, splitter handle | 3 | 15 |
| #1002-9 busy | 6 | 1 |
| #1002-10 tone variants / font / icons | 7.1, 7.2, 7.3 | 6, 21, 23 |
| #1002-11 options metadata | 11 | 9 |
| #1002-12 list concatenation | 12 | 10 |
| #1002-13 file bytes | 10 | 19 |
| #1002-14 `result().at` | 13 | 3 |
| #1003-1 workspace model | 15 | 20 |
| #1003-2 head parts | 8.1 | 16 |
| #1003-3 public helpers | 14 | 8 |
| #1003-4 several mounts | 2.2 | 14 |
| #1003-5 dialog footer | 8.2 | 17 |
| #1003-6 text area restyle | 3 | 5 |

## 20. Out of scope

- **#996:** a form's `submitLabel`, `title`, `showStatus`, field `layout` and `columns`; min/max sizes; alignment; a
  table's empty slot; the navigator's list look; a tab bar that hides with one tab. §4.2 leaves the cell as the place a
  field layout changes.
- **A document field overlay for options** (#1002-11's document side) and `optionsDependsOn` through `FieldMeta`.
- **Generic named slots** that mount arbitrary document subtrees into host chrome; the parts are typed (§8).
- **`unit` on a document `textInput`.** The input takes a unit from the forms engine; a document key for it can follow.
- **A `Frontend` over an application-owned `QGuiApplication`, engine and window.** Such an application builds
  `qt_quick::Backend` itself (§2.2).
- **Subtree overrides of a form field** (a document subtree as a field's control) and a grid widget built into the
  forms engine; §9 reaches the kit's grid through a host component.
- **The terminal's theme palette mapping**, and with it tone variants and fonts on the terminal.
- **Streaming inline bytes.** Above the cap, the file store's chunked transfer is the path.
