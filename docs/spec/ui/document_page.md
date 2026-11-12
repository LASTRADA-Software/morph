# Document page, regions and locale scope

A screen can show a fixed-size sheet of paper: a page of physical size, laid out at that size and scaled down to fit a
narrow window, with its letterhead and footer pinned to its margins, a window at a stated offset, and its figures
and words written in the locale of the person the sheet is for, not the locale of the person reading it. Three words
say it: `page`, `pageRegion` and `localeScope`. All three are mechanism: a CRM's quote, a bank's statement and an
invoice are the same page, and none of the words knows any of them.

Read this before changing what a page lays out, what a frontend does with one, or what a scope's locale reaches.
Rendering a page to a file or a printer is not here: a sheet is drawn on the screen and nowhere else
([Out of scope](#out-of-scope)).

| Name | Is |
|---|---|
| `ui::Mm` | A length in millimetres, held as an exact integer count of micrometres. |
| `ui::PageMargins` | `{top, right, bottom, left}` in `Mm`. |
| `ui::Page` / `ui::page(PageGeometry, children)` | The view-tree node of a sheet; `PageGeometry{width, height, margins}`. |
| `ui::PageRegion` / `ui::pageRegion(RegionGeometry, children)` | A region of a sheet; `RegionGeometry{anchor, x, y, width, height}`. |
| `ui::PageAnchor` | `Top`, `Bottom` or `Offset`. |
| `ui::layoutPage(geometry, regions, availableWidth)` | The pure function both frontends and the recording backend lay a page out with; answers a `PageLayout`. |
| `ui::author::page(PageOptions, Nodes)`, `pageRegion(RegionOptions, Nodes)`, `localeScope(AnyHandle, Nodes)` | The server-side builders; the document kinds `page`, `pageRegion` and `localeScope`. |
| `ui::PageWidget`, `ui::PageRegionWidget` | The backend widgets ([Backend contract](#backend-contract)). |

## Contents

- [What the existing vocabulary cannot say](#what-the-existing-vocabulary-cannot-say)
- [Lengths](#lengths)
- [The page](#the-page)
- [Regions](#regions)
- [Scaling](#scaling)
- [Density per frontend](#density-per-frontend)
- [The document form and the author API](#the-document-form-and-the-author-api)
- [The locale scope](#the-locale-scope)
- [Accessibility](#accessibility)
- [Backend contract](#backend-contract)
- [Recording backend](#recording-backend)
- [Tests](#tests)
- [Alternatives considered](#alternatives-considered)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)

## What the existing vocabulary cannot say

Four things a sheet needs have no word in `ui/1`, each for a reason stated by the contract:

- **A physical size.** Every extent is in backend units: a character cell on the terminal, the theme's
  `unitWidth` on Qt Quick, which is the average width of the theme font's characters and so changes with the font. A
  `fixed(210)` is not 210 mm in either.
- **A place on a sheet.** A stack places children along its axis; a grid by tracks. Nothing places a node at a distance
  from an edge of its container, which is what a standard form's address window is (DIN 5008 form B: 45 mm high and
  85 mm wide, 20 mm from the left edge and 45 mm from the top).
- **A subtree scaled as one.** `LayoutHints` sizes a node; nothing transforms the layout of everything inside one.
  Shrinking a sheet to a phone's width needs the sheet laid out at its own size first, so that its line breaks, its
  table column widths and its figures' positions are those of the sheet, and every part then reduced by one factor.
- **A locale for a subtree.** The formatting facts belong to the session; a screen can switch them for everyone
  (`setLocale`), not for the words of one sheet.

A footer held at the bottom of a sheet is the one thing a stack can already do approximately: a column of fixed height
with a `stretch` spacer before the footer. It is still a region here, because the page has no content-derived height
and a region anchored at the bottom takes its place from the margin box, whatever its own height is
([Regions](#regions)).

## Lengths

`ui::Mm` is a length in millimetres. It holds an exact `std::int32_t` count of micrometres and converts implicitly from
`double` (and `int`), rounding to the nearest micrometre, so `.width = 210` and `.y = 98.5` are written as the
standards write them. Reasoning: a page's geometry is compared in tests and travels in a document; a floating-point
length would make `margins sum to the width` and "the footer's bottom edge equals the margin box's" depend on rounding,
while a micrometre is finer than anything a screen or a printer resolves and an integer count compares exactly. The
document form is a number in millimetres with at most three decimals (a finer number is rounded the same way); the
recording backend writes it in the shortest decimal form (`98.5`, `210`).

A length of a page is finite and at least 0. The page's width and height are each 10 mm to 5000 mm: below 10 mm
nothing readable fits, above 5000 mm a sheet is not a sheet and the micrometre count would approach `int32` range.

## The page

`ui::page(PageGeometry{width, height, margins}, children)` is a sheet `width` × `height` millimetres. `margins` are the
distances from each edge of the sheet to its **margin box**: the rectangle regions are anchored to. They convert from a
single `Mm` for every side. The mount refuses (`std::logic_error`, naming the node's test id, as every layout refusal
does) a page whose margin box is empty (left plus right margin not below the width, top plus bottom not below the
height), and a document with one is refused at load.

A page's children are **regions** and nothing else: the mount refuses a child that is not a `pageRegion`, and a
`pageRegion` outside a page. The page has no flow of its own. A sheet whose body is a long table puts the table in a
region and that region has a place; where the body's top is, and where the footer's top must stay clear of it, are
facts of the form (a standard names them in millimetres), so they are stated, not derived. Reasoning: derived
placement (a body that flows below the letterhead and above the footer) needs the page to measure its regions' content
before placing any of them, an order of events the backends do not otherwise have and which makes the page's geometry
depend on the fonts installed; stated placement is pure arithmetic on numbers the author wrote, the same on every
frontend, and testable with no layout at all.

The page's own `Common` is honoured except `layout`: a page sizes itself, so the mount refuses a `width`, `height` or
bound in its `LayoutHints`. Its `alignSelf` is allowed and places the displayed sheet across its container's axis, which
is how a sheet is centred in a column; a container's `Stretch` alignment does not stretch a page, which sits at the
start. A page is a fixed object in its container: its footprint in the container's layout is its **displayed size**,
the sheet's size times the scale ([Scaling](#scaling)).

The sheet draws on a built-in surface, `paper`: a white background and black text, accent and muted roles that hold
their contrast on white, in every theme including a dark one. Reasoning: a sheet is paper, and what is printed on it
must not turn light-on-light when the application is in a dark theme; a document the person is about to print or compare
with a printout must look the same in every theme. The surface does not nest its colours into the controls inside it
beyond what every surface does ([`theme.md`](theme.md#surfaces)). The page also draws a hairline border and the
card elevation, so its edge reads against the window, and clips its content to the sheet.

## Regions

`ui::pageRegion(RegionGeometry{anchor, x, y, width, height}, children)` is a rectangle of the sheet that holds its
children as a vertical stack (the contract's stack rules, spacing step 0, `Stretch` alignment: each child is as wide as
the region). The region's width is always definite, so text in it wraps at that width, and its height is definite
whenever the region gives one, so a `stretch` child of a region of stated height shares the height left, which is how
a row's contents are pushed apart or a total is held at the bottom of a box.

All coordinates are millimetres in the sheet's own, unscaled coordinate system, with the origin at the sheet's top-left
corner and y downwards.

| `anchor` | The region's rectangle |
|---|---|
| `Top` | x from the left margin to the right margin edge (the margin box's width) unless `width` is given, which then starts at the left margin edge; its top edge at the margin box's top edge. |
| `Bottom` | As `Top` horizontally; its **bottom** edge at the margin box's bottom edge, so a footer of any height keeps its bottom where the margin says. |
| `Offset` | Its top-left corner at `(x, y)` from the **sheet's** top-left corner, not the margin box's. |

- `width` and `height` are optional. An absent `width` is the margin box's width for `Top` and `Bottom`, and the
  distance from `x` to the margin box's right edge (at least 0) for `Offset`. An absent `height` is the content's height.
  A given `height` is the region's extent, and the region clips its content to it, so that a window of stated size
  never bleeds past its frame.
- `x` and `y` mean something only for `Offset`: the mount refuses a nonzero `x` or `y` on `Top` or `Bottom`
  ("a pinned region has no offset"), rather than ignoring a number that the author expected to move it. Reasoning:
  there are two ways to read an offset on a pinned region (from the sheet, from the margin box), and refusing both
  leaves no silent third.
- **Why offsets are from the sheet's edges.** The figures standards give (a window 20 mm from the left and 45 mm from
  the top of the sheet) are distances from the sheet's edge. Offsetting from the margin box would make every such
  figure a subtraction the author does by hand, and a changed margin would silently move the window.
- The mount refuses (`std::logic_error`) a negative `x`, `y`, `width` or `height`, a zero `width` or `height`, and an
  `Offset` or pinned region whose given rectangle leaves the sheet (`x + width` past the sheet's width, `y + height`
  past its height; a pinned region's rectangle is checked by the same rule once placed). A region whose content turns
  out taller than its stated height is clipped, not refused: the content is data, the rectangle is the author's.
  A region with no stated height whose content runs below the sheet's bottom edge is clipped at the sheet; a page does
  not continue onto a next one ([Out of scope](#out-of-scope)).
- A region's `Common` is honoured except `layout`, which the mount refuses wholesale: the geometry names the size.
- **Overlap.** Regions are drawn in node order, later over earlier, and hit-tested topmost first. Overlap is not
  reported: a watermark and a stamp over text are overlapping regions on purpose. The sheet's background is below all
  regions.
- **Content-sized bottom anchors.** The reason `Bottom` exists beside `Offset` is that a region whose height is its
  content's cannot be placed from its top without knowing that height; a `Bottom` region is placed from its bottom edge,
  which is a number the author wrote. A footer's content may therefore grow (a longer line wrapping to two) upward,
  not off the sheet.

### The layout function

`ui::layoutPage(PageGeometry const&, std::span<RegionSpec const>, Mm availableWidth)` is a pure function of numbers.
`RegionSpec` is a region's `RegionGeometry` and its content height (the height of its content at its width, or
`Mm{0}` where the caller has none); it answers a `PageLayout`:

| Member | Is |
|---|---|
| `scale` | `min(1, availableWidth / width)` as a `double`, 1 for an unlimited width; the sheet's width is never 0, so it is defined. |
| `displayedWidth`, `displayedHeight` | The sheet's size times `scale`, in `Mm`, rounded to the micrometre. |
| `regions` | One `Mm` rectangle `{x, y, width, height}` per region, in the sheet's unscaled coordinates, in node order. |

The Qt Quick backend, the terminal backend and the recording backend all take a page's scale and its regions'
rectangles from this function, so the three cannot disagree about where the footer is; a frontend adds only its density
([Density per frontend](#density-per-frontend)) and its drawing. The function is in `include/morph/ui/page_layout.hpp`.

## Scaling

A page is laid out at its own size, then displayed scaled by `scale = min(1, available / width)`, where `available` is
the width its container offers it, in millimetres of the frontend's density.

- **Down, never up.** A window wider than the sheet shows the sheet at its own size, with the container's alignment
  (usually centred) on either side; enlarging a sheet would blur its text and make it larger than the paper it stands
  for.
- **Uniform.** One factor scales both axes, with the origin at the sheet's top-left corner. A page 210 × 297 mm offered
  105 mm is displayed 105 × 148.5 mm: its footprint in the container's layout is the displayed size, so the container
  scrolls by the scaled height.
- **By available width, not by width class.** The compact class is where it happens in practice, but a medium window
  narrower than a sheet scales it too, and a compact window of a very wide sheet scales it by more; a rule keyed to the
  class would leave a sheet wider than its window. The width a container offers is infinite inside a horizontal
  `scroll`, so a sheet there is shown at its own size and scrolled.
- **No lower bound.** A floor would make the sheet overflow its window sideways, which on a phone is worse than small
  type; a person who needs the sheet larger turns the window. The page does not claim to be readable at every scale.
- **Everything inside scales**: text, borders, controls. Pointer input is mapped through the transform, and keyboard
  focus and its ring are the focused widget's, scaled with it. The layout inside the sheet does not change with the
  scale, so a line breaks at the same word at every width.
- **The terminal does not scale** ([Density per frontend](#density-per-frontend)): a cell is the smallest thing it
  draws, and halving a page's cells would drop characters.

## Density per frontend

Millimetres reach a frontend as a density, a number of the frontend's own units per millimetre. The page and its regions'
rectangles are placed in millimetres, so the density is the only place a frontend's unit enters.

**Qt Quick** maps one millimetre to `logicalDotsPerInch / 25.4` pixels, where `logicalDotsPerInch` is the DPI of the
screen the window is on, read per axis, and changes with the window's screen. At 96 DPI an A4 sheet is 794 × 1123 px, the
size a web page calls it. The theme's font sizes are in pixels, so a 13 px text on a 96 DPI screen is 3.4 mm high, a
printed 9.75 pt: the type the sheet carries is the theme's, and the sheet's size is the paper's. The units a region's
children use stay theme units (`Theme.unitWidth`) as everywhere; only the page's and the regions' own rectangles are
millimetres. The page is an `Item` of its displayed size holding an inner `Item` of the sheet's pixel size with
`scale` set to the layout's scale and `transformOrigin: Item.TopLeft`, so regions, text and hit-testing share one
transform; regions are children of the inner item in node order.

**Terminal**: 10 columns per 25.4 mm across and 6 rows per 25.4 mm down, the typewriter pitch (10 characters and
6 lines to the inch). A cell is about twice as tall as wide, and a page of typed text at this pitch is the proportion a
page of text has: A4 is 83 columns by 70 rows (82.68 and 70.16, rounded to the nearest), an 85 mm × 45 mm window 33 columns by
10 rows. A region's rectangle is converted by rounding **edges**, not sizes: `left = round(x · 10 / 25.4)`, `right =
round((x + width) · 10 / 25.4)`, and the width is `right − left`; rows the same with 6. Rounding each edge independently
means two regions that touch in millimetres touch in cells, and a rounded size can never drift from its neighbour's
edge.

The terminal does not scale (`scale` is 1 whatever the viewport). A viewport narrower than the sheet shows its left
part. The page takes the keyboard focus itself while nothing inside can (as a read-only `scroll` does) and pans
horizontally: Left and Right move the view a cell, Home and End to either end. The other keys, and the wheel, are left
to the page's surroundings, so Up, Down, PageUp and PageDown move the enclosing `scroll` vertically. Reasoning: the terminal's job is to render the screen usably, not to reproduce the paper, and a page of 83 columns
fits the 80-to-120 column terminals people use within a few cells; panning is the minimum that makes the rest
reachable.

## The document form and the author API

The document gains three node kinds. `page` and `pageRegion` lower to `ui::Page` and `ui::PageRegion`; `localeScope`
lowers to a holder column ([The locale scope](#the-locale-scope)).

```json
{"kind": "page", "width": 210, "height": 297,
 "margins": {"top": 10, "right": 20, "bottom": 15, "left": 20},
 "a11y": {"name": {"expr": …}},
 "children": [
   {"kind": "pageRegion", "anchor": "offset", "x": 20, "y": 45, "width": 85, "height": 45, "children": […]},
   {"kind": "pageRegion", "anchor": "bottom", "children": […]}]}
```

| Key | Kinds | Shape |
|---|---|---|
| `width`, `height` | `page` | Literal numbers in millimetres, required. |
| `margins` | `page` | A literal number (every side), or a record of `top`, `right`, `bottom`, `left` (a side left out is 0); default 0. |
| `anchor` | `pageRegion` | The literal `"top"`, `"bottom"` or `"offset"`, required. |
| `x`, `y`, `width`, `height` | `pageRegion` | Literal numbers in millimetres; `x`, `y` default 0 and only `"offset"` accepts them; `width` and `height` optional. |
| `locale` | `localeScope` | An expression, required: the BCP 47 tag the scope formats in. |

Geometry is literal, not computed: it is set once, as a sizing is. A computed length would make the sheet's layout
depend on data and its scale a reactive value. The loader refuses, with the JSON pointer of the offending key, the
cases the mount refuses (a non-region child of a `page`, a `pageRegion` anywhere else, an empty margin box, a negative
or out-of-range length, an `x` or `y` on a pinned region, a region leaving the sheet, a `layout` key on either node) and a
`localeScope` without a `locale`. A `page` takes a list of `pageRegion` nodes, a `pageRegion` and a `localeScope` the node list of a `column`.

`ui::author::page(PageOptions{width, height, margins, a11y, …}, Nodes)`, `ui::author::pageRegion(RegionOptions{anchor, x,
y, width, height}, Nodes)` and `ui::author::localeScope(AnyHandle locale, Nodes)` write the kinds. `PageOptions` and
`RegionOptions` hold `Mm` (so `.width = 210` and `.x = 20` compile), and take the common options a node of the author
API takes (`id`, `testId`, `visible`, `a11y`, `surface`). `RegionOptions::anchor` is a `PageAnchor`. Children come first
and options second, as everywhere in the author API. `localeScope` takes no options: a scope is not a widget.
`localeScope`'s `locale` is a handle, so a scope reads the tag from a query (the customer's locale), a `let`, or a
literal (`literal("de")`).

`checkAccessibility` requires an `a11y.name` on a `page` ([Accessibility](#accessibility)).

## The locale scope

`ui::author::localeScope(locale, children)` makes **every formatting and every `t` evaluated inside it use the scope's
locale**; the session's locale is untouched, outside the scope and inside it.

### What the scope sets

The scope's `FormatContext` is the session's with these members taken from the scope's locale tag `L`:

- `locale` is `L`: `t` resolves `(key, L)` through the same `TranslationProvider`, with the provider's own fallback
  (the tag's prefixes, then the catalogue's default locale), and plural selection follows `L`'s language.
- `numeric` and `currency` are those of `localeFacts(L)` ([`locale_table.md`](locale_table.md)), so `number`, `decimal`,
  `quantity` and `money` write `L`'s separators, sign and currency placement.
- `datePattern` and `dateTimePattern`: **the application's declared pattern wins, else `L`'s row's**, which is the rule the
  session applies ([`locale_table.md`](locale_table.md#who-installs-the-facts)). An application that regulates the form
  of a date (lims's `dd-MMM-yyyy`) keeps that form in a sheet in another locale, because one application writing a date
  two ways is the thing the rule exists to prevent, and a person comparing the sheet with the screens beside it would
  otherwise read two forms of one date. An application that declares none gets `L`'s: a `de` scope under an `en` session
  writes `24.10.2026`.
- everything else (`units`, the display zone's offset, a `DateFormatter` the host installed) is the session's. The zone is
  the reader's: a timestamp on a sheet for a customer abroad is shown in the clerk's zone, which is what the clerk
  can check; a document that needs another zone is a different mechanism.

A tag with no row in the locale table writes with `en`'s facts and is reported, once per tag for the session's lifetime,
through the same channel and the same once-set as the session's own switch ("no formatting facts for locale `pl`;
numbers, money and dates use en's"). `L` itself is kept as `locale`, so catalogue lookup and plurals still follow it.

### What the `locale` expression may be

It is evaluated in the scope that encloses the `localeScope`, reactively. A string is a tag. `null` and `""` mean "not
known yet" (a query still loading, a customer with no stored locale): the scope then uses the session's locale, without a
report, and switches when a tag arrives. A value that is neither a string nor `null` is a reported mismatch at the
`locale` path, and the scope uses the session's. Reasoning: a sheet built from a loading query must not report a
problem for the moment before its data arrives, and showing it in the reader's locale for that moment is the same state
the rest of the sheet is in.

### Where it reaches

Evaluation inside the scope reads the scope's context wherever it reads the environment's `formatting()`: the display
functions (`number`, `decimal`, `quantity`, `money`, `date`, `dateTime`, `dateRange`), `t` and its argument formatting,
`format`, and `parseDecimal`. A value a node converts to text (a bare number bound to a `text`, a `keyValue` item, a table
cell) is converted through the scope's context as well, since the node is inside the scope.

Entry is the exception: a text input, a date input, and a number input's decimals inside a scope read and write with
the session's facts. A person types in their own locale; what an input shows is what they will type back, and a number
that is shown as `1.234,50` and parsed as `1.23450` is data loss. A sheet is display, and these are rare in one.

`{"env": "locale"}` and `session.locale` stay the session's: they answer who is reading, not what language the sheet is
in, and a screen decides things (which catalogue words to offer the clerk) from the former.

### Nesting and reactivity

Scopes nest, and the innermost wins; its context is built from the *session's* with its own tag, not from the enclosing
scope's, so an inner scope inherits nothing from an outer one but the session's other members. The scope's context is a
reactive value of the session's formatting facts and the `locale` expression, so a `setLocale` of the session (a
change of the declared patterns, of the catalogue) or a change of the tag re-evaluates exactly the properties inside the
scope that read a formatting.

### How it is lowered

The scope is not a widget. The interpreter lowers `localeScope` to a holder column (the column it inserts around a list
of nodes: spacing step 0, `Stretch` alignment, `holder` set, so a child's `bleed` reaches the scope's parent
as before) and mounts the children in a **child
scope** (`RtScope`) that carries the context. `ScopeEnv::formatting()` answers the innermost enclosing scope's context
and otherwise the session's; node-level conversions use the same lookup instead of the session's.

#### Scope and page

A `localeScope` is not a region, so it is refused as a child of a `page`. It goes around a `page`, which makes every
region in the sheet format in the scope's locale (the invoice's case: `localeScope(customerLocale, {page(…)})`), or
inside a region, around part of its content. The holder column the scope lowers to holds the page, which sits in it as in
any column.

## Accessibility

- **The page is a document region.** Its accessible role is `document` (`Accessible.Document` on Qt Quick) and the
  author names it: `checkAccessibility` requires an `a11y.name` on a `page`, as it does on a
  `table`, because an unnamed document in a screen reader's landmark list reads as "document". The terminal
  has no accessibility tree and records the name for the golden only.
- **A region is a grouping**, named only when the author names it (`a11y.name`): a letterhead and a footer have no
  heading words of their own. It is an Accessible grouping on Qt Quick; a region is not a landmark.
- **Reading order is node order.** The regions are children of the page in the order the document lists them, which is the
  order of the accessibility tree, of the keyboard focus (Tab) order, and of the terminal's focus cycle; the geometry
  never reorders them. An author lists the regions in the order the sheet is read: letterhead, address, the body, the
  footer. A visual position (a footer at the bottom) says nothing about reading order, and a region listed last that sits
  at the top is read last.
- **Scaling keeps it operable.** A scaled page maps pointer input through the transform; its keyboard focus follows the
  focus of the widget inside.
- **The locale scope sets the language of its content.** Qt Quick has no per-item language property on its accessible
  interface, so this is not exposed there; the scope's effect is the words themselves.
- The sheet's contrast is the `paper` surface's: black on white.

## Backend contract

Two widgets, both made by `IViewBackend`:

| Interface | Factory | Setters |
|---|---|---|
| `PageWidget` (a `ContainerWidget`) | `createPage(parent, PageGeometry)`: the geometry is a factory argument, as a stack's axis is | none. The page reads the width its container gives it from the container's layout; the contract carries no width. |
| `PageRegionWidget` (a `ContainerWidget`) | `createPageRegion(parent, RegionGeometry)` | none: the geometry is a factory argument too. |

A region's parent is a `PageWidget`; `createPageRegion` with another parent type is a contract violation the mount cannot
produce. Both are visible, enabled, not draggable, without name or role, as every new widget is
([`backend_contract.md`](backend_contract.md#widgets)), and `PageWidget` has the `paper` surface by default.

A frontend's page holds its regions as children, lays them out from `layoutPage` and its density, and sizes itself to the
displayed size. A `PageWidget` implementation never reports its scale to the mount; the scale is a function of the width it
is given and the geometry, which the mount does not need.

## Recording backend

The recording backend has no pixels, so a page is observed as the numbers `layoutPage` produces.

- **Available width.** `setPageAvailableWidth(Mm)` sets the width every page of the backend is offered; the default is
  unlimited, so the scale is 1. A change re-lays every page out, logging each changed property.
- **Content height.** A region without a stated height has the content height `0 mm` until `setRegionContentHeight(id, Mm)`
  gives one; a test of a content-sized `Bottom` region sets it.
- **Properties**, recorded by `Page` and `PageRegion`, as every kind records its own:

| Kind | Property | Value |
|---|---|---|
| `Page` | `size` | `210x297`, width and height in the shortest decimal (`148.5`) |
| `Page` | `margins` | `top,right,bottom,left` |
| `Page` | `scale` | the scale in its shortest round-trip decimal (`0.5`, `1`) |
| `Page` | `displayed` | the displayed size, `105x148.5` |
| `PageRegion` | `anchor` | `Top`, `Bottom` or `Offset` |
| `PageRegion` | `rect` | `x,y,width,height` in the sheet's own millimetres, as `layoutPage` resolves them |

- **Log lines**: `create Page#1 in root`, `create PageRegion#2 in Page#1`, and `set Page#1 scale=0.5`,
  `set Page#1 displayed=105x148.5` and `set PageRegion#2 rect=…` on each change of the available width or a content
  height. Factory parameters (`size`, `margins`, `anchor`) are recorded as properties without a log line, as other factory
  parameters are.
- **Golden dump**: `Page#1 a11yName=… a11yRole=document displayed=… margins=… scale=… size=… surface=paper` and, indented,
  `PageRegion#2 anchor=Offset rect=20,45,85,45`.
- **Scope**: there is nothing to observe of a `localeScope` but the text of the widgets inside it, and a holder `Column`.
  A test reads a `Text`'s `text` property for `€1,234.50` beside `1.234,50 €` and the log's `create Column#n`.

## Tests

`tests/test_ui_document_page.cpp` runs over the recording backend and the interpreter. Each case has the mutation that
must fail it:

| Case | Asserts | Mutation that fails it |
|---|---|---|
| A page at a narrower width | A4 offered 105 mm records `scale=0.5` and `displayed=105x148.5`; offered 300 mm records `scale=1` and `displayed=210x297` (never up) | `scale` computed without the `min(1, …)`; scale applied to the width only |
| A footer at the bottom of a short page | A `Bottom` region with a 12 mm content height on A4 with a 15 mm bottom margin records `rect` with `y = 297 − 15 − 12 = 270`, whatever its content, and the same footer given 30 mm of content records `y = 252`; a `Top` region records `y` = the top margin | the `Bottom` anchor placed from the top of the margin box; the margin ignored |
| An address window at its offset | An `Offset` region `x = 20`, `y = 45`, `width = 85`, `height = 45` on a page whose left margin is 25 records `rect=20,45,85,45` | the offset taken from the margin box (`rect=45,…`) |
| `money` in `de` inside a scope under an `en` session | `money(1234.5, "EUR")` in a `localeScope("de")` shows `1.234,50 €` while the same expression outside shows `€1,234.50`, and the scope follows a change of its tag and a session `setLocale` | the scope not installing its context (both show `€1,234.50`); the scope replacing the session's (both `1.234,50 €`) |
| Refusals | A non-region child of a page, a pinned region with `x`, a region leaving the sheet, an empty margin box, a `layout` on a page, a `pageRegion` outside a page: each `std::logic_error` naming the test id; the document forms refused at load with the pointer | each check removed |
| The terminal | The cell rectangle of a region from `layoutPage` and the pitch: A4 is 83 × 70 cells, a window at `(20, 45, 85, 45)` mm is columns 8 to 41 and rows 11 to 21 (edge rounding), and two abutting regions share an edge | rounding sizes instead of edges |
| Entry in a scope | A text input inside a `de` scope under an `en` session reads `1,5` as text and `1.5` as the number it shows | the input reading the scope's facts |

The frontends' suites check what only they can: Qt Quick's inner item's `scale` and `width` at 96 DPI and at 144
DPI, and the terminal's page size in cells and its pan.

## Alternatives considered

- **A `custom` QML component per document.** The design forbids per-application client code: every screen is a ui/1
  document, and a client that cannot render an invoice without the invoice's QML is a client the server must ship code
  to. The page is the mechanism, and the invoice is a document in it.
- **Rendering the sheet to an image on the server.** Document rendering is outside what morph builds. A rendered
  image is also not a ui/1 document: it has no text to select, find, read aloud or test; it does not follow the theme
  or the width; and it moves the formatting of the sheet to the server, which then needs the client's fonts.
- **Millimetres in `LayoutHints`.** Adding a `Sizing::millimetres(n)` would reach the whole vocabulary and every
  container's rules; a physical length means something only inside a page, where the density is defined, and an
  extent in millimetres in a column would be a unit the terminal and the theme cannot honour.
- **A flowing page**, whose body sits below the letterhead and above the footer. Rejected for the reason in
  [The page](#the-page): it needs measurement before placement.
- **Only `Offset` regions, no `Top`/`Bottom` anchors.** A `Bottom` region needs its height before it is placed; the
  anchor places it from the edge it is pinned to, so the footer can grow without the author recomputing its top. The
  cost is one enum value.
- **Scaling by a transform in every frontend's own units** rather than millimetres and a function. It would put the page
  arithmetic in two backends and no test could reach it without pixels.
- **A locale for the whole screen only** (`setLocale` on a screen). It moves the person's whole interface, and then moves
  it back; a clerk reading a German customer's invoice would see their own buttons turn German.
- **Putting the scope on the page** (`page(…, locale)`). A letter in a locale, a table in another locale and a
  summary box in the clerk's own are different scopes; the scope is a node because the locale of a subtree is not the
  page's.
- **A scope that also governs entry.** Rejected above: it creates numbers an input shows and cannot read back.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Length type | `Mm`, an exact micrometre count | Exact comparison in tests and on the wire; finer than any device. |
| Placement | Stated geometry, not flow | No measuring before placing; the same on every frontend; the standards state millimetres. |
| Page children | Regions only | A page with its own flow is a second layout model. |
| Offset origin | The sheet's edge | The figures a form's standard gives are measured from it. |
| Pinned regions with an offset | Refused | An offset on a pinned region has two readings. |
| Scale | `min(1, available / width)`, uniform, no floor | Fits without sideways overflow; never enlarges. |
| Scaling trigger | Available width | A class threshold can leave a sheet wider than its window. |
| Terminal density | 10 columns and 6 rows per 25.4 mm, edges rounded | A page of text reads as a page; abutting regions abut. |
| Terminal scaling | None, pan | A cell is the smallest thing it draws. |
| Sheet colour | A built-in `paper` surface | Paper is white in a dark theme; text on it must stay legible. |
| Scope's date patterns | The application's declared pattern wins | One application, one date form; the same rule as the session's. |
| Scope's zone and units | The session's | The zone is the reader's. |
| Scope and entry | Entry keeps the session's facts | What an input shows must be what it parses. |
| Scope's tag unknown | The session's locale, silently, while `null` or `""` | Loading is not a problem. |
| Scope's lowering | A holder column and a child `RtScope` | The view tree needs no new kind, and the formatting lookup is the interpreter's. |
| Unknown tag | `en`'s facts, reported once | The session's rule. |

## Out of scope

- Rendering a page to a file or a printer (PDF, print files) and pagination onto several sheets: document rendering is
  domain output and is not built here. A sheet is a view on the screen; a region taller than the sheet is clipped.
- Paper sizes as names (`A4`, `Letter`): an application passes millimetres.
- Rotating, zooming or panning a page on Qt Quick, beyond what the enclosing `scroll` does.
- A display zone of its own for a scope.
- Right-to-left layout and locale-specific digit shapes beyond what `NumericLocale` already holds.
