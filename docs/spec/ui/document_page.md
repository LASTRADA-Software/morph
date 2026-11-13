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
| `ui::Page` / `ui::page(Page)` | The view-tree node of a sheet; `Page{geometry, children, common}`, with `PageGeometry{width, height, margins}` as its `geometry`. Its children are `PageRegion` nodes; the mount refuses any other. |
| `ui::PageRegion` / `ui::pageRegion(RegionGeometry, children)` | A region of a sheet; `RegionGeometry{anchor, x, y, width, height}`. |
| `ui::PageAnchor` | `Top`, `Bottom` or `Offset`. |
| `ui::layoutPage(geometry, regions, availableWidth)` | The pure function both frontends and the recording backend lay a page out with; answers a `PageLayout`. |
| `ui::toCells(rect, pitch)`, `ui::toCells(rect, anchor, pitch, contentRows)` | The terminal's conversion of a millimetre rectangle to cells, in `page_layout.hpp` so a test reaches it without a terminal; the second is for a region of no stated height. |
| `ui::author::page(Nodes, PageOptions)`, `pageRegion(Nodes, RegionOptions)`, `localeScope(Nodes, AnyHandle)` | The server-side builders; the document kinds `page`, `pageRegion` and `localeScope`. |
| `ui::PageWidget`, `ui::PageRegionWidget` | The backend widgets ([Backend contract](#backend-contract)). |

## Contents

- [What the existing vocabulary cannot say](#what-the-existing-vocabulary-cannot-say)
- [Lengths](#lengths)
- [The page](#the-page)
- [Regions](#regions)
- [Size request](#size-request)
- [Scaling](#scaling)
- [Density per frontend](#density-per-frontend)
- [The document form and the author API](#the-document-form-and-the-author-api)
- [The locale scope](#the-locale-scope)
- [Accessibility](#accessibility)
- [Overflow](#overflow)
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

A footer held at the bottom of a sheet is the one thing a stack can already do: a column of stated height with a
`stretch` spacer before the footer lays the footer's bottom edge on the column's (measured on Qt Quick, in the
frontend's test). That is not enough here. The column's size is in units, so a `fixed(210)` is 210 characters wide
by the theme font (measured, and not 794 px), and the footer rides on a sibling spacer, not on a margin; a region
anchored at the bottom takes its place from the margin box whatever its own height is ([Regions](#regions)).

## Lengths

`ui::Mm` is a length in millimetres. It holds an exact `std::int32_t` count of micrometres and converts implicitly from
`double` (and `int`), rounding to the nearest micrometre, so `.width = 210` and `.y = 98.5` are written as the
standards write them. Reasoning: a page's geometry is compared in tests and travels in a document; a floating-point
length would make `margins sum to the width` and "the footer's bottom edge equals the margin box's" depend on rounding,
while a micrometre is finer than anything a screen or a printer resolves and an integer count compares exactly. The
document form is an integer count of micrometres, because a document carries exact numbers and a number with a
fraction is refused (`98.5` mm is `98500`); the author API takes millimetres and writes the count. The
recording backend writes it in the shortest decimal form (`98.5`, `210`).

The conversion from `double` is implicit so a geometry reads as a standard writes it. A NaN or an infinity is refused
(`std::invalid_argument`), and so is a value whose micrometre count does not fit an `int32` (about 2147 m). A length
of a page is finite and at least 0. The page's width and height are each 10 mm to 5000 mm: below 10 mm nothing readable
fits, and above 5000 mm what is meant is not a sheet; the bound is a sanity limit, not a representation limit.

## The page

`ui::page(Page{.geometry = PageGeometry{width, height, margins}, .children = …})` is a sheet `width` × `height` millimetres. `margins` are the
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
the sheet's size times the scale ([Size request](#size-request), [Scaling](#scaling)).

The mount calls `setAccessibleRole("document")` and `setSurface("paper")` on a page whose node names no role and no
surface, so the recording backend's golden holds both; a widget has no default of either.

The sheet draws on a built-in surface, `paper` ([`theme.md`](theme.md#surfaces)): a white background and dark ink in every
theme, a dark one included, with every colour role given as a literal, so that none comes from a dark theme and lands
on white. Reasoning: a sheet is paper; what is printed on it must not turn light-on-light when the application is in a
dark theme, and a document the person compares with a printout must look the same in every theme. A theme may replace
`paper` by name, as it may any built-in surface; the built-in one is fixed, and a test runs `themeContrastProblems` on the roles it reads from `paperScheme` in `Theme.qml`. The
page also draws a hairline border and the card elevation, so its edge reads against the window, and clips its content to
the sheet. The terminal draws no surface for it, and marks the sheet's edge with a frame
([Density per frontend](#density-per-frontend)).

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
- The mount refuses (`std::logic_error`) a negative `x`, `y`, `width` or `height`, a zero `width` or `height`, an
  `Offset` region with no stated `width` whose `x` lies at or past the margin box's right edge (it would resolve to
  a width of 0, which a stated width is refused for), and an
  `Offset` or pinned region whose given rectangle leaves the sheet (`x + width` past the sheet's width, `y + height`
  past its height; a pinned region's rectangle is checked by the same rule once placed). A region whose content turns
  out taller than its stated height is clipped, not refused: the content is data, the rectangle is the author's.
  A region with no stated height whose content runs below the sheet's bottom edge is clipped at the sheet; a page does
  not continue onto a next one ([Out of scope](#out-of-scope)). Either way the clipping is shown ([Overflow](#overflow)).
- A region's `Common` is honoured except `layout`, which the mount refuses wholesale: the geometry names the size.
- **Overlap.** Regions are drawn in node order, later over earlier, and hit-tested topmost first. Overlap is not
  reported: a watermark and a stamp over text are overlapping regions on purpose. The sheet's background is below all
  regions.
- **Content-sized bottom anchors.** The reason `Bottom` exists beside `Offset` is that a region whose height is its
  content's cannot be placed from its top without knowing that height; a `Bottom` region is placed from its bottom edge,
  which is a number the author wrote. A footer's content may therefore grow (a longer line wrapping to two) upward,
  not off the sheet.

### The layout function

`ui::layoutPage(PageGeometry const&, std::span<RegionSpec const>, std::optional<Mm> availableWidth)` is a pure function of
numbers; an absent width is unlimited.
`RegionSpec` is a region's `RegionGeometry` and its content height (the height of its content at its width, or
`Mm{0}` where the caller has none); it answers a `PageLayout`:

| Member | Is |
|---|---|
| `scale` | `min(1, availableWidth / width)` as a `double`, 1 for an unlimited width; the sheet's width is never 0, so it is defined. |
| `displayedWidth`, `displayedHeight` | The sheet's size times `scale`, in `Mm`, rounded to the micrometre. |
| `regions` | One `{x, y, width, height}` `Mm` rectangle per region, in the sheet's unscaled coordinates, in node order, and an `overflow` flag: the content is taller than the stated height, or the rectangle's bottom lies below the sheet's. |

The Qt Quick backend, the terminal backend and the recording backend all take a page's scale and its regions'
rectangles from this function, so the three cannot disagree about where the footer is; a frontend adds only its density
([Density per frontend](#density-per-frontend)) and its drawing. The function is in `include/morph/ui/page_layout.hpp`.

## Size request

A page states its size to its container the way every widget does ([`backend_contract.md`](backend_contract.md#containers)),
because a stack's rules read a child's natural, least and stretch lengths and not its scale:

- **Natural width** is the sheet's width at the frontend's density; **least width** is 0, so a page never raises its
  parent's width the way a badge does, and in a row it is a content child that shrinks, as a wrapping text does.
- **Across a column** it fills the line up to its natural width: it is never wider than the sheet. `alignSelf` places the
  displayed sheet in the rest of the line; without it the page sits at the start.
- **Its height is a function of the width it is given**: `width · sheetHeight / sheetWidth`, and that is the height it
  asks for.
- **Qt Quick**: `Layout.fillWidth: true`, `Layout.minimumWidth: 0`, `Layout.maximumWidth` the sheet's pixel width,
  `implicitHeight` the displayed height, from `layoutPage` with the width the layout gave.
- **Terminal**: the footprint is `min(container width, sheet cells + 2)` columns by sheet rows + 2: the sheet and its
  frame. Within a narrower footprint the sheet pans.

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

Millimetres reach a frontend as a density, a number of the frontend's own units per millimetre. A page and its regions'
rectangles are placed in millimetres, so the density is the only place a frontend's unit enters.

**Qt Quick**: 96 device-independent pixels to 25.4 mm on both axes, the CSS reference pixel the theme already speaks:
A4 is 794 × 1123 px at any screen DPI, on any platform. The logical DPI of the screen is not read. Reasoning: logical
DPI is 72, 96 or 144 depending on the platform and the screen the window is on, and it is not a physical size; reading
it would make the sheet's width, and with it every line break and table column inside the sheet, differ between
machines and reflow when a window moves to another screen, while the theme's own sizes (type included) stay as they are.
The units a region's children use stay theme units (`Theme.unitWidth`) as everywhere; only the page's and the regions'
rectangles are millimetres. The page is an `Item` of its displayed size holding an inner `Item` of the sheet's pixel size
with `scale` set to the layout's scale and `transformOrigin: Item.TopLeft`, so regions, text and hit-testing share one
transform; regions are children of the inner item in node order. The page reports the width its layout gave once the layout pass is over, because the scale changes the height and the layout may answer with another width.

**Terminal**: 10 columns per 25.4 mm across and 6 rows per 25.4 mm down, the typewriter pitch (10 characters and
6 lines to the inch). A cell is about twice as tall as wide, so a page of typed text at this pitch has the proportion of
a page of text: A4 is 83 columns by 70 rows (82.68 and 70.16, rounded to the nearest), an 85 mm × 45 mm window 33 columns
by 10 rows. `ui::toCells` converts by rounding **edges**, not sizes:
`left = round(x · 10 / 25.4)`, `right = round((x + width) · 10 / 25.4)`, the width is `right − left`, and rows
likewise with 6. Two regions that touch in millimetres touch in cells, and a rounded size never drifts from a neighbour's
edge. A region of no stated height is `contentRows` rows tall, where the terminal measured its content in rows: its
anchored edge (the top of a `Top` or `Offset` region, the bottom of a `Bottom` one) is the rounded edge and the other
edge is `contentRows` away, so a footer of three rows is three rows after rounding. The terminal gives `layoutPage` that
height as `contentRows · 25.4 / 6` mm, rounded to the micrometre.

**The terminal's sheet edge.** `paper` draws nothing on a cell grid, so the sheet is drawn inside a one-cell frame
(box-drawing characters, which pans with the sheet: a side that has panned out of view is not drawn): the footprint is the sheet's cells plus two columns and two rows, and the conversion from
millimetres to cells stays exact inside the frame.

**The terminal does not scale** (`scale` is 1 whatever the viewport). A viewport narrower than the sheet shows its left
part and pans horizontally:
- the page takes the keyboard focus itself while nothing inside can (as a read-only `scroll` does), shown as the frame
  drawn in the focus style; Left and Right move the view a cell, Home and End to either end; the other keys and the wheel
  are left to the page's surroundings, so Up, Down, PageUp and PageDown move the enclosing `scroll` vertically;
- while a widget inside is focusable, the page does not take the focus and the view follows the focused widget
  horizontally, as `Scroll` follows it vertically: the widget's columns are brought into view, its left edge first when
  it is wider than the view.

Reasoning: the terminal's job is to render the screen usably, not to reproduce the paper; a sheet of 83 columns fits the
80 to 120 column terminals people use within a few cells, and panning is the minimum that makes the rest reachable.

## The document form and the author API

The document gains three node kinds. `page` and `pageRegion` lower to `ui::Page` and `ui::PageRegion`; `localeScope`
lowers to a slot over a holder column ([The locale scope](#the-locale-scope)).

```json
{"kind": "page", "width": 210000, "height": 297000,
 "margins": {"lit": {"top": 10000, "right": 20000, "bottom": 15000, "left": 20000}},
 "a11y": {"name": …},
 "children": [
   {"kind": "pageRegion", "anchor": "offset", "x": 20000, "y": 45000, "width": 85000, "height": 45000, "children": […]},
   {"kind": "pageRegion", "anchor": "bottom", "children": […]}]}
```

| Key | Kinds | Shape |
|---|---|---|
| `width`, `height` | `page` | Literal integers in micrometres, required. |
| `margins` | `page` | A literal integer (every side), or a literal record of `top`, `right`, `bottom`, `left` (a side left out is 0); default 0. |
| `anchor` | `pageRegion` | The literal `"top"`, `"bottom"` or `"offset"`, required. |
| `x`, `y`, `width`, `height` | `pageRegion` | Literal integers in micrometres; `x`, `y` default 0 and only `"offset"` accepts them; `width` and `height` optional. |
| `locale` | `localeScope` | An expression, required: the BCP 47 tag the scope formats in. |
| `children` | all three | `page`: `pageRegion` nodes only. `pageRegion`, `localeScope`: any nodes. |

Geometry is literal, not computed: it is set once, as a sizing is, and a document that gives a non-literal or non-integer length is refused; a computed length would make the sheet's layout
depend on data and its scale a reactive value.

**The common keys** (`id`, `visible`, `enabled`, `tooltip`, `sizing`, the four bounds, `alignSelf`, `bleed`, `span`,
`testId`, `surface`, `autofocus`, `a11y`, `drag`, `drop`, `keys`) per kind:

| Kind | Honoured | Refused at load |
|---|---|---|
| `page` | `id`, `visible`, `enabled`, `tooltip`, `testId`, `surface`, `a11y`, `keys`, `alignSelf` (`start`, `center` or `end`), `span` | `sizing`, `minWidth`, `maxWidth`, `minHeight`, `maxHeight`, `bleed`, `alignSelf: stretch`, `drag`, `drop`, `autofocus` |
| `pageRegion` | `id`, `visible`, `enabled`, `tooltip`, `testId`, `a11y`, `keys`, `surface` | `sizing`, the four bounds, `alignSelf`, `bleed`, `span`, `drag`, `drop`, `autofocus` |
| `localeScope` | none: a scope is not a widget | every common key; wrap the scope in a node that has them |

`checkAccessibility` requires an `a11y.name` on a `page` ([Accessibility](#accessibility)).

The loader also refuses, with the JSON pointer of the key, a non-region child of a `page`, a `pageRegion` anywhere but
as a child of a `page`, an empty margin box, a negative or out-of-range length, an `x` or `y` on a pinned region, a
region whose rectangle leaves the sheet, and a `localeScope` without a `locale`. The mount refuses the same cases
(`std::logic_error`, naming the test id) for a view tree not built from a document.

**Author API.** The builders follow the author API's order, children first and options second:
`ui::author::page(Nodes regions, PageOptions{width, height, margins})`,
`ui::author::pageRegion(Nodes children, RegionOptions{anchor, x, y, width, height})` and
`ui::author::localeScope(Nodes children, AnyHandle locale)`. The options structs hold only geometry (`Mm` fields, so
`.width = 210` and `.x = 20` compile); `id`, `testId`, `visible`, `a11y`, `surface` and the other common properties are
the `Node` modifiers every node takes (`.testId(…)`, `.a11y(…)`). `localeScope`'s `locale` is a handle, so a scope reads
the tag from a query (the customer's locale), a `let`, or a literal.

## Overflow

A region whose content is taller than its stated height, or whose rectangle runs below the sheet's bottom edge, clips
its content. The clipping is shown, not silent: a reader of an invoice must not take a total under a list that lacks its
last rows for the whole list.

- **Qt Quick** draws a fade over the region's last line with a `…` glyph at its end.
- **The terminal** draws `…` in the last row of the clipped rectangle, at its last column.
- The recording backend records `overflow=true` on the region ([Recording backend](#recording-backend)).

## The locale scope

`ui::author::localeScope(children, locale)` makes **every formatting and every `t` evaluated inside it use the scope's
locale**; the session's locale is untouched, outside the scope and, for `{"env": "locale"}`, inside it.

### What the scope sets

The scope's `FormatContext` is the session's with these members taken from the scope's locale tag `L`:

- `locale` is `L`: `t` resolves `(key, L)` through the same `TranslationProvider`, which walks the tag's prefixes
  (`de-AT`, then `de`) and answers nothing when no catalogue has the key, so `t`'s own fallback text shows. Plural
  selection follows `L`'s language.
- `numeric` and `currency` are those of `localeFacts(L)` ([`locale_table.md`](locale_table.md)), so `number`, `decimal`,
  `quantity` and `money` write `L`'s separators, sign and currency placement. The facts come from the tag alone:
  `Session::setFormatLocale`, which chooses facts apart from the language for the session, does not reach a scope.
- `datePattern` and `dateTimePattern`: **the application's declared pattern wins, else `L`'s row's**, which is the rule the
  session applies ([`locale_table.md`](locale_table.md#who-installs-the-facts)). An application that regulates the form
  of a date (lims's `dd-MMM-yyyy`) keeps that form in a sheet in another locale, because one application writing a date
  two ways is the thing the rule exists to prevent. An application that declares none gets `L`'s: a `de` scope under an
  `en` session writes `24.10.2026`. The `Session` exposes the declared and the locale's patterns the scope reads.
- everything else (`units`, the display zone's offset, a `DateFormatter` the host installed) is the session's. The zone is
  the reader's: a timestamp on a sheet for a customer abroad is shown in the clerk's zone, which is what the clerk
  can check.

A tag with no row in the locale table writes with `en`'s facts and is reported **through the screen's `report`, at the
scope's `locale` path, once per tag for the session's lifetime**: the once-set is held by the `Session`, which every
scope of every screen reaches (the shell's own once-set for a switch of the session's locale is out of the interpreter's
reach). The text is the shell's: ``no formatting facts for locale `pl`; numbers, money and dates use en's``. `L` itself
stays `locale`, so catalogue lookup and plurals still follow it.

### What the `locale` expression may be

It is evaluated in the scope that encloses the `localeScope`, reactively. A string is a tag. `null` and `""` mean "not
known yet" (a query still loading, a customer with no stored locale): the scope then uses the session's locale, without a
report, and switches when a tag arrives. A value that is neither a string nor `null` is a reported mismatch at the
`locale` path, and the scope uses the session's. Reasoning: a sheet built from a loading query must not report a
problem for the moment before its data arrives.

### Where it reaches

The lookup is `RtScope::formatting()`: it walks the scope chain to the innermost scope that carries a context and
answers it, else the session's. `ScopeEnv::formatting()` answers it, so evaluation inside the scope reads it wherever it
reads the environment's `formatting()`: the display functions (`number`, `decimal`, `quantity`, `money`, `date`,
`dateTime`, `dateRange`), `t` and its argument formatting, and `format`. Every site of the interpreter that converted a
node's bound value to text through the session's facts (`bindProp`, `slotProp`, `textOf` for a `text`, a `keyValue`
item, a step, a menu label, a table cell, a chart's text) uses the node's scope's lookup instead, since the node is inside
the scope.

**Entry stays on the session.** A person types in their own locale; what an input shows is what they will type back,
and a number shown as `1.234,50` and parsed as `1.23450` is data loss. So these read the session's facts, whatever
scope encloses them:

- `parseDecimal`, which `expressions.md` defines as entry, and every parse of typed text;
- a text input's draft, its number places and its validation messages, and a date input's `pattern`;
- a form's overlays and engine options, the `arrange` and `pref` editors' ids, and unit and zone reads
  (`convertUnit`, a date's UTC offset): a unit and a zone are the reader's.

A `custom` node inside a scope honours it: its text and number props are converted with the scope's facts, and its
fallback is lowered in a scope of its own, whose names are isolated from the use site's but whose formatting is the
use site's (`RtScope::formatSource`), so the component's fallback writes in the scope's locale.

`{"env": "locale"}` stays the session's: it answers who is reading, not what language the sheet is in.

### Nesting and reactivity

Scopes nest, and the innermost wins; its context is built from the *session's* with its own tag, not from the enclosing
scope's, so an inner scope inherits nothing from an outer one but the session's other members. The scope's context is a
reactive value of the session's formatting facts and the `locale` expression, so a change of the session's declared
patterns or catalogue, or of the tag, re-evaluates exactly the properties inside the scope that read a formatting.

Some values are read once, as a node is lowered, and not by a property: a table's column labels, a `once` property
(`icon`, `mode`, `collapsible`), a literal. A property that is re-evaluated cannot reach them, so **the scope mounts its
children again whenever its tag changes**: the scope's slot is selected by the tag as a string (`""` for a tag not
known yet), and a new key lowers the children afresh in the new facts. Element state (a draft, a selection, a dialog's
openness) lives in the enclosing scope and survives the remount. The one value that does not follow is the session's
own language changing under a scope with an empty tag: that is a rare event for the session itself, and it moves
every property but not the once-read values.

### How it is lowered

The scope is not a widget. The interpreter lowers `localeScope` to a slot (a `Switch` whose selector is the tag and
whose only content is its fallback) over a holder column (the column it inserts around a list of nodes: spacing step 0,
`Stretch` alignment) and mounts the children in a **child scope** (`RtScope`) that is made at each mount and sits under
the scope that carries the context. Both are transparent: they declare nothing, and the elements of the children are
the enclosing scope's, where the document's collection registered them. A holder column stacks its children
vertically, so a `localeScope` inside a `row` is one cell of the row holding its children in a column, as a `boundary`
is.

A `localeScope` is not a region, so it is refused as a child of a `page`. It goes around a `page`, which makes every
region in the sheet format in the scope's locale (`localeScope({page(…)}, customerLocale)`), or inside a region, around
part of its content.

## Accessibility

- **The page is a document region.** The mount gives it the role `document` (`QAccessible::Document` on Qt Quick, which
  the role table gains) and the author names it: `checkAccessibility` requires an `a11y.name` on a `page`, as it does on
  a `table`, because an unnamed document in a screen reader's landmark list reads as "document". The terminal has no
  accessibility tree and records the name for the golden only.
- **A region is exposed only when it is named** (`a11y.name`); an unnamed region is not an accessible grouping, so a sheet
  does not add empty groups to the tree. A region is not a landmark.
- **Reading order is node order.** The regions are children of the page in the order the document lists them, which is the
  order of the accessibility tree, of the keyboard focus (Tab) order, and of the terminal's focus cycle; the geometry
  never reorders them. An author lists the regions in the order the sheet is read: a footer placed at the bottom and
  listed last is read last, and one listed first is read first wherever it sits.
- **Scaling keeps it operable.** A scaled page maps pointer input through the transform; its keyboard focus follows the
  focus of the widget inside.
- The sheet's contrast is the `paper` surface's.
- The scope's effect on the language of its content is the words themselves; Qt Quick's accessible interface has no
  per-item language property to set.

## Backend contract

Two widgets, both made by `IViewBackend`:

| Interface | Factory | Setters |
|---|---|---|
| `PageWidget` (a `ContainerWidget`) | `createPage(parent, PageGeometry)`: the geometry is a factory argument, as a stack's axis is | none. The page reads the width its container gives it from the container's layout; the contract carries no width. |
| `PageRegionWidget` (a `ContainerWidget`) | `createPageRegion(parent, RegionGeometry)` | none: the geometry is a factory argument too. |

A region's parent is a `PageWidget`. Both are visible, enabled, not draggable, without name, role or surface, as every
new widget is ([`backend_contract.md`](backend_contract.md#widgets)); the mount sets the page's role and surface.

A frontend's page holds its regions as children, lays them out from `layoutPage` and its density, and sizes itself by
the [size request](#size-request). It never reports its scale to the mount.

## Recording backend

The recording backend has no pixels, so a page is observed as the numbers `layoutPage` produces.

- **Available width.** `setPageAvailableWidth(std::optional<Mm>)` sets the width every page of the backend is offered; the
  default is unlimited, so the scale is 1. A change re-lays every page out.
- **Content height.** A region without a stated height has the content height `0 mm` until
  `setRegionContentHeight(id, Mm)` gives one.
- **Properties.** Factory parameters are recorded at creation without a log line, as other factory parameters are; the
  properties the layout derives are recorded at creation too and again, **with a log line**, whenever the available
  width or a content height changes them. This is an exception to "what a widget changes by itself has no log line": the
  scale is the thing a test of a narrower page watches.

| Kind | Property | Value |
|---|---|---|
| `Page` | `size` | `210x297`, width and height in the shortest decimal (`148.5`) |
| `Page` | `margins` | `top,right,bottom,left` |
| `Page` | `scale` | the scale in its shortest round-trip decimal (`0.5`, `1`) |
| `Page` | `displayed` | the displayed size, `105x148.5` |
| `PageRegion` | `anchor` | `Top`, `Bottom` or `Offset` |
| `PageRegion` | `rect` | `x,y,width,height` in the sheet's own millimetres, as `layoutPage` resolves them |
| `PageRegion` | `overflow` | `true` or `false` |

- **Log lines**: `create Page#1 in root`, `create PageRegion#2 in Page#1`, and `set Page#1 scale=0.5`,
  `set Page#1 displayed=105x148.5`, `set PageRegion#2 rect=…` and `set PageRegion#2 overflow=true` on a change.
- **Golden dump**: `Page#1 a11yName=… a11yRole=document displayed=… margins=… scale=… size=… surface=paper` and, indented,
  `PageRegion#2 anchor=Offset overflow=false rect=20,45,85,45`.
- **Scope**: there is nothing to observe of a `localeScope` but the text of the widgets inside it and a slot over a holder `Column`.

## Tests

`tests/test_ui_document_page.cpp` runs over the recording backend and the interpreter, each case with the mutation that
must fail it; the frontends' suites hold the cases only they can reach.

| Case | Asserts | Mutation that fails it |
|---|---|---|
| A page at a narrower width | A4 offered 105 mm records `scale=0.5` and `displayed=105x148.5`; offered 300 mm records `scale=1` and `displayed=210x297` | `scale` without the `min(1, …)`; scale applied to the width only |
| A footer at the bottom of a short page | A `Bottom` region of 12 mm content on A4 with a 15 mm bottom margin records `y = 270`; of 30 mm content, `y = 252`; a `Top` region records `y` = the top margin | the `Bottom` anchor placed from the top; the margin ignored |
| An address window at its offset | An `Offset` region at `(20, 45)`, 85 × 45, on a page with margins of 10 top and 25 left records `rect=20,45,85,45` | the offset taken from the margin box |
| `money` in `de` under an `en` session | `money(1234.5, "EUR")` in a `localeScope("de")` shows `1.234,50 €`, outside `€1,234.50` | the scope not installing its context; the scope replacing the session's |
| Bare values in a scope | a bare number bound to a `text`, a literal number, a `keyValue` value and `t` follow the scope | `bindProp` reading the session's facts |
| The declared pattern wins | an application declaring `dd-MMM-yyyy` writes a date in a `de` scope in that form; with none declared, `24.10.2026` | the row's pattern winning |
| Nested scopes | an inner `en` scope inside a `de` one writes `en`'s facts, and one with an empty tag inside a `de` one writes the session's, not `de`'s | built from the enclosing scope |
| `env.locale` | stays the session's inside a scope | read from the scope |
| An empty tag | `""` and `null` write the session's, with no report; a number as the tag is reported at the `locale` path | the report on `null` |
| An unknown tag | `pl` writes `en`'s facts and is reported once for two reads and two scopes | the report on every read |
| The session changes under a scope | changing the session's declared date pattern re-writes a date in the scope; switching the session's language alone leaves the scope as it was | the scope reading a stale context |
| Entry in a scope | in a `de` scope under `en`, `parseDecimal("1.5")` is `1.5`; a number input shows a bound 1.5 as `1.5` and commits a typed `2.25` as 2.25 | `parseDecimal` reading the scope's facts; the draft reading the scope's |
| Stateful nodes in a scope | a `textInput`, `select`, `checkbox`, `dateTimeInput`, `table` and `dialog` inside a scope mount and keep their state | the element lookup stopping at the scope |
| A tag that arrives or changes | `null` then `de` then `en` after the mount re-writes a bound number, a literal, a `t`, and a table's translated column labels; the labels follow a tag a query delivers | the tag not read; the slot not keyed by the tag |
| A `custom` node | the fallback of a component inside a `de` scope writes `1,5` | the fallback's scope not linked to the use site |
| Overflow | a region of stated height 20 mm with 30 mm of content records `overflow=true` | no flag |
| Refusals | each case of the document form refused at load with its pointer and at mount with its test id | each check removed |
| Placement on the frontends | per frontend, a content-sized `Bottom` region's bottom edge lies at the margin and an `Offset` region lies at its pixel or cell position; a page in a column narrower than the sheet is as wide as the column and scaled | a content height of 0 passed to the layout; content-sized page |
| The terminal | `toCells`: A4 is 83 × 70 cells, the window at `(20, 45, 85, 45)` mm is columns 8 to 41 and rows 11 to 21, abutting regions share an edge, a three-row footer is three rows | rounding sizes instead of edges |

Qt Quick's suite checks the page item at 794 × 1123 px whatever the screen's DPI.

## Alternatives considered

- **A `custom` QML component per document.** The design forbids per-application client code: every screen is a ui/1
  document, and a client that cannot render an invoice without the invoice's QML is a client the server must ship code
  to.
- **Rendering the sheet to an image on the server.** Document rendering is outside what morph builds. A rendered image
  is also not a ui/1 document: it has no text to select, find, read aloud or test; it does not follow the theme or the
  width; and it moves the formatting of the sheet to the server, which then needs the client's fonts.
- **Millimetres in `LayoutHints`.** A physical length means something only inside a page, where the density is defined,
  and an extent in millimetres in a column would be a unit the terminal and the theme cannot honour.
- **A flowing page**, whose body sits below the letterhead and above the footer. Rejected for the reason in
  [The page](#the-page): it needs measurement before placement.
- **Only `Offset` regions, no `Top`/`Bottom` anchors.** A `Bottom` region needs its height before it is placed; the
  anchor places it from the edge it is pinned to, so the footer can grow without the author recomputing its top.
- **Scaling by a transform in every frontend's own units** rather than millimetres and a function. It would put the page
  arithmetic in two backends and no test could reach it without pixels.
- **A locale for the whole screen only** (`setLocale` on a screen). It moves the person's whole interface, and then moves
  it back; a clerk reading a German customer's invoice would see their own buttons turn German.
- **A `locale` argument on each display function.** It reaches neither `t` nor a bare number a node converts to text,
  which is why the scope is a node.
- **Putting the locale on the page** (`page(…, locale)`). A letter in one locale, a table in another and a summary box in
  the clerk's own are different scopes; the locale of a subtree is not the page's.
- **A scope that also governs entry.** Rejected above: it creates numbers an input shows and cannot read back.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Length type | `Mm`, an exact micrometre count | Exact comparison in tests and on the wire; finer than any device. |
| Placement | Stated geometry, not flow | No measuring before placing; the same on every frontend; the standards state millimetres. |
| Page children | Regions only | A page with its own flow is a second layout model. |
| Offset origin | The sheet's edge (the brief anchors regions to the margin box) | The figures a form's standard gives are measured from the edge; `Top` and `Bottom` follow the margin box. |
| Pinned regions with an offset | Refused | An offset on a pinned region has two readings. |
| Size request | Natural = the sheet, least = 0, height for width | A page neither raises its parent nor overflows a narrow column. |
| Scale | `min(1, available / width)`, uniform, no floor | Fits without sideways overflow; never enlarges. |
| Scaling trigger | Available width | A class threshold can leave a sheet wider than its window. |
| Qt density | 96 device-independent px per 25.4 mm, DPI not read | The sheet's proportions to the theme's type do not depend on the platform. |
| Terminal density | 10 columns and 6 rows per 25.4 mm, edges rounded, a one-cell frame | A page of text reads as a page; abutting regions abut; the sheet has an edge. |
| Terminal scaling | None; horizontal pan and follow-focus | A cell is the smallest thing it draws. |
| Overflow | Shown on both frontends, recorded | Clipped data must not read as complete data. |
| Sheet colour | A built-in `paper` surface with every role literal | Paper is white in a dark theme; a theme may still replace it by name. |
| Scope's date patterns | The application's declared pattern wins | One application, one date form; the same rule as the session's. |
| Scope's zone and units | The session's | The zone is the reader's. |
| Scope and entry | Entry and `parseDecimal` keep the session's facts | What an input shows and a body parses must be what the person types. |
| Scope's tag `null` or `""` | The session's locale, silently | Loading is not a problem. |
| Scope's lowering | A slot over a holder column, and a child `RtScope` per mount | The view tree needs no new kind, and the formatting lookup is the interpreter's. |
| Unknown tag | `en`'s facts, reported once per tag through the screen's `report` | The interpreter reaches the screen's report and the session, not the shell. |
| Node name | `page` | The brief names it; the sheet is its picture. |

## Out of scope

- Rendering a page to a file or a printer (PDF, print files) and pagination onto several sheets: document rendering is
  domain output and is not built here. A sheet is a view on the screen; a region taller than the sheet is clipped and
  marked.
- Paper sizes as names (`A4`, `Letter`): an application passes millimetres.
- Rotating, zooming or panning a page on Qt Quick, beyond what the enclosing `scroll` does.
- A display zone of its own for a scope.
- Right-to-left layout and locale-specific digit shapes beyond what `NumericLocale` already holds.
