# LIMS gallery batch I: the head's line, the shell's surfaces, edges that bleed, and component looks

Batch I of the LIMS gallery visual review. A fresh comparison after batch H (all ten screens at 1000×830 and 390×844,
against the mockups and the pre-H grabs) found three regressions, sixteen framework gaps (G-a to G-p) and the screen
fixes that the vocabulary already expresses. This design closes all of them. Writing it found three more framework
gaps, which it closes too:

- G-q, a `row` cannot be `divided`;
- G-r, a banner's action has one look and one place;
- part of G-m: an icon-only push button whose icon the theme does not name draws blank.

The aim is the review's: ui/1 itself produces each mockup's look. A difference is a vocabulary or frontend change,
and a screen changes only where the vocabulary already says what the design says. The rules of batches G and H hold:

- A look is a token or a rule, never a screen's choice of pixels.
- A key means the same thing on every kind that takes it.
- The strong weight is the theme's one emphasis weight.
- The terminal keeps its cells. A purely visual change says what it leaves unchanged there, and a change to what a
  user can read or reach is drawn on both frontends.
- A look that changes every application's shell is the theme's to switch.

Only environmental differences stay out (the fonts: Inter and JetBrains Mono are not installed here).

Every claim below was checked against `4d86dd7f4` (`lg-i-design`; the grabs in `lg-cmp/app-hh` were taken from a build
of it). Each item says whether its premise was **reproduced** (run or measured here) or **read** (inferred from the
code). Grabs are `morph_examples_client --app lims-gallery --screen lims-gallery.<id> --grab-size WxH --grab FILE`;
mockups are the `design/mockups/*.dc.html` files rendered in headless Firefox (`lg-cmp/mock/*.png`). A mockup figure
is read from its CSS where the CSS gives it and measured in the render otherwise.

## Probes

Four throwaway probes ran with `qml6` and `qmltestrunner` (Qt 6.12.0, offscreen) and are kept out of the tree
(`lg-id-probe1.qml` to `lg-id-probe4.qml` in the session scratchpad). Qt 6.8 was not run; every property used below
predates Qt 6.

**1. The unit, offscreen.** The gallery's theme names no font family, so a unit is the platform font's average
character width:

```
PROBE platform family Sans Serif px 12 pt 9 avgW 7.671875 h 16.46875
PROBE 13px avgW 8.3125 h 17.84375 expanded at 1227.5 px; compact below 613.75
```

A 1000 px window is 130 units, a `medium` one. H's probe 3 agrees: 32 units measured about 248 px there.

**2. A negative layout margin.** A rule in a `ColumnLayout` 200 px wide, given `Layout.leftMargin: -16` and
`Layout.rightMargin: -16`:

```
PROBE neg-margin rule x -16 w 232 | sibling x 0 w 200 | col w 200 implicitW 50
```

A layout honours negative margins, so a child can reach past its container's padding without a second layout pass.
G-d and G-f rely on this.

**3. A pointer target larger than what is drawn.** A `Button` anchored 6 px outside a 32 px slot on every side, with
its background inset by 6 px on every side, clicked 4 px and 8 px left of the drawn square:

```
PROBE button -6 -6 44 44 background 6 6 32 32
PROBE click 4px outside the drawn square -> 1
PROBE click 8px outside -> 1
```

The second count stays 1: a press outside the 44 px square does not reach it. A child that reaches past its parent
still takes the press. R1 relies on this.

**4. The style's own heights.** A Basic `TabButton` and `CheckBox` in the platform font:

```
PROBE tab implicitHeight 40 content 17 top 6 bottom 6 bar 40
PROBE checkbox implicitHeight 40 top 6 bottom 6 indicator 28 content 17
```

`MorphTabs` and `MorphCheckbox` replace the style's height with their own, so the grabs measure what the frontend
draws:

- instrument's strip is white from y = 76 to 95 (20 px) under the head's rule at y = 75;
- the strip's rule is at y = 96;
- the first card's border is at y = 125, 28 px under the rule.

That is `instrument-1000x830.png`, read at x = 500.

## The items

| Item | Where it lands | Verdict on the finding |
|---|---|---|
| R1 | an unframed square draws at the control height and keeps the touch target as its pointer target | Right (reproduced) |
| R2 | closed by G-a (`leadPlacement: end`) | Right |
| R3 | screen fix: review's queue entry | Right |
| G-a | the screen's `leadPlacement` (`below`, `inline`, `end`); the app document's `principalPlacement` | Right. **Wider than listed:** the console's user name is the shell's principal, which needs a place in the top bar |
| G-b | the shell's top line centres its items while the head is one line | Right (reproduced: 11 px off at 390) |
| G-c | a tab is the control height; an inset tabs node drops the spacing over its page | Right (reproduced, probe 4) |
| G-d | a dialog's and a drawer's rules span the overlay | Right (probe 2) |
| G-e | a sheet's head centres its title and draws a rule under it | Right |
| G-f, G-k | one key, `bleed`: a child spans past its container's horizontal padding | Right. They are one seam: the same need in two screens |
| G-g | the built-in surface `compactToolbar`; an action's `iconOnly` | Right |
| G-h | the width classes' `expanded` bound | **A decision for the user** (Decisions for the user) |
| G-i | `leadPlacement: inline` (G-a) | Right |
| G-j | the theme switch `collapsible.marker` | Right |
| G-l | a check box's own vertical padding goes; its box grows on compact | Right |
| G-m | built-in chevrons for `nav.back` and `nav.forward`; an icon-only button is never blank | Right. **Wider than listed:** a push button with an icon its theme does not name draws nothing today |
| G-n | the flag `headJoined`; the built-in surface `toolbarJoined` | Right: after G-c the rule cuts a 32 px white block, and the mockups draw none |
| G-o | a step connector stands off its neighbours by step 2 | Right |
| G-p | the splitter handle's colours and grip | Right |
| G-q | `divided` on a `row` | Found here: audit's expanded fix needs it |
| G-r | a banner action's `style` and `stretch` | Found here: console's and stability's fixes need it |

## Theme switches and surfaces

One new look changes every application that uses collapsible sections, so it is a switch, off by default:

| Token | Values (default first) | What it switches | Read by |
|---|---|---|---|
| `collapsible.marker` | `shown`, `none` | Whether a collapsible section's header and a collapsible panel's header draw their ▸/▾ chevron (G-j) | `MorphSectionHeader` (through `Theme.collapsibleMarker`) |

- It is a `keyword` token, as `shape.iconOnly` is: `kKeywords` gains its entry and `kThemeTokens` grows from 67 to 68.
- Any other value is refused at `addFile` (`collapsible.marker: only shown and none are drawn`).
- The terminal reads no theme token and keeps its `▸ `/`▾ ` heading.

Two built-in surfaces join the five, so the shell's top bar can change its look per form and per page with no new
pure virtual (G-g, G-n):

| Surface | Background | Colour roles | The shell draws it |
|---|---|---|---|
| `compactToolbar` | the `toolbar`'s, with its border | the `toolbar`'s | as its top bar while the frame is compact |
| `toolbarJoined` | the `bar`'s: `color.surface`, no border | the `bar`'s | as its top bar while the shown page continues the head's block (G-n), and the frame is not compact |

- A theme index's surface of the same name replaces either one, as for the five, so an application gets today's
  look with no theme file.
- The gallery's index adds `"compactToolbar": "theme/compact-toolbar.tokens.json"`. That file sets the dark head of
  `10` compact:
  - `color.surface`, and `color.border` the same, so no rule shows: `#1C1E21`;
  - `color.text` and `color.accent`: `#FFFFFF`;
  - `color.muted`: `#C3C9D2`;
  - `color.hover`: `#FFFFFF1F`.
- The terminal draws both inverted, as it draws `toolbar` (`invertsSurface`), so its top bar keeps its look.

The gallery's bundle (`lims_gallery.cpp`) turns `collapsible.marker` to `none` and declares the compact toolbar.

## R1: an unframed square keeps its glyph's size

**Problem (reproduced).**

- **The grabs.** With `shape.iconOnly: square` the shell's ghost ☰ is a 44 px square on compact (`touchTargets`) and
  32 px at 1000 (`MorphMenu`: `Math.max(trigger.implicitWidth, trigger.implicitHeight)`, the trigger's background
  `implicitHeight: Theme.targetHeight`).
  - The title moves from x = 44 to 68 at 390, and to 56 at 1000.
  - Instrument compact's badges wrap, and its head grows from 76 to 100 px.
  - Results compact's title wraps ("…Paracetamol 500 / mg"), and its head grows from 59 to 98 px.
- **The mockup.** The ☰ of `10` compact is drawn small and unframed. The title starts about 54 px from the card's
  edge: 14 px of padding, a 32 px square, then an 8 px gap.
- **The framed ⋯ is different.** `03` compact draws its framed ⋯ 44 px square, level with its 44 px Book and Log
  maintenance buttons (x = 1418 to 1460).

**What a touch target requires.**

- WCAG 2.2 SC 2.5.8 (AA) asks for a 24×24 CSS px target, or enough spacing around a smaller one.
- SC 2.5.5 (AAA), Apple's HIG and the theme's own `control.touchHeight` ask for 44.
- Material separates the two: a 24 dp icon in a 48 dp target.
- What is measured is the area that takes the press, not what is drawn (probe 3).

**Decision.** An **unframed** square (a ghost push button or a ghost menu trigger, drawn square by `shape: square` or
by the theme's `shape.iconOnly`) is drawn at the control height and keeps the touch target as its pointer target:

- **It lays out and draws at `Theme.controlHeight`** square, 32 px at the default scale, at both densities. Its hover
  tint and focus ring are that square.
- **Its pointer target is the touch height's square around it**, `Theme.targetHeight`: 44 on compact and 32
  elsewhere. The control inside the widget is anchored `(targetHeight − controlHeight) / 2` past each edge, with its
  background inset by the same.
  - The extra 6 px lies inside the gaps around the ☰: the shell's top line pads its start by step 3 (12 px), the gap
    to the title is step 3, and the top bar pads its line by step 2 (8 px).
  - Where a neighbour's own target is closer than 6 px, the overlap goes to the item painted later, the later sibling,
    as Qt Quick delivers to the topmost item. The spec says so.
- **A framed square is unchanged.** It is the target height, as H decided, since its frame is what lines it up with
  the 44 px buttons beside it.
- So the ☰ is a 32 px slot on both forms, and the title starts at 12 + 32 + 12 = 56 px. The head's height is set by
  the title and the trail again, not by the ☰.

**Per backend.**

- **Qt Quick:** `MorphButton` and `MorphMenu`'s trigger gain `readonly property bool glyphSquare: squared && ghost`
  (`squareTrigger` and ghost emphasis for the menu). While it holds:
  - the widget's implicit size is `Theme.controlHeight` on both axes;
  - the inner `Button` is anchored to fill the widget with `anchors.margins: -(Theme.targetHeight -
    Theme.controlHeight) / 2`;
  - its `topInset`, `bottomInset`, `leftInset` and `rightInset` are that same amount, so `MorphButtonBackground`
    draws the 32 px square.
- **Terminal:** unchanged. It reads no theme switch, and a cell is the target.

**Spec.** `theme.md` (`shape.iconOnly`: "an unframed square is drawn at the control height, its pointer target the
target height"); `document.md`, the `button` row (`square`); `qt_quick/backend.md` (Button, Menu).

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`, under `shape.iconOnly: square` with `touchTargets` set:

- the ghost `nav.menu` trigger of a button-style menu is `Theme.controlHeight` wide and tall;
- a press 4 px left of it opens its popup, and a press 8 px left does not;
- a framed `more` trigger is `Theme.targetHeight` square.
- **Control:** draw a ghost square as a framed one, and the first check fails (44). Drop the margins, and the second
  fails.

**Verify.** Re-grab console, instrument and results at 390×844. The title starts at x = 56. Instrument's badges keep
one line, and results' title wraps no more than its own length makes it.

## R2 and G-a: where the head's lead sits

**Problem (reproduced in the grabs; read).**

- **The mockups put status at the end of the head's first line:**
  - receipt: "Session since 08:30", then "12 received" and "1 rejected" (`02`, at the trailing edge; compact "12" and
    "1");
  - results: "In progress" at the trailing edge of its eyebrow line (`08`);
  - console: the lab select after the title; at the trailing edge, the search, "Connected" and the user's name, "Dr.
    A. Meyer" (`10`: `<span class="tag t-ok">Connected</span><span class="muted">Dr. A. Meyer</span>` closing its
    `<header>`).
- **Plate's subtitle follows its title on the same line** (`07`: "Plate PL-0193  ELISA, host cell protein · worksheet
  WS-2026-0415"). That is G-i.
- **The shell draws the lead only under the title.** `buildHeadLead` puts the subtitle and the badges in one row, which
  the shell mounts in `headLeadSlot()`, under `shell_title` in the lead block.
  - R2 is this: H moved results' worksheet line to the eyebrow, which left "In progress" alone on a line under the
    title. The head grew from 58 to 76 px.
- **The principal is the sidebar's**, at its foot (`ShellFrame::_principal`). The console mockup's sidebar has no foot;
  the name closes the top bar.

**Decision.** The lead (the subtitle and the badges, which share a line) gets a placement, and the principal gets a
place in the top bar.

### The screen's `leadPlacement`

`leadPlacement` is a screen member, an expression of a name:

- **`below`** (the default): today's place, under the title.
- **`inline`**: on the title's line, right after the title. The lead starts at the title's end and may stretch to the
  trail. Where it does not fit, it wraps to a line of its own under the title, before the trail does.
- **`end`**: at the trailing edge of the head's first line, after the tools and before the actions and the menu. In
  reading order, a lead at the end comes after the tools.

It is a name because plate needs `inline` on expanded and `below` on compact. A literal is checked at load, and any
other name is refused (`` `leadPlacement` is `below`, `inline` or `end` ``). A computed value that names none is
reported once at its path and taken as `below`. The app document refuses it, as it refuses every head member.

**How the workspace builds it.** The placement can change at run time, so the workspace builds the lead once per
place, each shown only while its placement is the one in force. A hidden copy is in neither the layout nor the
accessibility tree.

- `HeadParts` gains `inlineLead`. The three copies' test ids are:
  - below: `head_lead`, `head_subtitle`, `head_status_<n>` (as today);
  - inline: `head_lead_inline`, `head_subtitle_inline`, `head_status_inline_<n>`;
  - end: `head_lead_end`, `head_subtitle_end`, `head_status_end_<n>`.
- `buildHeadLead(tab, LeadPlacement)` takes the place it builds for. The copy's visibility is `leadHasContent(tab) &&
  leadPlacementOf(tab) == place`.
- `buildHeadTrail` inserts the `end` copy between the tools' `forEach` and the actions' `forEach`. `trailHasContent`
  counts it.
- **In the page** (`HeadPlacement::InPage`) the tab bar carries the title, so there is no title line. `inline` draws
  as `below` there, and `end` sits in the trail as in the shell.
- `ScreenHead::leadPlacement()` is a virtual returning `reactive::Computed<LeadPlacement> const*`, default `nullptr`
  (`below`), so a host screen's head compiles unchanged.
- `enum class LeadPlacement : std::uint8_t { Below, Inline, End }` sits beside `HeadBadge` in `interpreter.hpp`.
- The interpreter's `Screen::leadPlacement()` is a tracked read of the member.

**How the shell places `inline`.**

- `ShellFrame::headInlineSlot()` is a container in the wrapping `_topMain`, after `_leadBlock` and before
  `_headSlot`. Its member `_headInline` is declared between `_headLead` and `_headSlot`, so declaration order is
  creation order.
- `ShellFrame::setLeadInline(bool)` moves the 32-unit minimum (`kShellHeadLeadMinimum`) from the title block to the
  inline slot:
  - **inline:** the lead block is content-sized with no minimum. The inline slot stretches from the 32-unit minimum,
    capped at its natural width as the lead block's is today, and shows while the head shows.
  - **not inline:** the lead block stretches from 32 units, as today, and the inline slot is hidden. A shown, empty
    slot would still take its minimum.
- So plate at 1000 lays out its title, then the subtitle stretching to the trail. Where the trail does not fit beside
  them, the trail drops to a line of its own, as it does today.

### The app document's `principalPlacement`

`principalPlacement` is an app document member, a literal name: `sidebar` (the default) or `head`. A screen refuses
it (`` `principalPlacement` is the app document's ``), and any other name is refused at load.

- `AppRoot::principalPlacement()` answers it, as `PrincipalPlacement` (`Sidebar`, `Head`), defined beside
  `NavigationBadge`.
- `ShellFrame::setPrincipalInHead(bool)`:
  - **in the head:** the principal is a muted text, `shell_head_principal`, at the top line's trailing end, after
    `_topMain`. Its member `_topPrincipal` is declared between `_headSlot` and `_topEnd`. The sidebar's
    `shell_principal` is then hidden.
  - **compact:** the head's principal is hidden, and the navigation menu's "Sign out" entry stays described by it, as
    today. `10` compact draws no name.
- The sign-out button stays at the sidebar's foot.
- The client (`src/client/shell.cpp`) sets it after `mountApp()` from `AppRoot::principalPlacement()`.
- It is the app document's, not the theme's, because it is where a fact sits, not a look; a theme does not reach
  `ShellFrame`, which is built from backend widgets in C++. The default keeps every application's shell as it is.

**Per backend.** Nothing new. Both frontends draw the same rows and texts. On the terminal a lead at the `end` is one
more run of cells before the actions, and an inline lead follows the title on its row.

**Spec.**

- `document.md`: "The document" (`leadPlacement`, `principalPlacement`) and "The head" (the three places and their
  order);
- `host.md` (`ScreenHead::leadPlacement`);
- `interpreter.md` (`Screen::leadPlacement`, `AppRoot::principalPlacement`);
- `workspace.md` (`HeadParts::inlineLead`, the three copies and their ids);
- `shell_frame.md`: `headInlineSlot`, `setLeadInline`, `setPrincipalInHead`, and the Shape table's top line;
- `author.md` (`Scope::leadPlacement`, the app document's `principalPlacement`);
- `client/client.md` (the principal's placement).

**Tests.**

- Corpus (`tests/ui/corpus/documents.json`):
  - `leadPlacement: "end"` loads on a screen, and `"top"` is refused;
  - `leadPlacement` is refused on the app document;
  - `principalPlacement: "head"` loads on the app document and is refused on a screen.
- `tests/test_ui_workspace.cpp`:
  - with `end`, `head_lead_end` is visible inside `head_trail` between the tools and the actions, and `head_lead` is
    hidden;
  - with `inline`, `headPartsForActive().inlineLead` is visible and the lead is hidden;
  - a width change that switches `inline` to `below` swaps them;
  - **Control:** build the lead in one place whatever the placement, and the first check fails.
- `tests/test_ui_shell_frame.cpp`:
  - after `setLeadInline(true)`, `shell_topbar_main`'s children are, in order, the menu, the lead block,
    `shell_head_inline` and `shell_head`; the lead block is recorded `content` with no `min`, and the inline slot
    `stretch(1)/content min=32`;
  - after `setPrincipalInHead(true)`, `shell_head_principal` shows the principal and `shell_principal` is hidden;
  - compact hides both;
  - **Control:** a no-op `setLeadInline`, and the first check fails.
- `tests/client/test_shell.cpp`:
  - an app document with `principalPlacement: "head"` shows the principal in the top bar;
  - a screen with `leadPlacement: "inline"` mounts its lead in `headInlineSlot()`;
  - **Control:** a client that never calls `setPrincipalInHead`.
- `tests/tui/test_tui_shell_frame.cpp`: at 120 columns, a head with `end` draws its badge on the title's row after the
  tools.
  - **Control:** `below`, which draws it on the next row.

**Gallery** (lane I-S):

- receipt: `leadPlacement: end` at both widths;
- results: `end` on expanded, and "In progress" hidden on compact (`08` compact draws none);
- console: `end`, with "Connected" hidden on compact (`10` compact draws none);
- plate: `inline` on expanded and `below` on compact;
- the app document: `principalPlacement: "head"`.

**Verify.** Re-grab receipt, results, console and plate at both sizes.

- Receipt's session line and badges close the title's line.
- Results' "In progress" sits at the right on the eyebrow's line, and its head is no taller than the eyebrow and the
  title.
- Console's line ends with "Connected" and "Dr. A. Meyer".
- Plate's subtitle follows "Plate PL-0193".

**Residual.** The console's lab select is a head tool, so it sits at the trailing edge with the search, not after the
title as `10` draws it. No mockup but the console puts a tool at the leading side.

## G-b: a one-line head is centred across its line

**Problem (reproduced).**

- In `audit-390x844.png` the title's ink runs from y = 13 to 24, centre 18.5. The Export PDF button's fill runs from
  y = 8 to 51, centre 29.5, as does the ☰ glyph's (25 to 34). The title is 11 px high of the line's centre. At 1000
  the title sits at the top beside the 32 px buttons the same way.
- **The cause.** `ShellFrame::_topMain` aligns its items `Align::Start`, so that a lead block of several lines (an
  eyebrow, the title, the lead) lines its top up with the trail's. That is right for instrument (`03`: the actions'
  top at the eyebrow's) and results (`08`: "In progress" on the eyebrow's line). But a block that is the title alone
  sits at the top of a 44 px line.

**Decision.** While the head is one line, the top line centres its items across it; otherwise their tops align, as
today.

- `Workspace::activeHeadOneLine()` is a tracked read: `true` while the active tab's head has no eyebrow and its lead
  is not drawn below the title (no lead content, or a placement of `inline` or `end`).
- `ShellFrame::setHeadOneLine(bool)` sets `_topMain`'s align to `Center` or `Start`. A wrapping row aligns per line
  (`MorphStack`, the line's `align`), so a trail that wrapped to its own line is centred on that line.
- The client sets it from an effect, `_headLayout`, owned and reset with `_headShown`. The same effect also calls
  `setLeadInline` (G-a) and `setHeadJoined` (G-n), each from its own tracked read.

**Per backend.** Qt Quick: the per-line alignment already exists. Terminal: a one-line head is one row, unchanged.

**Spec.** `shell_frame.md` (`setHeadOneLine`; the top line's alignment); `workspace.md` (`activeHeadOneLine`).

**Tests.**

- `tests/test_ui_workspace.cpp`:
  - `activeHeadOneLine()` is `true` for a title with `end` badges;
  - it is `false` with an eyebrow, and `false` with a subtitle below;
  - **Control:** ignore the eyebrow, and the second check fails.
- `tests/test_ui_shell_frame.cpp`: `setHeadOneLine(true)` records `shell_topbar_main` with `align=center`, and `false`
  with `align=start`.
  - **Control:** a no-op.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a compact `ShellFrame` 390 px wide whose title is alone and whose
  trail is one button. The title's vertical centre is within 1 px of the button's.
  - **Control:** keep `Start`, and the offset is 11 px.

**Verify.** Re-grab audit and samples at both sizes. The title's centre is level with the actions'.

## G-c: tabs at the control height, and the page under an inset strip

**Problem (reproduced, probe 4 and the grab).**

- **The strip's height.** Instrument's strip is 20 px tall at 1000 (y = 76 to 95). `MorphTabs` sizes a tab
  `Math.max(implicitContentHeight + topPadding + bottomPadding, Theme.touchTargets ? Theme.targetHeight : 0)`, so
  off compact a tab is its label and the style's padding.
  - The mockup's tab is `font:500 13px; padding:9px 2px; border-bottom:2px`, about 38 px.
  - Its compact tab is `min-height: 44px`, which `targetHeight` already gives.
- **The inset.** H8's inset page sits 28 px under the strip's rule (y = 97 to 124): the column's `spacing:
  Theme.spaceOf(3)` (12) plus the page's `Layout.margins: root.insetPx` (16). The design's is 16.

**Decision.**

- **A tab is at least the control height**, `Theme.targetHeight`, on every form: 32 px, and 44 on compact as today.
  The More button follows the bar's height, as it does today. A tab is a control, and 20 px was below the height of
  every other control.
- **An inset tabs node puts no spacing between its strip and its page.** While `inset` is above 0, the column's
  spacing is 0, and the page's inset is the gap: 16 px. Without `inset`, the strip keeps its step-3 spacing.
- This changes every application's tab strips, the workspace's own included: about 12 px taller at the default
  scale. It is a rule, the control height, read from the existing token. Decisions for the user, item 2, asks whether
  to keep it a rule.

**Per backend.**

- **Qt Quick:** `MorphTabs`. The `TabButton`'s `implicitHeight` is `Math.max(…, Theme.targetHeight)`, and the layout's
  `spacing` is `root.inset > 0 ? 0 : Theme.spaceOf(3)`.
- **Terminal:** unchanged. A tab is one row, and an inset page already starts at `barRows + _insetRows`
  (`TabsImpl::childAreas`), with no row between.

**Spec.** `document.md`, the `tabs` row (the page is `inset` under the strip's rule); `qt_quick/backend.md` (Tabs).

**Tests.** `tests/qt_quick/test_qt_quick_containers.cpp`:

- an expanded tabs node's tab button is `Theme.controlHeight` tall;
- with `inset: 4`, the page's first child is `Theme.spaceOf(4)` under the strip's rule;
- **Control:** today's `MorphTabs`, which gives about 20 and 28.

**Verify.** Re-grab instrument at 1000. The strip is 32 px tall, and the first card is 16 px under its rule.

**Residual.** 32 px against the design's 38. Every control is the control height, as G8 decided.

## G-d: a dialog's rules span the overlay

**Problem (read; the grabs show it).** `MorphDialog`'s `headRule` and `footerRule` are children of the content column,
inside the popup's `padding: Theme.spaceOf(4)`. They stop 16 px from each side. `MorphDrawer` does the same. The
mockups' rules span the dialog (`Main` expanded: `border-bottom:1px` across the head; `08`'s change dialog likewise).

**Decision.** Both rules span the overlay's full width. Each takes `Layout.leftMargin: -popup.leftPadding` and
`Layout.rightMargin: -popup.rightPadding` (probe 2). The popup does not clip, and its background spans its whole
width, so the rule is drawn to the border. Dialogs and drawers both, since the rule means the same on both.

**Per backend.** Qt Quick only. The terminal draws no rules.

**Spec.** `qt_quick/backend.md` (Dialog, Drawer: "the head's and the footer's rules span the overlay").

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`: an open dialog with a description and an action. Both
`headRule` and `footerRule`, mapped to the popup's background, run from x = 0 to its width.

- **Control:** today's rules, which start at `Theme.spaceOf(4)`.

**Verify.** Re-grab e-signature at 1000 and results at 1000 (the change dialog).

## G-e: a sheet's head

**Problem (read; the grabs show it).** `Main` compact draws a sheet's head as "Cancel" at the leading edge and the
title centred on the sheet, with a rule under the head across the sheet. `MorphDialog`'s `sheetHead` is a `RowLayout`
of the dismiss button and a `fillWidth` label aligned left. Its `headRule` shows only with a description, which a
sheet usually lacks (e-signature's is empty on compact).

**Decision.**

- **The title is centred on the sheet.** The label centres its text, and a trailing `Item` as wide as the dismiss
  button balances the row, so the title centres on the sheet rather than on the room left.
- **A sheet always draws its head rule**, edge to edge (G-d), under the head row, or under the description where there
  is one.

**Per backend.** Qt Quick only. The terminal draws an overlay as a box with its title in the frame.

**Spec.** `document.md`, the `dialog` row (`sheet`: "a head of a dismiss control and the title centred on the sheet,
ruled off from the body"); `qt_quick/backend.md` (Dialog).

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`: a sheet 390 px wide.

- Its title label's text centre is within 1 px of 195.
- `headRule` is visible, from x = 0 to 390.
- **Control:** today's head. The centre is at about (390 + dismiss width) / 2, and the rule is hidden.

**Verify.** Re-grab e-signature at 390. Compare with `Main` compact.

## G-f and G-k: a child that bleeds to its container's edges

**Problem (read; the grabs show both).**

- **Review's flagged rows.** `09` compact draws them edge to edge, divided and tinted. The pinned body insets
  everything by 16 px.
- **Receipt's session table.** `02`'s "This session" card puts its table's header band and rules to the card's edges,
  while the card's title and the footnote ("S-26-104228 rejected: seal broken.") stay 12 px in. `panel`'s `padding`
  applies to every child alike.
- **One seam, not two bugs.** The same hand-rolled need shows in two screens (AGENTS.md: "The same example defect in a
  second place … is a missing framework seam"). Each screen today can only drop the padding for everything, or nest
  columns and pad every other sibling by hand.

**Decision.** `bleed`, a literal flag a node may carry.

- **What it does.** The node's sides lie at the edges of the nearest padded container around it, past that
  container's horizontal padding, while its siblings stay inside. Only the sides bleed: a node's place along a column
  is its siblings', and the designs need nothing else.
- **Through unpadded stacks.** A stack with no horizontal padding passes on the inset it was given. So a node in an
  unpadded column inside a padded one (a screen's content inside `pinned`'s body) reaches the padded one's edges.
- **Where the reach stops.** The padded containers are a `column` or `row` with padding, a `panel`, and a `tabs` page
  (padded by `inset`). The reach stops at the first padded one, or at any other kind.
- **Where it is allowed.** `bleed` is given only on a child of a `column`, of a `panel` or of a `tabs` tab, and not on
  the view root. Elsewhere it is refused at load (`` `bleed` is a child's of a column, a panel or a tab ``), since a
  row's or a grid's child has neighbours at its sides.
- **No padded container in reach.** A node that reaches none spans its parent as any child does. That can only be
  known once the padding is evaluated, which may be computed.
- **In the view tree** it is `LayoutHints::bleed` (`bool`, appended after `alignSelf`). It goes through `setLayout`,
  so no backend gains a method.
- **Author:** `Node::bleed()`.

**Per backend.**

- **Qt Quick:** a container writes `bleedInset` on each child it adopts, and again whenever its padding changes, as
  it writes `crossAlign` (`widget.cpp`):
  - a padded `MorphStack` writes its `insetX`;
  - an unpadded one writes the `bleedInset` it was given;
  - `MorphPanel` writes `Theme.spaceOf(padding)`, so a child reaches the frame's inner edge, not over its border;
  - `MorphTabs` writes `insetPx` on its pages' children.
  - `MorphWidget` binds `Layout.leftMargin` and `Layout.rightMargin` to `bleed ? -bleedInset : 0` (probe 2).
- **Terminal:** the layout gives a bleeding child the columns of its padded ancestor's horizontal padding at both
  sides, through unpadded stacks, in `container_widgets.cpp`'s placement, as `alignSelf` is honoured there.
- **`RecordingBackend`:** records `bleed` in the layout string.

**Spec.**

- `document.md`, "Every node" (`bleed`) and the load-time rules;
- `view_tree.md` (`LayoutHints::bleed`);
- `backend_contract.md` ("Layout hints");
- `author.md` (`bleed()`);
- `qt_quick/backend.md` and `tui/frontend.md`, the Layout rows.

**Tests.**

- **Layout case** (`testing/backend_conformance.hpp`) "a bleeding child spans its padded column's width while a
  sibling stays inset": a column with padding `[0, 4]` holding an unpadded column, which holds two texts, the second
  with `bleed`. The second's left is the outer column's left and its width the outer column's. The first's left is one
  step-4 inset in.
  - **Control:** a backend that ignores `bleed`.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a panel with `padding: 2` holding a table with `bleed`. The table's
  header background runs from the frame's inner left edge to its inner right edge.
  - **Control:** the same, without the key.
- Corpus: `bleed: true` loads on a column's child, a panel's child and a tab's child. It is refused on the root and on a
  row's child.

**Gallery** (screen fixes): receipt's session table (G-k) and review's compact flagged rows (G-f).

**Verify.** Re-grab receipt at 1000: the table's header band reaches the card's edges, and the title and the footnote
stay in. Re-grab review at 390: the flagged rows run from x = 0 to 390.

## G-g: the compact head's surface, and an icon-only action

**Problem (read; the grab shows it).**

- **The surface.** `10` compact draws the shell's top bar dark (`#1C1E21`, white text, a white ☰ and a white scan
  icon). `10` expanded draws it white. `ShellFrame` sets `_topBar->setSurface("toolbar")` once, so a theme can only
  restyle both forms together.
- **The action.** The compact Scan is an icon alone. A head action (`actionButton`) always draws its label, so the
  grab's Scan is a framed "⛶ Scan" 82 px wide.

**Decision.**

- **`ShellFrame` picks the top bar's surface by its form**, in one function `showTopBarSurface()`, which `setCompact`
  and `setHeadJoined` (G-n) call:
  - `compactToolbar` while compact;
  - otherwise `toolbarJoined` while joined;
  - otherwise `toolbar`.
  - Theme switches and surfaces describe both new surfaces.
- **`iconOnly` on an `actionButton` part**, a condition. While true, the button draws its icon alone, and its label
  is its accessible name and its tooltip.
  - A part with `iconOnly` and no `icon` is refused at load (`` an icon-only action names its `icon` ``).
  - The part is the same in a screen's head and in a dialog's or drawer's footer, so both take it:
    - `ActionModel` gains `Prop<bool> iconOnly`, which `lowerActionParts` lowers;
    - `actionBarOf` lowers it for a footer;
    - `HeadAction` gains `bool iconOnly`;
    - `buildHeadTrail`'s button binds its label to `""` while it holds, and its `a11y.name` and `tooltip` to the
      label.
  - Author: `ActionButtonOptions::iconOnly`.
- Under `shape.iconOnly: square`, an icon-only ghost action is R1's glyph-sized square.

**Per backend.**

- **Qt Quick:** `Theme.qml` gains the `compactToolbar` scheme, bound to the toolbar's, and the `toolbarJoined` case,
  answering `barScheme`. `draws` accepts both, and `MorphWidget` draws its border for `compactToolbar` as for
  `toolbar`.
- **Terminal:** `invertsSurface` adds both names. An icon-only action whose icon has no glyph draws its accessible name
  (G-m), so it is never blank.

**Spec.**

- `theme.md` (the Surfaces table);
- `shell_frame.md` (the top bar's surface by form);
- `document.md`, the `actionButton` table (`iconOnly`);
- `workspace.md` (the trail's icon-only buttons);
- `author.md`;
- `qt_quick/backend.md` and `tui/frontend.md` (surfaces).

**Tests.**

- `tests/test_ui_shell_frame.cpp`:
  - a new frame's top bar is recorded `surface=toolbar`, `setCompact(true)` makes it `compactToolbar`, and
    `setCompact(false)` makes it `toolbar` again;
  - **Control:** set the surface once.
- `tests/qt_quick/test_qt_quick_theme.cpp`: a theme index whose `compactToolbar` surface sets `color.surface:
  #1C1E21` reaches `Theme.scheme("compactToolbar").surface`, and leaves `toolbar`'s as it was.
  - **Control:** answer the toolbar's scheme for both names.
- `tests/test_ui_workspace.cpp`: an action with `iconOnly: true`, `icon: "scan"` and label "Scan" is recorded as a
  button with an empty label, the accessible name "Scan" and the tooltip "Scan".
  - **Control:** ignore the key.
- Corpus: `iconOnly` with an `icon` loads, and without one is refused.

**Gallery** (lane I-S):

- the bundle's compact toolbar surface;
- console's compact Scan: `emphasis: ghost`, `iconOnly: true`. It is compact-only already.

**Verify.** Re-grab console at 390: a dark head with a white ☰, title and scan icon. Compare with `10` compact.

## G-h: the sidebar at 1000 px

**Problem (reproduced, probe 1; read).**

- **The mockups.** Every mockup is drawn at two widths: expanded 1000 px, compact 390 px. `10` and `Main` draw the
  expanded shell with a permanent sidebar, 176 px wide, beside an 824 px page. Nothing is drawn between the two
  widths.
- **The shell.** `src/client/shell.cpp`'s `compactFrame` is compact below `expanded`, with this reason: "with its
  sidebar, an 80-unit `medium` window would leave the page narrower than any `compact` one".
- **The classes.** `ui::widthClassOf` puts `expanded` at 160 units. With the gallery's unit of 7.67 px that is 1228 px
  (probe 1), so at 1000 px (130 units) the frame is compact. Every 1000 px grab therefore shows ☰ and no sidebar, which
  also hides H5's navigator overlines.

**What the mockups imply.** The designs draw the sidebar at 1000 px, and treat everything that is not a phone as
expanded. The gallery's `byWidth` already gives `medium` the expanded layout.

**The options.**

1. **Move `expanded` down to 108 units.**
   - 108 is the compact bound, 80, plus the sidebar's 28 (`kShellSidebarWidth`): the narrowest window whose page
     beside the sidebar is not compact. So the reason `compactFrame` gives still holds, with no frame-specific
     threshold.
   - At the default unit that is about 830 to 865 px, close to Material's `expanded` at 840 dp. The compact bound, 80
     units, is about 614 px, close to Material's 600.
   - It changes every document's `expanded` class and every terminal's: 108 columns, not 160.
2. **Keep 160 units, and grab the gallery at 1228 px or wider.** No framework change, but the gallery is then compared
   with its mockups at a width they do not draw.
3. **A frame threshold of its own**, such as the sidebar from 108 units, with the classes unchanged. It needs the
   backend to report its width in units, or a fourth class: a new pure virtual and listener on every backend, for one
   consumer.

**Recommendation: 1.** The constant becomes `kExpandedWidthUnits = 108` in `backend.hpp`, beside `widthClassOf`.
`shell_frame.hpp` gets a `static_assert` that `kShellSidebarWidth + 80 <= kExpandedWidthUnits`, so a wider sidebar
cannot quietly break the rule. `compactFrame` keeps its reason. Touch targets and sheets follow `compact` only, so
they do not change.

**If the user chooses 1:**

- `backend.hpp`'s `widthClassOf` and its doc;
- `expressions.md` and `backend_contract.md` ("`medium` below 108");
- `tests/test_ui_recording_backend.cpp`'s bounds: 107.5 is `medium`, 108 is `expanded`;
- **Control:** the old bound, and the `108` check fails.
- The gallery's tests that assume a medium window (any at 80 to 159 units that expects ☰) are checked by the lane.
- **Verify:** re-grab every screen at 1000×830. The sidebar shows with the navigator's overlines, and the page is
  1000 − 28 units wide.

**Residual.** The sidebar is 28 units (215 px at 7.67 px) against the mockups' 176 px. That is a framework constant,
not a gap the review listed, so it does not change here.

## G-i: the subtitle after the title

Closed by G-a's `leadPlacement: inline`. Plate takes it on expanded.

## G-j: collapsible sections without their marker

**Problem (read; the grabs show it).** `03`, `06`, `07` and `10` compact draw collapsed and open sections as cards
with a strong title and no chevron. The grabs draw `▸`/`▾` at the header's trailing edge (`MorphSectionHeader`,
`chevron` and `chevronGlyph`).

**Decision.** The theme switch `collapsible.marker` (Theme switches and surfaces):

- With `none`, `MorphSectionHeader` draws no chevron, and the header keeps everything else: it is still the whole
  row's toggle, at least a touch target tall, with its role, its name and its expanded state.
- The design's cards read as tappable by their shape. The state stays where assistive technology reads it.

**Per backend.** Qt Quick: `Theme.collapsibleMarker` (`true` by default), which hides both chevron items. Terminal:
unchanged (`▸ `/`▾ `).

**Spec.** `theme.md` (the closed set, the On Qt Quick table); `qt_quick/backend.md` (Collapsible, Panel).

**Tests.**

- `tests/test_ui_theme_values.cpp`: `collapsible.marker: none` reads `none`, and `hidden` is refused at `addFile`.
  - **Control:** accept any string.
- `tests/qt_quick/test_qt_quick_theme.cpp`: the value reaches `Theme.collapsibleMarker`.
  - **Control:** leave the path out of `kThemeTokens`.
- `tests/qt_quick/test_qt_quick_containers.cpp`: under `none`, a collapsible's `chevron` and `chevronGlyph` are
  hidden, and a click on its header still toggles it.
  - **Control:** ignore the token.

**Verify.** Re-grab stability, plate, instrument and console at 390.

## G-l: check boxes at the design's size and spacing

**Problem (reproduced in the grabs; read).**

- **Expanded.** `02`'s and `04`'s check boxes are 16 px, in rows 22 px apart; the grabs' rows are 29 to 33 px apart.
  `MorphCheckbox` keeps the style's `topPadding` and `bottomPadding` (6 px each, probe 4) around a box of
  `Theme.unitHeight` (16.5 px).
- **Compact.** `02` and `05` draw a 22 px box in a 44 px row. The grab's box is 16 px, since the indicator does not
  grow with the row.

**Decision.**

- **A check box has no vertical padding of its own.** It is as tall as its label's line or its box, and the
  container's `gap` spaces the rows. On compact it is at least the target height, as today.
- **On compact the box is half the touch height:** `Theme.touchHeight / 2`, 22 px. Elsewhere it is
  `Theme.unitHeight`. It is a rule from the existing token. The table's select column draws the same indicator, so its
  boxes grow on compact too.

**Per backend.** Qt Quick: `MorphCheckbox` (`topPadding: 0`, `bottomPadding: 0`) and `MorphCheckIndicator`
(`implicitWidth` and `implicitHeight` follow `Theme.touchTargets`). Terminal: unchanged (`[x]`).

**Spec.** `qt_quick/backend.md` (Checkbox).

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`:

- a check box is `Math.max(label line, Theme.unitHeight)` tall off compact;
- with `touchTargets` its `boxIndicator` is `Theme.touchHeight / 2` square;
- **Control:** today's padding (+12 px) and indicator (`unitHeight`).

**Verify.** Re-grab receipt and audit at 1000 (rows about 21 px apart with `gap: 1`) and samples at 390 (22 px
boxes).

## G-m: a pager's glyphs, and a button that is never blank

**Problem (reproduced, crop of both pagers).** Audit's `‹` `›` are about 2×3 px of ink in their squares. `04`'s are
about 5×8, drawn in Inter. The platform font's single guillemets are small at any size: H's probe 1 measured `‹` at
13 px as 3–4 × 7 px. The size of the glyph is the font's, so text cannot give it.

**Wider than listed.** A push button with an `icon` its theme's set does not name and no label draws nothing on
Qt Quick (`theme.md`: "a button … nothing"). `MorphMenu`'s trigger falls back to a glyph and then to its accessible
name. The terminal's button draws a framework glyph, or nothing.

**Decision.**

- **Built-in chevrons.** The framework names `nav.back` and `nav.forward` (in `kFrameworkIcons`; no built-in component
  draws them yet) are drawn by Qt Quick as a vector chevron when the set does not name them.
  - The new `MorphChevron.qml` is two antialiased 1.5 px strokes at ±45°.
  - Its height is `Math.round(0.6 × Theme.bodyPixels)`, 8 px at 13 px. It is in the caption's colour, and its
    direction follows the icon name. It is not mirrored under right-to-left, since the name already says which way.
- **An icon-only push button is never blank.** With no label, it draws, in order:
  1. the set's icon;
  2. a built-in chevron;
  3. a framework name's glyph (`iconGlyph`);
  4. its accessible name.

  This is the order `MorphMenu`'s `triggerText` follows. On the terminal it is the glyph, else the accessible name.
- **Pagers** are written `button({.icon = "nav.back", .shape = ControlShape::Square}).a11y("Previous page")`. The
  label goes, and the name stays.

**Per backend.**

- **Qt Quick:** `MorphPlainContent` gains `iconName`, set by `MorphButton`. It draws `MorphChevron` for those two names
  when the set lacks them, and otherwise the glyph or the name, as above. `MorphChevron.qml` joins the module's QML
  list in `src/qt_quick/CMakeLists.txt`.
- **Terminal:** `ButtonImpl` (`leaf_widgets.cpp`). With an empty label and no glyph, it draws the accessible name.

**Spec.** `theme.md` (Framework names: `nav.back` and `nav.forward` are drawn as chevrons; the button's fallbacks);
`qt_quick/backend.md` and `tui/frontend.md` (Button).

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a square button with `icon: nav.back` and no set shows a visible `MorphChevron` at least `0.55 ×
    Theme.bodyPixels` tall;
  - a button with `icon: scan`, no label, no set and the name "Scan" shows the text "Scan";
  - **Control:** today's content item, which draws neither.
- `tests/tui/test_tui_widgets.cpp`: the second button draws `[ Scan ]`.
  - **Control:** `[  ]`.

**Gallery.** Audit's and samples' pagers (lane I-V), and console's sample pager (lane I-S).

**Verify.** Re-grab audit at 1000. The chevrons are about 5×8 px. Compare with `04`.

## G-n: a page that continues the head's block

**Problem (read; the grabs show it).**

- `03` (instrument's strip), `05` compact (samples' search, Filters and Sort), `06` (stability's stepper, at both
  widths), `08` compact (results' pager) and `09` compact (review's tabs) continue the head's white block with no rule
  between the head and what follows. Each draws its rule under the whole block.
- **The cause.** The shell's top bar is on `toolbar`, which `MorphWidget` borders (`border.width: root.surface ===
  "toolbar" || …`).
- **H kept the rule** because dropping it for every pinned page would also drop the rule that `02` compact draws under
  its head, where nothing continues the block.
- **Given G-c**, the strip is 32 px of white, and the 1 px rule above it cuts the block the mockups draw whole.

**Decision.** The page says it continues the head, and the shell drops its rule for it.

- **`headJoined`, a literal flag on `column` and `row`.** The stack's first child continues the head's block:
  - as the shell's shown root (seen through `switch`es, as `pageEdge` is), the shell draws its top bar on
    `toolbarJoined`, which has no rule;
  - the stack's first child is then responsible for the rule under the block. `pinned`'s divided column draws it
    under its top bar, and a `tabs` strip draws its own.
- **It needs `pageEdge: true`** on the same stack, since an inset page has a grey margin between it and the head. It is
  refused without it at load (`` `headJoined` needs `pageEdge` ``).
- **`pinned` writes it when it has a top bar** and an inset. Stability, samples' and results' compact layouts then
  need no key.
- **Following it.**
  - `Screen::headJoined()` is a tracked read, which shares `pageEdge()`'s walk through `switch`es, factored into one
    `shownRoot()`.
  - `ScreenHead::headJoined()` is a virtual with a default of `nullptr`.
  - `Workspace::activeHeadJoined()` is `!activePageInset() && headJoined`. So a strip of two tabs, which insets the
    page, keeps the rule.
  - `ShellFrame::setHeadJoined(bool)` calls `showTopBarSurface()` (G-g).
- **Compact keeps `compactToolbar`**, joined or not. The gallery's compact head is dark, so no rule shows against the
  white block. In the default theme a compact joined page keeps its rule (residual).

**Per backend.** As G-g: Qt Quick's `toolbarJoined` scheme, unbordered; the terminal inverts it as `toolbar`, so it
keeps its look.

**Spec.**

- `document.md`, the `column, row` row (`headJoined`) and "A bar that stays in view" (`pinned` writes it);
- `view_tree.md` is unchanged: the flag is a document key the interpreter reads, as `pageEdge` is;
- `interpreter.md` (`Screen::headJoined`);
- `host.md`; `workspace.md` (`activeHeadJoined`);
- `shell_frame.md` (`setHeadJoined`, the top bar's surfaces);
- `author.md` (`ContainerOptions::headJoined`, `pinned`).

**Tests.**

- `tests/test_ui_author.cpp`:
  - `pinned(content, {.top = …})` writes `headJoined: true`, and one with no top bar or with `.inset = 0` writes none;
  - **Control:** today's helper.
- Corpus: `headJoined` with `pageEdge` loads; without it, or computed, it is refused.
- `tests/test_ui_host.cpp`: `headJoined()` reads through a `byWidth` switch to a pinned page with a top bar, and
  answers `false` for its other case.
  - **Control:** read the root only.
- `tests/test_ui_workspace.cpp`: `activeHeadJoined()` is `true` for that screen alone and `false` once a second tab
  opens.
  - **Control:** ignore the strip.
- `tests/test_ui_shell_frame.cpp`: `setHeadJoined(true)` records the top bar's `surface=toolbarJoined`, and after
  `setCompact(true)` `compactToolbar`.
  - **Control:** a no-op.
- `tests/client/test_shell.cpp`: opening a pinned page with a top bar sets the joined surface, and switching to a
  plain screen restores `toolbar`.
  - **Control:** drop the effect.

**Gallery.**

- instrument's root and plate's compact root take `headJoined: true` (`pageEdge` already, or in the same fix);
- review's and audit's compact roots too (Screen fixes);
- stability, samples' compact and results' compact get it through `pinned`.

**Verify.** Re-grab instrument and stability at 1000, and samples, results and plate at 390. No rule shows between
the head and the block that continues it, and one shows under the block.

**Residual.** In the default theme a compact joined page keeps the rule under the head.

## G-o: step connectors stand off the steps

**Problem (read; the grab shows it).** `06` leaves about 10 px between a step's label and the connector, and between
the connector and the next circle. `MorphSteps`' connector `Rectangle` fills its cell (`anchors.left/right:
parent.left/right`).

**Decision.** A connector is inset by `Theme.spaceOf(2)` at each end, and `minimumConnector` grows by twice that. It
is a rule, at both widths.

**Per backend.** Qt Quick: `MorphSteps`. Terminal: unchanged. Labelled steps are one per row, and unlabelled marks are
three cells wide with the connector after.

**Spec.** `qt_quick/backend.md` (Steps).

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`: in a 600 px labelled steps node, each `connector`'s left edge
is at least `Theme.spaceOf(2)` right of the preceding `label`'s right edge.

- **Control:** today's, at 0.

**Verify.** Re-grab stability at 1000.

## G-p: the splitter's handle

**Problem (read; the grab shows it).**

- **The design.** `09`'s handle is `width: 6px; background: #E1E5EA`, with a centred 2×28 px grip in `#9AA2AE`.
- **Today.** `MorphSplitterHandle` is the Basic style's: a 6 px bar in `palette.button`, with no grip, in colours
  that ignore the theme.

**Decision.**

- The handle is drawn in the splitter's scheme:
  - at rest, the bar is `scheme.border`;
  - hovered or pressed, it is `scheme.controlBorder`.
- A grip is centred on it, 2 px across the axis by `Math.min(28, length / 3)` along it, radius 1, in
  `scheme.neutralDot`, so the handle reads as something to drag.
- The handle stays 6 px thick. It is the `splitterHandle` part a restyle replaces, as today.

**Per backend.** Qt Quick only. The terminal's handle stays a rule of `│` or `─`.

**Spec.** `qt_quick/backend.md` (Splitter).

**Tests.** `tests/qt_quick/test_qt_quick_containers.cpp`: a horizontal splitter's handle has a visible child named
`grip`, 2 px wide and centred on the handle, and the handle's colour is `scheme.border`.

- **Control:** the Basic handle.

**Verify.** Re-grab review at 1000. Compare with `09`.

## G-q: a divided row

**Problem (found here; read).** Audit's expanded fix (`04`) puts the facets as a white pane flush to the page's edge,
with a 1 px rule between it and the trail. The rule between children is `divided`, which the loader refuses on a
`row` (`document.md`: "the loader refuses it on a `row`").

**Decision.** `divided` is allowed on a `row` that does not `wrap`. It draws a vertical 1 px rule in the divider colour
between each two shown children, centred in the gap and taking 1 px of its own, as a column's does. With `wrap` it is
still refused, since a rule between lines and within them would be two rules.

**Per backend.**

- **Qt Quick:** `MorphStack`'s divider `Repeater` (`model: root.divided && root.axis === 0 && !root.wrap ? … : []`)
  takes both axes. A row's divider is 1 px wide and the line's height.
- **Terminal:** draws none, as for a column.
- **The mount** already calls `setDivided` on any stack that gives the key.

**Spec.** `document.md`, the `column, row` row; `author.md` (`ContainerOptions::divided`).

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`: a row of two columns with `divided: true` shows one divider, 1 px
  wide, between them.
  - **Control:** today's `MorphStack`.
- Corpus: `divided` loads on a row, and is refused on a row with `wrap: true`.

## G-r: a banner's action, as a link or a button

**Problem (found here; read).**

- **The designs draw a banner's action three ways:**
  - stability's attention banner links the field's name inline at the end of its sentence (`06`: "1 field needs
    attention: <u>Containers per pull</u>.");
  - the console's alert has a framed "Open run" button at the trailing edge (`10` expanded);
  - on compact, the same is a full-width button under the text, inside the alert (`10` compact).
- **Today.** `MorphBanner` draws its action as a ghost button at the trailing edge (the label `fillWidth`), and the
  terminal draws `[label]` after the text.

**Decision.** The `action` part of a `banner` takes two more keys:

- **`style`**, a literal name:
  - `ghost`: the default, today's;
  - `button`: a framed normal button;
  - `link`: the label in the accent, underlined, right after the text, wrapping with it.
- **`stretch`**, a literal flag: the action takes a line of its own under the text, as wide as the banner. It is
  refused with `link`.
- **Only the banner takes them.** An `emptyState`'s `action` (the same part) refuses both: its action is its one
  button.

In the view tree, `ActionButton` gains `BannerActionStyle style` and `bool stretch`, appended. `BannerWidget` gains
`setActionStyle(BannerActionStyle, bool stretch)`, a pure virtual the mount calls once, only when the document gives
either key. Author: `ActionPart::style` and `stretch`.

**Accessibility.** A `link` is still a button to assistive technology (it runs commands, not a navigation), named by
its label.

**Per backend.**

- **Qt Quick:** `MorphBanner`.
  - A `link`: the message's label stops filling the width, the action follows it, and the close button keeps the
    trailing edge.
  - A `button`: the action's background is `MorphButtonBackground` framed, not ghost.
  - `stretch`: the action moves to a second row with `Layout.fillWidth`.
- **Terminal:**
  - `link`: the label underlined in the accent, after the text;
  - `button`: `[ label ]`;
  - `stretch`: on its own row.
- **`RecordingBackend`:** records `actionStyle=link` and `stretch=true`.

**Spec.** `document.md` (the `banner` row, the `emptyState` row); `view_tree.md` (Banner); `backend_contract.md`
(`BannerWidget`); `author.md`; `qt_quick/backend.md` and `tui/frontend.md` (Banner).

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a `link` action's `x` is the text's right edge plus the row's spacing, and its label is underlined;
  - a `stretch` `button` action is as wide as the banner's content and below the text;
  - **Control:** ignore the style.
- `tests/tui/test_tui_display.cpp`: a `button` banner draws `[ Open run ]`.
  - **Control:** `[Open run]`.
- Corpus:
  - `style: "link"` loads on a banner's action;
  - `stretch` beside `link` is refused;
  - both are refused on an `emptyState`'s action.

## R3 and the screen fixes

Each fix lands in the lane that owns its screen, after the items it uses (Lanes). Where a fix uses no new
vocabulary, it says so.

### E-signature (`esignature.cpp`, lane I-Q)

- **The refusal** (medium, compact). The banner carries the whole sentence: `refusedBanner` drops the compact branch,
  so "Password incorrect." is its strong run at both widths. The password field keeps `tone: err` and drops its
  compact `.errors` at both widths: the field is outlined, the banner says why.
- **"Signing at…"** takes `role: small`.
- **The compact summary's** record ID is a strong mono run: `ui::styled(sign.record, {.strong = true, .mono =
  true})`.
- The sheet's head and the rules come from G-d and G-e, with no screen code.

### Receipt (`receipt.cpp`, lane I-S)

- `leadPlacement: end` at both widths (G-a).
- **The sample grid** (`sampleCard`) takes `align: start`, so "Temperature on arrival", with its description under
  it, does not push its row's other fields down (medium).
- **"This session"** keeps `padding: 2`, and the table takes `.bleed()` (G-k).
- **Compact Reject** is `sizing(ui::fixed(14))` (110 px at 7.67 px).

### Stability (`stability.cpp`, lane I-V)

- **The three field columns** (`expandedConditions`' grid) take `align: start` (medium).
- **The timepoint columns** (`matrix`) take `.align = Align::Center`, so each header is over its box.
- **"Continue to tests"** moves from step 0 to step 1 (`wizard`), the step whose next one is the tests.
- **The attention banner** (`attention`) reads "1 field needs attention:", with the action `{.label = "Containers per
  pull", .style = link}` (G-r).
- **Compact's per-pull error** spans both fields:
  - `perPullField(study, true)` drops its `.errors` and takes `.tone(err)` while the stock is short;
  - an `error` text under the row carries the message;
  - the attention banner still names the field.
- The sections' chevrons go through the theme (G-j).

### Plate (`plate.cpp`, lane I-S)

- `leadPlacement`: `inline` on expanded, `below` on compact (G-a, G-i).
- **The compact subtitle** reads "ELISA, host cell protein · 14 samples to place". The muted "14 samples to place"
  line goes.
- **Compact's Auto-fill and Save layout** sit in the head's white block (medium). The compact root is a `column` with
  `pageEdge: true` and `headJoined: true`: the button row in a `bar` column padded `{"lit": [2, 4]}`, then the rows'
  sections in a column padded 4.
- **The selection bar's** "6 wells selected · A3–A8" bolds the count alone: `selectionText` becomes `spans`, with
  `ui::strong` on "6 wells selected" and the range plain, and the text's role `normal`.
- **"To place"**: the `forEach` takes `divided: true`, and "+ 9 more" takes `alignSelf: center`.
- **Compact well rows** read "**A1** · Standard 1, 100 ng/mL" as one text. `runLine` replaces the fixed-width name and
  the line with one text whose `spans` are the strong name (or range), " · " and the line.

### Results (`results.cpp`, lane I-S)

- `leadPlacement: end` on expanded. "In progress" is hidden on compact (R2, G-a).
- **Compact's head** (medium):
  - `titleSpans` is `cond(compact, spans({"Sample ", n, " of 8 · ", mono(sample)}), [])`. The title stays "Assay and
    impurities · Paracetamol 500 mg" as the tab's caption;
  - ‹ Previous and Next › move into `pinned`'s top bar, which joins the head (G-n);
  - the in-page "Sample 2 of 8" heading goes.
- **Compact's bottom bar** holds the out-of-specification line above the buttons: `.bottom = {oosText, row{save,
  submit}}`.
- **The expanded table** gains a leading `Sample` column, `fixed(11)` with an empty cell, as `08` draws. Each group
  heading is strong, the key as a strong mono run.

### Review (`review.cpp`, lane I-S)

- **R3.** `queueEntry`'s content column takes `padding: ui::padding(1, 0)`, so an entry's text is 12 px from its
  rules, as `09`'s.
- **The queue entry's first line** is a row: the strong mono ID, then the age, muted, right-aligned (`justify:
  between`).
- **The sheet line** (`sheetHead`) takes `spans` with the worksheet ID as a mono run.
- **The result table's** `result` column is strong.
- **The compact worksheet** (medium):
  - its root is a `column` with `pageEdge: true` and `headJoined: true`;
  - the screen's `eyebrow`, `titleSpans` and four `status` parts take the sheet line, the worksheet title and the
    summary tags on compact (each tag a `status` part reading `pick(review, "compactSummary").at(i)`, visible while
    engaged);
  - the back link sits in a `bar` column padded `{"lit": [1, 4]}`;
  - the record tabs take `inset: 4` and `stripSurface: "bar"`, with each page in a `scroll`;
  - the decisions sit in a `bar` row padded `{"lit": [2, 4]}`, divided from the tabs.
- **Compact's flagged rows** are a divided `forEach` with `.bleed()` (G-f). Each row is a `column` padded `{"lit": [2,
  4]}` on the `outOfSpec` surface while flagged, in place of the padded panels.
- **Compact's All/Flagged** control drops its "Show" caption and stretches.
- The splitter's grip comes from G-p.

### Audit (`audit.cpp`, lane I-V)

- **Expanded** (medium). The root is a `row` with `pageEdge: true`, `divided: true` (G-q), the page's height and
  `align: stretch`:
  - the facets in a `column` on `bar`, padded `{"lit": [3, 3]}`, `fixed(29)`;
  - then the chips and the trail in a `column` padded 4.
- **Compact** (medium). The root is a `column` on `bar` with `pageEdge: true` and `headJoined: true`:
  - Filters, Export CSV and the count in a column padded `{"lit": [2, 4]}`;
  - the entries a divided `forEach`, each entry padded `{"lit": [3, 4]}`;
  - Load 25 more padded 4.
- **The pager** uses `nav.back` and `nav.forward` (G-m).

### Samples (`samples.cpp`, lane I-V)

- **The expanded bulk bar** (medium). Its row takes `padding: ui::padding(0, 2)` inside the panel's step 1, so its text
  is 12 px from the border and its buttons 4 px from the top.
- **The compact search, Filters and Sort** (medium) move into `pinned`'s `top`, which joins the head (G-n).
- **"·"** precedes "Select all 312": the selected text's spans end with " ·".
- **The pager** uses `nav.back` and `nav.forward` (G-m).

### Console (`console.cpp`, lane I-S)

- `leadPlacement: end`. "Connected" is hidden on compact, and the compact Scan is `ghost` and `iconOnly` (G-a, G-g).
- **The alert's** "Open run" takes `style: button`, and on compact also `stretch: true` (G-r).
- **"The 6 most urgent of 38."** goes. The "On hold" note stays.
- **The two bottom panes** are one card. The splitter sits in a `panel` with `padding: 0`, and each pane is a column
  padded 2 in place of its own panel.
- **The sample pager** uses `nav.back` and `nav.forward` (G-m).

### Instrument (`instrument.cpp`, lane I-V)

- The root takes `headJoined: true` at both widths (G-n). The strip's height comes from G-c.

### The gallery's bundle and app document (`lims_gallery.cpp`, lane I-S)

- `collapsible.marker: none`;
- the `compactToolbar` surface file;
- `principalPlacement: "head"`.

## Totals

**New document keys and members:**

- the screen's `leadPlacement`; the app document's `principalPlacement`;
- every node's `bleed` (a child of a `column`, a `panel` or a tab);
- `column` and `row`: `headJoined`; `divided` on a non-wrapping `row`;
- an `actionButton` part's `iconOnly`;
- a banner action's `style` and `stretch`.

**Theme:**

- the keyword token `collapsible.marker` and `Theme.collapsibleMarker`;
- the built-in surfaces `compactToolbar` and `toolbarJoined`.

**New pure virtuals**, each with every implementation (Qt Quick, terminal, `RecordingBackend`): one,
`BannerWidget::setActionStyle(BannerActionStyle, bool)`.

**Other API:**

- `LayoutHints::bleed`; `ActionButton::style` and `stretch`; `BannerActionStyle`;
- `LeadPlacement`, `PrincipalPlacement`;
- `ScreenHead::leadPlacement()` and `headJoined()`, each with a default;
- `Screen::leadPlacement()`, `Screen::headJoined()`, `AppRoot::principalPlacement()`;
- `HeadAction::iconOnly`, `ActionModel::iconOnly`;
- `Workspace`: `HeadParts::inlineLead`, `activeHeadOneLine()`, `activeHeadJoined()`;
- `ShellFrame`: `headInlineSlot()`, `setLeadInline`, `setHeadOneLine`, `setHeadJoined`, `setPrincipalInHead`;
- `kExpandedWidthUnits`, if the user takes G-h's recommendation;
- Author:
  - `Node::bleed()`;
  - `ContainerOptions::headJoined`;
  - `ActionButtonOptions::iconOnly`;
  - `ActionPart::style` and `stretch`;
  - `Scope::leadPlacement`.

**Conformance:** one new layout case, G-f's bleeding child.

**QML:** one new file, `MorphChevron.qml`.

**Toolchain notes for every lane.**

- New members go at the end of the view-tree structs, so designated initializers keep their order on MSVC.
- `ShellFrame`'s new members keep declaration order equal to creation order (`-Wreorder`).
- No parameter is named after a member or member function beside it (GCC 15 `-Wshadow`).
- No identifier in `include/morph` is `slots`, `signals` or `emit`.
- Every QML property used here exists on Qt 6.8, and `qmllint` runs over the changed files.
- A new public symbol carries full `@param`/`@return` docs, checked through a translation unit that includes it under
  clang.

## Not in this batch

- **The fonts.** Inter and JetBrains Mono are not installed, so glyph widths, the mono face and receipt's "Sample S-…"
  heading differ. The design's 14 px is between the theme's 13 px `section` and 16 px `heading` steps, and a new type
  step for one heading is not a gap.
- **Tabs and controls at 38 and 30 px.** Every control is the control height (G8).
- **The sidebar's width**, 215 px against 176 (G-h, residual).
- **Audit's head over the main column only.** `04` draws the facets from the window's top, beside the head. The shell's
  head spans the window on every screen.
- **Review's compact back link above the eyebrow.** The head has no place for a control above its eyebrow, so the link
  sits under the badges, above the tabs. No other mockup puts a control there.
- **The console's lab select at the title's side** (G-a, residual).
- **The default theme's compact joined pages** keep the rule under the head (G-n, residual).

## Decisions for the user

1. **G-h: where `expanded` begins.**
   - *Recommended:* 108 units (the compact bound plus the sidebar), which shows the sidebar at the mockups' 1000 px
     and keeps the page beside it at least `medium`. It moves every document's and every terminal's `expanded` bound
     from 160 to 108.
   - *Alternatives:* keep 160 and grab the gallery at 1228 px or wider; or a frame-only threshold, which needs a new
     backend width report.
2. **G-c: tabs at the control height in every application.**
   - *Recommended:* a rule. A tab is a control, and the strip's 20 px was below every other control. It reads the
     existing `control.height`, so a theme still sets it.
   - *Alternative:* a theme switch (`tabs.height: label | control`), off by default, as H made the square triggers.
     The batch rule "a look that changes every application's shell is the theme's to switch" argues for it, because
     the workspace's own strip grows by 12 px too.

## Taken at this design's defaults

- **R1:** an unframed square draws at 32 px and keeps a 44 px pointer target (WCAG 2.5.5, probe 3); a framed square
  keeps the target height.
- **G-a:**
  - one `leadPlacement` for the subtitle and the badges together, with the `end` lead between the tools and the
    actions;
  - the principal's place is the app document's, default `sidebar`.
- **G-b:** a one-line head is centred; a taller one keeps its tops aligned.
- **G-d, G-e, G-l, G-o, G-p:** rules for every application, since each matches the conventional look the defaults
  already aim at.
- **G-f and G-k are one key, `bleed`:** horizontal only, through unpadded stacks.
- **G-g and G-n:** the top bar's look is a surface per form. Compact stays `compactToolbar` when joined.
- **G-j:** a theme switch, off by default.
- **G-m:** vector chevrons for `nav.back` and `nav.forward`, and an icon-only button that falls back to its name.
- **G-r:** the action's look is the banner's only, and `emptyState` refuses it.

## Defects this batch fixes

- **An icon-only push button whose icon the theme does not name draws nothing** (G-m). Read: `MorphPlainContent`
  draws an `Image` only when its source is ready, and `theme.md` documents the blank. The terminal draws an empty
  bracket for a name with no glyph.
- **An inset tabs node puts 28 px, not its inset, between its strip and its page** (G-c). Reproduced in the grab
  (y = 97 to 124).

Neither is filed, since each is fixed by the change that touches it (AGENTS.md: "A fix smaller than the issue
describing it").

## Lanes

Three lanes, each in its own worktree, each owning the files it changes. Implementation runs on Sonnet. Each lane
wraps every build, test and clang-tidy run in `flock <scratchpad>/morph-build.lock` and builds with `-j10` at most.

### Framework files

| Lane | Items, in order | Owns |
|---|---|---|
| I-Q, Qt Quick component looks | G-j (theme keyword), G-l, G-o, G-p, G-c, G-d, G-e, G-m, R1 | `theme.hpp` (`kKeywords`, `kThemeTokens`); Qt Quick `theme.cpp`, `MorphButton.qml`, `MorphMenu.qml`, `MorphPlainContent.qml`, `MorphChevron.qml` (new), `MorphTabs.qml`, `MorphDialog.qml`, `MorphDrawer.qml`, `MorphSectionHeader.qml`, `MorphCheckbox.qml`, `MorphCheckIndicator.qml`, `MorphSteps.qml`, `MorphSplitterHandle.qml`, `src/qt_quick/CMakeLists.txt`; terminal `leaf_widgets.*` |
| I-V, vocabulary and containers | G-q, G-f/G-k (`bleed`), G-r, then G-n's key (`headJoined`, `pinned`) and the two surfaces' frontends (G-g, G-n) | `view.hpp`; `mount.hpp`; `testing/recording_backend.hpp`; `testing/backend_conformance.hpp`; Qt Quick `MorphWidget.qml`, `MorphStack.qml`, `MorphPanel.qml`, `MorphBanner.qml`, `widget.cpp`, `containers.cpp`, `leaves.cpp`, `kinds.hpp`; terminal `widget.cpp` (`invertsSurface`), `container_widgets.*`, `layout.*`, `display_widgets.*` (the banner) |
| I-S, shell and head | G-h (if taken), G-a (with R2 and G-i), G-b, G-g (frame surface, `iconOnly`), G-n (screen, head, workspace, frame, client) | `shell_frame.hpp`; `workspace.hpp`; `host.hpp`; `src/client/shell.cpp` |

`tests/ui_echoing_backend.hpp` forwards only the `create…` calls, so no lane changes it.

**Landing order: I-Q, then I-V, then I-S.** The lanes work in parallel. Each lands on `feat/lims-gallery` with one
commit per item, green on its own. I-V rebases onto I-Q, and I-S onto I-V, before landing.

- **I-V waits for nothing** from I-Q in its framework code. Its screens' pagers (G-m) are written with framework icon
  names that already exist, and are verified after I-Q lands.
- **I-S waits for I-V's:**
  - `headJoined` key, `ContainerOptions::headJoined` and `pinned`'s writing it, for `Screen::headJoined()` and its
    tests;
  - `compactToolbar` and `toolbarJoined` in `Theme.qml`, `MorphWidget` and the terminal, for the frame's surface tests
    on Qt Quick and the terminal;
  - `bleed` and the banner's `style` for its screens (receipt, review, console).

  I-S writes the code that depends on them after its rebase. Its headless tests (`test_ui_shell_frame`,
  `test_ui_workspace`) record surface names and need nothing from I-V.

### Shared files

Every file two lanes touch has a named region per lane. A later lane rebases onto the earlier lane's version. Within
a region, rows and cases are appended, never reordered.

| Shared file | I-Q's region | I-V's region | I-S's region |
|---|---|---|---|
| `Theme.qml` | `collapsibleMarker`, beside `iconOnlySquare` | the `compactToolbar` scheme, and the `toolbarJoined` and `compactToolbar` cases of `scheme()` and `draws()` | — |
| `document.hpp` | — | `kContainerKeys` (`headJoined`), the every-node keys (`bleed`) and their parent check, the banner action part (`style`, `stretch`), the `row`'s `divided` refusal | the `Document` struct and its top-level loading (`leadPlacement`, `principalPlacement`), the app document's refusal list, the `actionButton` part (`iconOnly`) |
| `interpreter.hpp` | — | the lowering of common layout (`bleed`), banners (`style`, `stretch`) and stacks (`divided`) | `LeadPlacement`, `PrincipalPlacement`, `ActionModel::iconOnly`, `lowerActionParts`, `actionBarOf`, `HeadAction`, `Screen::leadPlacement()`, `Screen::headJoined()` with `shownRoot()`, `AppRoot::principalPlacement()` |
| `author.hpp` | — | `ContainerOptions` (`headJoined`), `pinned`, `Node::bleed()`, `ActionPart` | `Scope::leadPlacement`, `ActionButtonOptions::iconOnly` |
| `backend.hpp` | — | `BannerWidget::setActionStyle`, `BannerActionStyle` | `widthClassOf`, `kExpandedWidthUnits` (G-h) |
| `docs/spec/ui/document.md` | the `tabs` row (G-c), the `dialog` row (G-e) | "Every node", the `column, row`, `banner` and `emptyState` rows, "A bar that stays in view", the load-time rules | "The document", "The head", the `actionButton` table |
| `docs/spec/ui/theme.md` | the closed set, the On Qt Quick table, Framework names | the Surfaces table | — |
| `docs/spec/ui/author.md` | — | containers, `pinned`, `bleed`, `ActionPart` | `Scope`, the app document, `ActionButtonOptions` |
| `docs/spec/qt_quick/backend.md`, `docs/spec/tui/frontend.md` | the Button, Menu, Tabs, Dialog, Drawer, Checkbox, Steps, Splitter, Collapsible and Panel rows | the Column, Row, Stack, Layout and Banner rows, and the surfaces | — |
| `tests/test_ui_document.cpp`, `tests/qt_quick/test_qt_quick_containers.cpp`, `tests/qt_quick/test_qt_quick_widgets.cpp`, `tests/qt_quick/test_qt_quick_theme.cpp` | new `TEST_CASE`s appended at the end | appended after I-Q's | appended after I-V's |
| `tests/ui/corpus/` | new entries only | new entries only | new entries only |

### Screens

Each screen file, and its test under `examples/lims_gallery/tests/`, has one owning lane. The owner makes every edit
on it after the items it uses have landed, which the landing order guarantees.

| File | Owner | Uses |
|---|---|---|
| `esignature.cpp` | I-Q | no new vocabulary (G-d and G-e need no screen code) |
| `stability.cpp` | I-V | G-r (the link), G-n through `pinned` |
| `audit.cpp` | I-V | G-q, G-n (`headJoined`), G-m (the pager) |
| `samples.cpp` | I-V | G-n through `pinned`, G-m (the pager) |
| `instrument.cpp` | I-V | G-n |
| `receipt.cpp` | I-S | G-a, G-k (`bleed`) |
| `results.cpp` | I-S | G-a, H4's `titleSpans`, G-n through `pinned` |
| `plate.cpp` | I-S | G-a (`inline`), G-n |
| `review.cpp` | I-S | R3, G-a's head members, G-f (`bleed`), G-n, H8's tabs keys |
| `console.cpp` | I-S | G-a, G-g (`iconOnly`), G-r, G-m (the pager) |
| `lims_gallery.cpp` | I-S | G-j's and G-g's theme entries, G-a's `principalPlacement` |

I-Q and I-V verify their items with uncommitted edits to screens they do not own, such as I-Q's
`collapsible.marker: none` in the bundle. I-S runs the final re-grab of all ten screens at both sizes after every
lane has landed, and records the result in `examples/lims_gallery/design/visual-review.md`.
