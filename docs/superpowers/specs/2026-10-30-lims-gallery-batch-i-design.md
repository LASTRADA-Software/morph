# LIMS gallery batch I: the head's line, the shell's surfaces, edges that bleed, and component looks

Batch I of the LIMS gallery visual review. A fresh comparison after batch H (all ten screens at 1000×830 and 390×844,
against the mockups and the pre-H grabs) found three regressions, sixteen framework gaps (G-a to G-p) and the screen
fixes that the vocabulary already expresses. This design closes all of them. Writing it and its review found more,
which it closes too:

- G-q, a `row` cannot be `divided`;
- G-r, a banner's action has one look and one place;
- part of G-m: an icon-only push button whose icon the theme does not name draws blank;
- part of G-g: a themed icon is drawn in the SVG's own colours, so it cannot follow its button's foreground;
- S3: a dialog's or drawer's footer action drops its `icon`;
- S10: a binding loop on `MorphText`'s `implicitWidth`, reported by Qt 6.8.1.

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

Three throwaway probes ran with `qml6` (Qt 6.12.0, offscreen) and are kept out of the tree (`lg-id-probe1.qml`,
`lg-id-probe2.qml` and `lg-id-probe4.qml` in the session scratchpad). Qt 6.8 was not run here; every property used
below predates Qt 6. A local Qt 6.8.1 build of the tree exists at `build/lg-qt681` for the lanes.

**1. The unit, offscreen.** The gallery's theme names no font family, so a unit is the platform font's average
character width:

```
PROBE platform family Sans Serif px 12 pt 9 avgW 7.671875 h 16.46875
PROBE 13px avgW 8.3125 h 17.84375 expanded at 1227.5 px; compact below 613.75
```

A 1000 px window is 130 units. H's probe 3 agrees: 32 units measured about 248 px there.

**2. A negative layout margin.** A rule in a `ColumnLayout` 200 px wide, given `Layout.leftMargin: -16` and
`Layout.rightMargin: -16`:

```
PROBE neg-margin rule x -16 w 232 | sibling x 0 w 200 | col w 200 implicitW 50
```

A layout honours negative margins, so a child can reach past its container's padding without a second layout pass.
G-d and G-f rely on this. The implicit width shrinks by the margins, so a bleeding child adds nothing to its
container's natural width (G-f states it).

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

(Probe 3, a pointer target larger than the drawn button, served a first R1 design that the review's correction of R1's
premise made moot. It is not relied on.)

## The items

| Item | Where it lands | Verdict on the finding |
|---|---|---|
| R1 | the compact top line's own padding and gap | **Premise corrected**: `10` compact's ☰ is a 44 px unframed square too; what differs is the bar's padding |
| R2 | closed by G-a (`leadPlacement: end`) | Right |
| R3 | screen fix: review's queue entry | Right |
| G-a | the screen's `leadPlacement` (`below`, `inline`, `end`); the app document's `principalPlacement` | Right. **Wider than listed:** the console's user name is the shell's principal |
| G-b | the shell's top line centres its items while the head is one line | Right (reproduced: 11 px off at 390) |
| G-c | a tab is the control height, everywhere; an inset strip puts no spacing over its page | Right (reproduced, probe 4). **Decided by the user:** a plain rule |
| G-d | a dialog's and a drawer's rules span the overlay | Right (probe 2) |
| G-e | a sheet's head centres its title and draws a rule under it | Right |
| G-f, G-k | one key, `bleed`; and `tabs`' `pageInset`, so a page can scroll its own padding | Right. They are one seam: the same need in two screens |
| G-g | the screen's `headSurface`; an action's `iconOnly`; icons tinted by `currentColor` | Right. The dark head is the console's own choice |
| G-h | `expanded` from 108 units | **Decided by the user** |
| G-i | `leadPlacement: inline` (G-a) | Right |
| G-j | the theme switch `collapsible.marker` | Right |
| G-l | a check box's own vertical padding goes; its box grows on compact | Right |
| G-m | built-in chevrons for `nav.back` and `nav.forward`; an icon-only button is never blank | Right. **Wider than listed:** an icon the theme does not name draws nothing today |
| G-n | the flag `headJoined`; the built-in surface `toolbarJoined` | Right: after G-c the rule cuts a 32 px white block, and the mockups draw none |
| G-o | a step connector stands off its neighbours by step 2 | Right |
| G-p | the splitter handle's colours and grip | Right |
| G-q | `divided` on a `row` | Found here: audit's expanded fix needs it |
| G-r | a banner's `actionStyle` and `actionStretch`; a link drawn in the message | Found here: console's and stability's fixes need it |
| S3 | a footer action keeps its `icon` | Found by the review (read) |
| S10 | `MorphText`'s `implicitWidth` binding loop on Qt 6.8.1 | Reported by the comparison; cause read, to be reproduced on `build/lg-qt681` |

## Theme switches and surfaces

One new look changes every application that uses collapsible sections, so it is a switch, off by default:

| Token | Values (default first) | What it switches | Read by |
|---|---|---|---|
| `collapsible.marker` | `shown`, `none` | Whether a collapsible section's header and a collapsible panel's header draw their ▸/▾ chevron (G-j) | `MorphSectionHeader` (through `Theme.collapsibleMarker`) |

- It is a `keyword` token, as `shape.iconOnly` is: `kKeywords` gains its entry and `kThemeTokens` grows from 67 to 68.
- Any other value is refused at `addFile` (`collapsible.marker: only shown and none are drawn`).
- The terminal reads no theme token and keeps its `▸ `/`▾ ` heading.

One built-in surface joins the five (G-n):

| Surface | Background | Colour roles | The shell draws it |
|---|---|---|---|
| `toolbarJoined` | the `bar`'s: `color.surface`, no border | the `bar`'s, through its own `MorphScheme` bound role by role to `barScheme`'s | as its top bar while the shown page continues the head's block, on both forms |

- It has its own scheme object, so a theme index that declares a `toolbarJoined` surface replaces that object's roles
  and leaves `bar` as it was. A theme that restyles `bar` restyles the joined head with it, since the roles are
  bindings.
- The terminal draws it inverted, as it draws `toolbar` (`invertsSurface`), so its top bar keeps its look.

The dark compact head of `10` is not a theme surface of its own: it is the console's choice of a surface for its head
(G-g's `headSurface`). The gallery's index declares that surface's colours as `darkHead`, which no screen gets unless it
names it.

The gallery's bundle (`lims_gallery.cpp`) sets `collapsible.marker: none` and declares `darkHead`.

## R1: the compact top line's padding

**Problem (reproduced; premise corrected by the review).**

- **The grabs.** With `shape.iconOnly: square` the shell's ghost ☰ is a 44 px square on compact and 32 px at a
  `medium` width. The title moves from x = 44 to 68 at 390, and to 56 at 1000.
  - Instrument compact's badges wrap, and its head grows from 76 to 100 px.
  - Results compact's title wraps ("…Paracetamol 500 / mg"), and its head grows from 59 to 98 px.
- **The mockup draws the same square.** `10` compact's header is `height: 52px; gap: 6px; padding: 0 4px`, and its
  ☰ is `class="btn lg" style="width: 44px; padding: 0; background: transparent; border-color: transparent"`, an
  unframed 44 px square. The title starts at 4 + 44 + 6 = 54. Its scan button is the same square at the trailing end.
- **What differs is the bar's padding.** The shell pads its top line's ends by step 3 (`_topLine->setGap(3)`, 12 px)
  and its items apart by step 3 (`_topMain->setGap(3)`), so the title starts at 12 + 44 + 12 = 68. The 44 px square
  is what the design draws and what a touch target needs (WCAG 2.5.5, the theme's `control.touchHeight`). It stays.

**Decision.** On its compact form the frame's top line uses the design's tighter spacing:

- the line's ends are padded by step 1 (4 px): `_topLine->setGap(1)`;
- its items are step 2 (8 px) apart: `_topMain->setGap(2)`;
- `setCompact(false)` restores steps 3 and 3.

The title then starts at 4 + 44 + 8 = 56 against the design's 54, and the trailing square ends 4 px from the edge, as
the design's scan button does. That gives back 12 of the 24 px R1 measured; the rest is the design's own square. A
44 px square beside the title is what `10` draws, so a title that still wraps is one the design's would wrap at the
same width less 2 px.

**Per backend.** Both frontends lay the frame out from the same stacks. On the terminal, steps 1 and 2 are 0 and 1
columns (`layout::stepCells`), so a compact top line starts at column 0 and keeps one column between the menu and the
title.

**Spec.** `shell_frame.md`: the Shape table's top line ("compact: its ends padded by step 1 and its items step 2
apart").

**Tests.**

- `tests/test_ui_shell_frame.cpp`: after `setCompact(true)`, `shell_topbar_line` is recorded with `gap=1` and
  `shell_topbar_main` with `gap=2`; after `setCompact(false)`, both `gap=3`.
  - **Control:** leave the gaps alone, and the first check fails.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a compact `ShellFrame` 390 px wide under `shape.iconOnly: square`.
  The ☰ is `Theme.targetHeight` square at x = `Theme.spaceOf(1)`, and the title's `x` is `Theme.spaceOf(1) +
  Theme.targetHeight + Theme.spaceOf(2)`.
  - **Control:** the same.

**Verify.** Re-grab console, instrument and results at 390×844. The title starts at x = 56.

## R2 and G-a: where the head's lead sits

**Problem (reproduced in the grabs; read).**

- **The mockups put status at the end of the head's first line:**
  - receipt: "Session since 08:30", then "12 received" and "1 rejected" (`02`, at the trailing edge; compact "12" and
    "1");
  - results: "In progress" at the trailing edge of its eyebrow line (`08`);
  - console: the lab select after the title; at the trailing edge, the search, "Connected" and the user's name, "Dr.
    A. Meyer" (`10`: `<span class="tag t-ok">Connected</span><span class="muted">Dr. A. Meyer</span>` closing its
    `<header>`).
- **Plate's subtitle follows its title on the same line** (`07`). That is G-i.
- **The shell draws the lead only under the title.** `buildHeadLead` puts the subtitle and the badges in one row, which
  the shell mounts in `headLeadSlot()`, under `shell_title`. R2 is this: with the worksheet line moved to the eyebrow,
  "In progress" sits alone on a line under results' title, and the head grew from 58 to 76 px.
- **The principal is the sidebar's**, at its foot (`ShellFrame::_principal`). The console mockup's sidebar has no foot;
  the name closes the top bar.

**Decision.** The lead (the subtitle and the badges, which share a line) gets a placement, and the principal gets a
place in the top bar.

### The screen's `leadPlacement`

`leadPlacement` is a screen member, an expression of a name:

- **`below`** (the default): today's place, under the title.
- **`inline`**: on the title's line, right after the title. Where it does not fit, it wraps to a line of its own,
  before the trail does.
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
- **The `inline` and `end` copies are content-sized**: no stretch and no minimum width of their own. Only the `below`
  copy keeps today's stretch. Where the inline copy lies within the line is the frame's slot's business (below).
- `buildHeadTrail` inserts the `end` copy between the tools' `forEach` and the actions' `forEach`. `trailHasContent`
  counts it.
- **In the page** (`HeadPlacement::InPage`) the tab bar carries the title, so there is no title line. `inline` draws
  as `below` there, and `end` sits in the trail as in the shell.
- `Workspace::activeLeadPlacement()` is a tracked read of the active head's placement (`Below` with no active tab).
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
  - **inline:** the lead block is content-sized with no minimum. The slot stretches from the 32-unit minimum, capped
    at its natural width, which is its content-sized copy's. The copy sits at the slot's start, so the lead follows
    the title and the spare width lies after it, before the trail.
  - **not inline:** the lead block stretches from 32 units, as today, and the slot is hidden. A shown, empty slot
    would still take its minimum.
- **The client** mounts the part in it. `src/client/shell.cpp` gains `std::optional<ui::Mounted> _headInline`,
  emplaced in `showActiveHead` from `parts.inlineLead` into `&_frame->headInlineSlot()`, and reset in `resetHead` with
  `_headLead`. Its `setLeadInline` call is in the `_headLayout` effect (G-b), from `activeLeadPlacement()`.

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
- The client sets it after `mountApp()` from `AppRoot::principalPlacement()`.
- It is the app document's, not the theme's, because it is where a fact sits, not a look; a theme does not reach
  `ShellFrame`, which is built from backend widgets in C++. The default keeps every application's shell as it is.

**Per backend.** Nothing new. Both frontends draw the same rows and texts. On the terminal a lead at the `end` is one
more run of cells before the actions, and an inline lead follows the title on its row.

**Spec.**

- `document.md`: "The document" (`leadPlacement`, `principalPlacement`) and "The head" (the three places and their
  order);
- `host.md` (`ScreenHead::leadPlacement`);
- `interpreter.md` (`Screen::leadPlacement`, `AppRoot::principalPlacement`);
- `workspace.md` (`HeadParts::inlineLead`, the three copies, their sizes and ids, `activeLeadPlacement`);
- `shell_frame.md`: `headInlineSlot`, `setLeadInline`, `setPrincipalInHead`, and the Shape table's top line;
- `author.md` (`Scope::leadPlacement`, the app document's `principalPlacement`);
- `client/client.md` (the inline lead's mount; the principal's placement).

**Tests.**

- Corpus (`tests/ui/corpus/documents.json`):
  - `leadPlacement: "end"` loads on a screen, and `"top"` is refused;
  - `leadPlacement` is refused on the app document;
  - `principalPlacement: "head"` loads on the app document and is refused on a screen.
- `tests/test_ui_workspace.cpp`:
  - with `end`, `head_lead_end` is visible inside `head_trail` between the tools and the actions, recorded `content`
    with no `min`, and `head_lead` is hidden;
  - with `inline`, `headPartsForActive().inlineLead` is visible and content-sized, and the lead is hidden;
  - a width change that switches `inline` to `below` swaps them, and `activeLeadPlacement()` follows;
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
  - a screen with `leadPlacement: "inline"` mounts its lead in `headInlineSlot()`, and switching to a screen without
    it unmounts it;
  - **Control:** a client that never mounts `_headInline`.
- `tests/tui/test_tui_shell_frame.cpp`: a `ShellFrame` at 120 columns whose head comes from a `Workspace` harness, its
  screen giving `leadPlacement: "end"`, a tool and a badge. The badge is drawn on the title's row after the tool.
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
  `setLeadInline` (G-a), `setHeadJoined` (G-n) and `setHeadSurface` (G-g), each from its own tracked read.

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

**Decision (the user's: a plain rule).**

- **A tab is at least the control height**, `Theme.targetHeight`, on every form and in every application, the
  workspace's own strip included: 32 px, and 44 on compact as today. The More button follows the bar's height, as it
  does today. A tab is a control, and 20 px was below every other control.
- **An inset strip puts no spacing between itself and the page.** While `inset` is above 0, the column's spacing is
  0, and the page's own inset (`pageInset`, G-f) is the gap. Without `inset`, the strip keeps its step-3 spacing.

**Per backend.**

- **Qt Quick:** `MorphTabs`. The `TabButton`'s `implicitHeight` is `Math.max(…, Theme.targetHeight)`, and the layout's
  `spacing` is `root.inset > 0 ? 0 : Theme.spaceOf(3)`.
- **Terminal:** unchanged. A tab is one row, and an inset page already starts at `barRows + _insetRows`
  (`TabsImpl::childAreas`), with no row between.

**Spec.** `document.md`, the `tabs` row (the page is its `pageInset` under the strip's rule); `qt_quick/backend.md`
(Tabs).

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

- **The title is centred on the sheet.** The sheet head's label takes `objectName: "sheetTitle"`, `textFormat:
  Text.PlainText` (a document's title is characters, never markup) and `horizontalAlignment: Text.AlignHCenter`. A
  trailing `Item` as wide as the dismiss button balances the row, so the title centres on the sheet rather than on the
  room left.
- **A sheet always draws its head rule**, edge to edge (G-d), under the head row, or under the description where there
  is one.

**Per backend.** Qt Quick only. The terminal draws an overlay as a box with its title in the frame.

**Spec.** `document.md`, the `dialog` row (`sheet`: "a head of a dismiss control and the title centred on the sheet,
ruled off from the body"); `qt_quick/backend.md` (Dialog).

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`: a sheet 390 px wide, titled `<b>Sign</b>`.

- `sheetTitle`'s text centre is within 1 px of 195, and it shows the characters `<b>Sign</b>`.
- `headRule` is visible, from x = 0 to 390.
- **Control:** today's head. The centre is at about (390 + dismiss width) / 2, and the rule is hidden.

**Verify.** Re-grab e-signature at 390. Compare with `Main` compact.

## G-f and G-k: a child that bleeds to its container's edges

**Problem (read; the grabs show both; the review found the scroll's clip).**

- **Review's flagged rows.** `09` compact draws them edge to edge, divided and tinted. The pinned body insets
  everything by 16 px.
- **Receipt's session table.** `02`'s "This session" card puts its table's header band and rules to the card's edges,
  while the card's title and the footnote ("S-26-104228 rejected: seal broken.") stay 12 px in. `panel`'s `padding`
  applies to every child alike.
- **One seam, not two bugs.** The same hand-rolled need shows in two screens (AGENTS.md: "The same example defect in a
  second place … is a missing framework seam").
- **Under tabs, a page's padding is outside its scroll.** Review's compact rows sit in a tab page. `MorphTabs` pads
  the page container (`Layout.margins: root.insetPx`, `MorphTabs.qml:260`), outside the scroll a page puts its content
  in, and `MorphScroll` clips (`clip: true`). A child of the scrolled content cannot reach past padding that lies
  outside the scroll's clip.

**Decision.** Two keys.

### `bleed`

A literal flag a node may carry.

- **What it does.** The node's sides lie at its parent's edges, past the parent's horizontal padding, while its
  siblings stay inside. Only the sides bleed: a node's place along a column is its siblings', and the designs need
  nothing else.
- **The direct parent only**, on both frontends. Both screens give the key to a direct child of the padded container,
  and a reach through other containers would make a node's extent depend on ancestors it cannot see.
- **Where it is allowed.** `bleed` is given only on a child of a `column`, a `panel` or a `tabs` tab, and not on the
  view root. Elsewhere it is refused at load (`` `bleed` is a child's of a column, a panel or a tab ``), since a row's
  or a grid's child has neighbours at its sides. A parent with no horizontal padding gives it nothing to bleed past.
- **Its natural width ignores the bleed.** A bleeding child adds its own natural width to its parent's, as any child
  does, not that width plus the padding (probe 2). The parent's padding is already counted once.
- **In the view tree** it is `LayoutHints::bleed` (`bool`, appended after `alignSelf`). It goes through `setLayout`,
  so no backend gains a method.
- **Author:** `Node::bleed()`.

### `pageInset` on `tabs`

`tabs`' `inset` today means two things: where the strip's first tab starts, and the padding of every page.

- **`pageInset`** is a new literal spacing step, 0 to 5, that pads each page. It defaults to `inset`, so instrument and
  every node written for H8 keep their look. A computed or out-of-range value is refused, as `inset`'s is.
- **`inset`** keeps the strip's meaning, and the gap rule of G-c.
- **A page that scrolls** sets `pageInset: 0` and pads its own scrolled content, so a child of that content can bleed
  to the scroll's edges.
- `TabsWidget::setPageInset(int)` is a new pure virtual, called only when the document gives the key. The workspace's
  own strip calls neither.
- Author: `TabsOptions::pageInset`.

**Per backend.**

- **Qt Quick:**
  - each container's content item carries `childBleedInset`:
    - `MorphStack`'s layout `insetX`;
    - `MorphPanel`'s inner column `Theme.spaceOf(padding)`, so a child reaches the frame's inner edge, not over its
      border;
    - `MorphTabs`' pages `Theme.spaceOf(pageInset)`.
  - `MorphWidget` binds `readonly property real bleedInset: parent && parent.childBleedInset !== undefined ?
    parent.childBleedInset : 0`, and `Layout.leftMargin` and `Layout.rightMargin` to `bleed ? -bleedInset : 0`. A
    plain QML binding to the parent's own inset, with no C++ write.
  - `MorphTabs` pads its pages by `pageInset`.
- **Terminal:**
  - the container gives a bleeding direct child its own horizontal padding's columns at both sides, in
    `container_widgets.cpp`'s placement, as `alignSelf` is honoured there;
  - `TabsImpl::setPageInset` pads the pages, and `setInset` only places the strip.
- **`RecordingBackend`:** records `bleed` in the layout string, and `pageInset=0`.

**Spec.**

- `document.md`, "Every node" (`bleed`), the load-time rules, and the `tabs` row (`pageInset`);
- `view_tree.md` (`LayoutHints::bleed`, Tabs);
- `backend_contract.md` ("Layout hints"; `TabsWidget::setPageInset`);
- `author.md` (`bleed()`, `tabs`);
- `qt_quick/backend.md` and `tui/frontend.md`, the Layout and Tabs rows.

**Tests.**

- **Layout case** (`testing/backend_conformance.hpp`) "a bleeding child spans its padded column's width while a
  sibling stays inset": a column with padding `[0, 4]` holding two texts, the second with `bleed`. The second's left
  is the column's left and its width the column's. The first's left is one step-4 inset in. The column's natural
  width is the wider text's plus twice the inset.
  - **Control:** a backend that ignores `bleed`.
- `tests/qt_quick/test_qt_quick_containers.cpp`:
  - a panel with `padding: 2` holding a table with `bleed`: the table's header background runs from the frame's inner
    left edge to its inner right edge;
  - a tabs node with `inset: 4` and `pageInset: 0`: the strip's first tab is `Theme.spaceOf(4)` in, and the page's
    first child is at the page's left;
  - **Control:** the same, without the keys.
- `tests/tui/test_tui_containers.cpp`: a column padded 2 columns holding a bleeding text draws it from column 0.
  - **Control:** ignore the hint.
- Corpus:
  - `bleed: true` loads on a column's child, a panel's child and a tab's child, and is refused on the root and on a
    row's child;
  - `pageInset: 0` loads, and a computed one is refused.

**Gallery** (screen fixes): receipt's session table (G-k) and review's compact flagged rows (G-f).

**Verify.** Re-grab receipt at 1000: the table's header band reaches the card's edges, and the title and the footnote
stay in. Re-grab review at 390: the flagged rows run from x = 0 to 390 and scroll with the page.

## G-g: the console's dark head, an icon-only action, and tinted icons

**Problem (read; the grab shows it).**

- **Only the console's compact head is dark.** `10` compact draws the shell's top bar `#1C1E21` with a white ☰, title
  and scan icon. `02`, `04`, `05`, `08` and `09` compact draw a white head ruled off from the page. `ShellFrame` sets
  `_topBar->setSurface("toolbar")` once, so a dark head could only come from the theme, for every screen at once.
- **The action.** The compact Scan is an icon alone. A head action (`actionButton`) always draws its label, so the
  grab's Scan is a framed "⛶ Scan" 82 px wide.
- **The icon's colour.** `10`'s scan icon is `stroke="currentColor"`, white on the dark head. A themed icon is drawn as
  an `Image` of its SVG (`MorphPlainContent`), in the SVG's own colours: the gallery's `kScanIcon` hard-codes
  `stroke="#17784A"`, which would be green on dark.

**Decision.**

### The screen's `headSurface`

- **`headSurface`, a screen member:** an expression of a surface's name, drawn as the shell's top bar while the screen
  is shown. `null` or absent is none. A name that is no surface draws as none does, as a node's `surface` does. The
  app document refuses it.
- **The top bar's surface**, in one function `ShellFrame::showTopBarSurface()`:
  - `toolbarJoined` while the shown page continues the head (G-n), on both forms;
  - otherwise the screen's `headSurface`, while it names one;
  - otherwise `toolbar`.
- **Following it.** `ScreenHead::headSurface()` is a virtual returning `reactive::Computed<std::string> const*`,
  default `nullptr`. `Screen::headSurface()` is a tracked read, and `Workspace::activeHeadSurface()` is a tracked read
  that is empty with no active tab. `ShellFrame::setHeadSurface(std::string_view)` is called from the client's
  `_headLayout` effect (G-b).
- **The console asks for it**: `headSurface: cond(eq(widthClass, "compact"), "darkHead", null)`. `darkHead` is a
  surface of the gallery's theme index (Theme switches and surfaces).
  - With G-h at 108 units, a 1000 px grab (130 units) is `expanded`: the frame shows its sidebar, and the console's
    head is the white `toolbar`.
  - A 390 px grab is `compact`: the dark head.
  - A `medium` window (80 to 108 units) has the compact frame with ☰ and the white `toolbar`, since the console asks
    only on `compact`.
  - Every other screen is white on every form.
- **No rule under a dark head.** `MorphWidget` borders only `toolbar` and `card`, so a named surface has no rule;
  `10` draws none.

### `iconOnly` on an `actionButton` part

- A condition. While true, the button draws its icon alone, and its label is its accessible name and its tooltip.
- A part with `iconOnly` and no `icon` is refused at load (`` an icon-only action names its `icon` ``).
- The part is the same in a screen's head and in a dialog's or drawer's footer, so both take it:
  - `ActionModel` gains `Prop<bool> iconOnly`, which `lowerActionParts` lowers;
  - `actionBarOf` lowers it for a footer, and now passes the action's `icon` too (S3);
  - `HeadAction` gains `bool iconOnly`;
  - `buildHeadTrail`'s button binds its label to `""` while it holds, and its `a11y.name` and `tooltip` to the label.
- Author: `ActionButtonOptions::iconOnly`.
- Under `shape.iconOnly: square` an icon-only action is the target height's square, 44 px on compact, as `10` draws
  the scan button.

### Icons drawn in their control's colour

A themed SVG that paints with `currentColor` is drawn in the colour of the control that shows it:

- a push button's caption colour, as `MorphButton`'s `palette.buttonText` (white on the dark head, through
  `darkHead`'s `color.accent` for a ghost button);
- a text input's leading icon in the accent, so receipt's green scan field keeps its look;
- a badge's and a menu entry's icon in their text colour.

An SVG without `currentColor` is drawn as it is, so every existing icon set keeps its look.

- **Theme.** `applyBundleTheme` (`theme.cpp`) also hands each icon's SVG text to `Theme.iconSvgs` (name to text).
  `Theme.iconSourceIn(name, color)` answers the `data:image/svg+xml` URL with every `currentColor` replaced by the
  colour's `#rrggbb`, cached by name and colour; `iconSource(name)` stays for callers with no colour. An SVG painted by
  replacing text is what Qt's SVG image plugin draws; it has no `currentColor` of its own.
- **Draw sites.** `MorphPlainContent` (buttons and the menu trigger), `MorphTextInput`'s leading icon, `MorphBadge` and
  `MorphMenu`'s entries ask `iconSourceIn` with their foreground.
- **The gallery's** `kScanIcon` paints `stroke="currentColor"`.

**Per backend.**

- **Qt Quick:** `Theme.qml` (`iconSvgs`, `iconSourceIn`), `theme.cpp`, and the four draw sites.
- **Terminal:** an icon is text, so it draws in its control's style already. An icon-only action whose icon has no
  glyph draws its accessible name (G-m), so it is never blank. A named head surface is drawn as that surface is
  elsewhere: `darkHead` is not inverted, so on the terminal the console's compact top bar is plain cells, not inverse.

**Spec.**

- `document.md`: "The document" and "The head" (`headSurface`), the `actionButton` table (`iconOnly`);
- `host.md`, `interpreter.md` (`headSurface`);
- `workspace.md` (`activeHeadSurface`; the trail's icon-only buttons);
- `shell_frame.md` (the top bar's surface);
- `theme.md` (Icons: `currentColor`; `iconSourceIn`);
- `author.md`;
- `qt_quick/backend.md` (Button, Text input, Badge, Menu) and `tui/frontend.md` (the top bar's surface).

**Tests.**

- `tests/test_ui_shell_frame.cpp`:
  - a new frame's top bar is recorded `surface=toolbar`;
  - `setHeadSurface("darkHead")` makes it `darkHead`, and `setHeadSurface("")` `toolbar` again;
  - with `setHeadJoined(true)` it is `toolbarJoined` whatever the head surface, compact or not;
  - **Control:** set the surface once.
- `tests/test_ui_workspace.cpp`:
  - `activeHeadSurface()` follows a screen's `headSurface` across a width change;
  - an action with `iconOnly: true`, `icon: "scan"` and label "Scan" is recorded as a button with an empty label, the
    accessible name "Scan" and the tooltip "Scan";
  - **Control:** ignore either key.
- `tests/qt_quick/test_qt_quick_theme.cpp`:
  - with a set whose `dot` icon is `<svg …><circle fill="currentColor" …/></svg>`, `Theme.iconSourceIn("dot",
    "#ffffff")`'s SVG text holds `fill="#ffffff"` and no `currentColor`;
  - a ghost button showing `dot` on a surface whose accent is white draws a white pixel at the icon's centre;
  - **Control:** answer `iconSource(name)` whatever the colour, and both checks fail.
- `tests/test_ui_document.cpp` (S3): a dialog footer action with `icon: "scan"` lowers to a button whose icon is
  `scan`.
  - **Control:** today's `actionBarOf`, which drops it.
- Corpus: `iconOnly` with an `icon` loads, and without one is refused; `headSurface` loads on a screen and is refused on
  the app document.

**Gallery** (lane I-S):

- the bundle's `darkHead` surface (`color.surface` `#1C1E21`, `color.text` and `color.accent` `#FFFFFF`,
  `color.muted` `#C3C9D2`, `color.hover` `#FFFFFF1F`) and the `currentColor` scan icon;
- console's `headSurface` as above, and its compact Scan `emphasis: ghost`, `iconOnly: true` (compact-only already).

**Verify.** Re-grab console at 390: a dark head with a white ☰, title and scan icon, and no rule. Re-grab receipt,
audit, samples, results and review at 390: white heads, ruled off. Receipt's scan field icon stays green.

## G-h: the sidebar at 1000 px

**Problem (reproduced, probe 1; read).**

- **The mockups.** Every mockup is drawn at two widths: expanded 1000 px, compact 390 px. `10` and `Main` draw the
  expanded shell with a permanent sidebar, 176 px wide, beside an 824 px page.
- **The shell.** `src/client/shell.cpp`'s `compactFrame` is compact below `expanded`, with this reason: "with its
  sidebar, an 80-unit `medium` window would leave the page narrower than any `compact` one".
- **The classes.** `ui::widthClassOf` puts `expanded` at 160 units: 1228 px with the gallery's 7.67 px unit. So at
  1000 px (130 units) the frame is compact, which also hides H5's navigator overlines in every grab.

**Decision (the user's).** `expanded` starts at **108 units**: the compact bound, 80, plus the sidebar's 28
(`kShellSidebarWidth`), the narrowest window whose page beside the sidebar is not compact.

- `backend.hpp` gains `inline constexpr int kExpandedWidthUnits = 108;` beside `widthClassOf`, which uses it.
  `shell_frame.hpp` gets `static_assert(kShellSidebarWidth + 80 <= kExpandedWidthUnits)`, so a wider sidebar cannot
  quietly break the rule.
- `compactFrame` keeps its reason. Touch targets and sheets follow `compact` only, so they do not change.
- It moves every document's and every terminal's `expanded` bound from 160 to 108 (on the terminal, 108 columns).
- At the default unit, 108 units is about 830 to 865 px, close to Material's `expanded` at 840 dp, as 80 units (about
  614 px) is close to its 600.

Considered and not taken: keeping 160 and grabbing at 1228 px or wider; a frame-only threshold, which needs a new
backend width report.

**Existing tests it changes.** Editing them is part of this item, since each pins the old bound:

- `tests/test_ui_recording_backend.cpp:690–694`: 107.5 is `medium` and 108 `expanded`.
  - **Control:** the old bound, and the `108` check fails.
- `tests/qt_quick/test_qt_quick_widgets.cpp:1088–1097`. The window starts at `100 * unit` in place of `800`, so it
  starts `medium` whatever the unit, and the resize loop becomes `{170.0, 120.0, 100.0, 70.0, 75.0}`. The expected
  `{"expanded", "medium", "compact"}` is unchanged; 120 is now `expanded` and reports nothing.
- `tests/qt_quick/test_qt_quick_frontend.cpp:~427–431`. The window starts at `widthFor(engine, 100)`, not 120, so it
  starts `medium` as it did, and the lane rechecks the classes heard after it.
- `tests/tui/test_tui_frontend.cpp:503–510`. The resizes become 90 and then 100 columns, both `medium` and reported
  once, and the comment says so.

**Do the expanded layouts fit the narrower page?** The sidebar leaves a 1000 px window a page of 1000 − 28 units ≈
785 px, and 757 px inside the page's inset, against the mockups' 824 px page (`10`, `Main`). Read from the screens,
two splitters were sized for the 1000 px page the screens drew before:

- **review's** `{37, 96}` units is 1020 px, wider than the page. The queue keeps `09`'s ratio of 280 to 712: at 785 px
  that is 221 and 564 px. Its sizes become `{29, 74}`.
- **console's** `{70, 45}` units is 882 px. It keeps `10`'s ratio of 424 to 360 within the 757 px: `{53, 45}`.

The lane re-grabs every screen at 1000×830 and checks, for each expanded layout, that no content runs past the page's
right edge and nothing scrolls sideways. It adjusts any other fixed sizing that does, naming each change in the screen
fixes:

- audit's facets, `fixed(29)`;
- plate's sources, `fixed(30)`;
- receipt's session, `fixed(40)`;
- stability's summary, `fixed(33)`.

**Spec.** `expressions.md` and `backend_contract.md` ("`medium` below 108"); `backend.hpp`'s doc;
`client/client.md` (the frame's form).

**Verify.** Re-grab every screen at 1000×830. The sidebar shows, with the navigator's overlines, and the page is
about 785 px wide.

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
Qt Quick (`theme.md`: "a button … nothing"). `MorphMenu`'s trigger falls back to a glyph (its own `⋯` and `☰`) and
then to its accessible name. The terminal's button draws a framework glyph, or nothing.

**Decision.**

- **Built-in chevrons.** The framework names `nav.back` and `nav.forward` (in `kFrameworkIcons`; no built-in component
  draws them yet) are drawn by Qt Quick as a vector chevron when the set does not name them.
  - The new `MorphChevron.qml` is two antialiased `Rectangle`s, each 1.5 px thick, rotated by ±45° about their meeting
    end. No `QtQuick.Shapes`, so no new module reaches the WebAssembly build.
  - Its height is `Math.round(0.6 × Theme.bodyPixels)`, 8 px at 13 px. It is in the caption's colour, and its
    direction follows the icon name. It is not mirrored under right-to-left, since the name already says which way.
- **One glyph table for every framework name.** `applyBundleTheme` (`theme.cpp`) writes `Theme.iconGlyphs`, name to
  glyph, from `ui::iconGlyph` over `kFrameworkIcons`, once at start-up whether or not the bundle has a set. QML then
  reads the same glyphs the terminal draws, and `MorphMenu`'s own `⋯`/`☰` literals give way to it.
- **An icon-only push button is never blank.** With no label, it draws, in order:
  1. the set's icon (`iconSourceIn`, G-g);
  2. a built-in chevron;
  3. `Theme.iconGlyphs[name]`;
  4. its accessible name.

  On the terminal it is the glyph, else the accessible name.
- **Pagers** are written `button({.icon = "nav.back", .shape = ControlShape::Square}).a11y("Previous page")`. The
  label goes, and the name stays.

**Per backend.**

- **Qt Quick:** `MorphPlainContent` gains `iconName`, set by `MorphButton`. It draws `MorphChevron` for those two names
  when the set lacks them, and otherwise the glyph or the name, as above. `MorphMenu`'s `triggerText` reads
  `Theme.iconGlyphs`. `MorphChevron.qml` joins the module's QML list in `src/qt_quick/CMakeLists.txt`.
- **Terminal:** `ButtonImpl` (`leaf_widgets.cpp`). With an empty label and no glyph, it draws the accessible name.

**Spec.** `theme.md` (Framework names: `nav.back` and `nav.forward` are drawn as chevrons on Qt Quick; `iconGlyphs`;
the button's fallbacks); `qt_quick/backend.md` and `tui/frontend.md` (Button).

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a square button with `icon: nav.back` and no set shows a visible `MorphChevron` at least `0.55 ×
    Theme.bodyPixels` tall;
  - a button with `icon: scan`, no label, no set and the name "Scan" shows the text "Scan";
  - a button with `icon: close` and no set shows `×`;
  - **Control:** today's content item, which draws none of them.
- `tests/qt_quick/test_qt_quick_theme.cpp`: `Theme.iconGlyphs["more"]` is `⋯` after `applyBundleTheme` with no set.
  - **Control:** skip the write.
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
    `toolbarJoined`, on both forms (G-g's `showTopBarSurface`);
  - the stack's first child is then responsible for the rule under the block. `pinned`'s divided column draws it
    under its top bar, a `tabs` strip draws its own, and a hand-made root is `divided` (plate's and audit's compact
    roots).
- **It needs `pageEdge: true`** on the same stack, since an inset page has a grey margin between it and the head. It is
  refused without it at load (`` `headJoined` needs `pageEdge` ``).
- **`pinned` writes it when its top bar always shows**: it has a top bar, an inset, and none of the bar's nodes has a
  `visible` condition. A bar that can hide (H7) would leave the head with no rule under it while hidden, so such a
  page writes none, and a screen that wants it writes the key itself. Stability's, samples' compact and results'
  compact top bars always show.
- **Following it.**
  - `Screen::headJoined()` is a tracked read, which shares `pageEdge()`'s walk through `switch`es, factored into one
    `shownRoot()`.
  - `ScreenHead::headJoined()` is a virtual with a default of `nullptr`.
  - `Workspace::activeHeadJoined()` is `!activePageInset() && headJoined`. So a strip of two tabs, which insets the
    page, keeps the rule.
  - `ShellFrame::setHeadJoined(bool)` calls `showTopBarSurface()` (G-g).

**Per backend.** Qt Quick: `Theme.qml` gains `toolbarJoinedScheme`, its own `MorphScheme` whose every role is bound to
`barScheme`'s, the `toolbarJoined` case of `scheme()`, and the name in `draws()`. `MorphWidget` gives it no border. The
terminal inverts it as `toolbar`, so its top bar keeps its look.

**Spec.**

- `document.md`, the `column, row` row (`headJoined`) and "A bar that stays in view" (`pinned` writes it, and when);
- `interpreter.md` (`Screen::headJoined`);
- `host.md`; `workspace.md` (`activeHeadJoined`);
- `shell_frame.md` (`setHeadJoined`, the top bar's surfaces);
- `theme.md` (the Surfaces table);
- `author.md` (`ContainerOptions::headJoined`, `pinned`).

**Tests.**

- `tests/test_ui_author.cpp`:
  - `pinned(content, {.top = …})` with unconditioned nodes writes `headJoined: true`;
  - one whose top node has `.visible(c)`, one with no top bar, and one with `.inset = 0` write none;
  - **Control:** today's helper, and write it for every top bar, and the first and second checks fail in turn.
- Corpus: `headJoined` with `pageEdge` loads; without it, or computed, it is refused.
- `tests/test_ui_host.cpp`: `headJoined()` reads through a `byWidth` switch to a pinned page with a top bar, and
  answers `false` for its other case.
  - **Control:** read the root only.
- `tests/test_ui_workspace.cpp`: `activeHeadJoined()` is `true` for that screen alone and `false` once a second tab
  opens.
  - **Control:** ignore the strip.
- `tests/qt_quick/test_qt_quick_theme.cpp` (S1):
  - a theme index whose `toolbarJoined` surface sets `color.surface: #EEEEEE` reaches
    `Theme.scheme("toolbarJoined").surface` and leaves `Theme.scheme("bar").surface` as it was;
  - one whose `bar` surface sets `#DDDDDD` reaches both;
  - **Control:** answer `barScheme` for `toolbarJoined`, and the first check fails.
- `tests/client/test_shell.cpp`: opening a pinned page with an always-shown top bar records the top bar's
  `surface=toolbarJoined`, and switching to a plain screen restores `toolbar`.
  - **Control:** drop the effect.

**Gallery.**

- instrument's root, and plate's, audit's and review's compact roots, take `headJoined: true` (Screen fixes);
- stability, samples' compact and results' compact get it through `pinned`.

**Verify.** Re-grab instrument and stability at 1000, and samples, results, plate, audit and review at 390. No rule
shows between the head and the block that continues it, and one shows under the block.

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
- **Today.** `MorphSplitterHandle` is the Basic style's: a 6 px bar in `palette.button`, with no grip, in colours that
  ignore the theme.

**Decision.**

- The handle is drawn in the splitter's scheme:
  - at rest, the bar is `scheme.border`;
  - hovered or pressed, it is `scheme.controlBorder`.
- A grip, `objectName: "grip"`, is centred on it: 2 px across the axis by `Math.min(28, length / 3)` along it, radius
  1, in `scheme.neutralDot`, so the handle reads as something to drag.
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

- **Qt Quick:** `MorphStack`.
  - `gapLength` adds the rule's 1 px on both axes: `Theme.spaceOf(root.gap) + (root.divided ? 1 : 0)`, in place of
    `root.divided && root.axis === 0`.
  - The divider `Repeater` takes both axes. A row's divider is 1 px wide, the line's height, at `x: layout.x +
    modelData.x + modelData.width + (root.gapLength - 1) / 2`.
- **Terminal:** draws none, as for a column.
- **The mount** already calls `setDivided` on any stack that gives the key.

**Spec.**

- `document.md`, the `column, row` row;
- `author.md` (`ContainerOptions::divided`);
- `backend.hpp`'s `StackWidget::setDivided` doc ("between each two shown children of the stack", not "of a vertical
  stack");
- `backend_contract.md`, the `StackWidget` row ("A divided stack");
- `qt_quick/backend.md` (Row: the vertical rule).

**Tests.**

- `tests/qt_quick/test_qt_quick_containers.cpp`: a row of two fixed-width columns with `gap: 2` and `divided: true`
  shows one divider, 1 px wide, centred between them, and the second column starts `Theme.spaceOf(2) + 1` after the
  first.
  - **Control:** today's `MorphStack`: no divider, and a step-2 gap.
- Corpus: `divided` loads on a row, and is refused on a row with `wrap: true`.

## G-r: a banner's action, as a link in the message or a button

**Problem (found here; read).**

- **The designs draw a banner's action three ways:**
  - stability's attention banner links the field's name inside its sentence, with the sentence's full stop after it
    (`06`: "1 field needs attention: <u>Containers per pull</u>.");
  - the console's alert has a framed "Open run" button at the trailing edge (`10` expanded);
  - on compact, the same is a full-width button under the text, inside the alert (`10` compact).
- **Today.** `MorphBanner` draws its action as a ghost button at the trailing edge (the label `fillWidth`), and the
  terminal draws `[label]` after the text.

**Decision.** The look is the banner's, so the keys are on the `banner`, not on the `action` part, which `emptyState`
shares.

- **`actionStyle`**, a literal name:
  - `ghost`: the default, today's;
  - `button`: a framed normal button at the trailing edge;
  - `link`: the action drawn inside the message's text, underlined in the accent, wrapping with the sentence.
- **`actionStretch`**, a literal flag: the action takes a line of its own under the text, as wide as the banner. It is
  refused beside `actionStyle: link`, and without an `action`.
- **Where the link sits.** In a `link` banner's `spans`, at most one run may carry `"action": true`. That run's text is
  the link's text, at that place in the sentence. Without such a run, the link follows the message, its text the
  action's label. `action` on a run is refused anywhere else: in another kind's spans, or in a banner whose style is
  not `link`. Stability writes `spans({"1 field needs attention: ", actionRun("Containers per pull"), "."})`.
- **In the view tree.** `Banner` gains `BannerActionStyle actionStyle = BannerActionStyle::Ghost` and `bool
  actionStretch = false`, appended. `TextSpan` gains `bool action = false`, appended, which only a banner reads.
- **The backend.** `BannerWidget` gains `setActionStyle(BannerActionStyle style, bool stretch)`, a pure virtual the
  mount calls once, only when the document gives either key.
- **Author:** `BannerOptions::actionStyle` and `actionStretch`, and `ui::actionRun(text)`.

**Accessibility.** A link here runs commands; it does not navigate. To assistive technology it stays a button, named
by the action's label:

- Rich text exposes no accessible node for an anchor, so an accessible proxy carries it: a 1×1 transparent `Item`,
  `objectName: "actionProxy"`, inside the banner, after the label.
- The proxy takes `Accessible.role: Accessible.Button`, `Accessible.name` the action's label, `activeFocusOnTab: true`
  and `Accessible.onPressAction`, and Enter and Space on it fire the action.
- While it has focus, the link's markup is drawn with the focus colour's background, so a keyboard user sees where the
  focus is.
- The label's own text stays the banner's accessible description, as today.

**Per backend.**

- **Qt Quick:** `MorphBanner`.
  - **`link`.** The message label (rich text whenever there are runs, and for a link) renders the action as `<a
    href="action">…</a>`, styled underlined in the accent. The label's `onLinkActivated` fires the action, so the link
    wraps with its sentence wherever the line breaks fall. `runsHtml` (in `MorphWidget`) gains the anchor for a run with
    `action`, and the banner appends one after the runs when none has it.
  - **`button`.** The action's background is `MorphButtonBackground` framed, not ghost.
  - **`actionStretch`.** The action moves to a second row with `Layout.fillWidth`.
- **Terminal:** `BannerImpl` (`display_widgets.cpp`).
  - `link`: the run, or the label after the text, drawn underlined in the accent, within the wrapped text. A click on
    those cells, or Enter while the banner's action has focus, fires it.
  - `button`: `[ label ]`.
  - `actionStretch`: on its own row.
- **`RecordingBackend`:** records `actionStyle=link` and `actionStretch=true`, and a run's `action`.

**Spec.** `document.md` (the `banner` row; the runs' keys); `view_tree.md` (Banner, `TextSpan`);
`backend_contract.md` (`BannerWidget`); `author.md`; `qt_quick/backend.md` and `tui/frontend.md` (Banner).

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a `link` banner with an action run: the label's text holds one `<a href="action">` around "Containers per pull"
    followed by ".";
  - calling the label's `linkActivated("action")` fires the action once;
  - `actionProxy`'s role is `Button` and its name the action's label, and Enter with focus on it fires the action;
  - a stretched `button` action is as wide as the banner's content and below the text;
  - **Control:** ignore the style: the label has no anchor and the proxy does not exist.
- `tests/tui/test_tui_display.cpp`:
  - a `button` banner draws `[ Open run ]`;
  - a `link` banner draws "Containers per pull" underlined within its sentence, and a click on it fires the action;
  - **Control:** `[Open run]`, and the label after the text.
- Corpus:
  - `actionStyle: "link"` loads, and an `action` run inside its spans loads;
  - `actionStretch` beside `link`, an `action` run in a `ghost` banner, an `action` run in a `text`'s spans, and two
    `action` runs are refused.

## S3: a footer action keeps its icon

**Problem (read).** `ScreenCore::actionBarOf` (`interpreter.hpp:7322–7335`) builds each footer `Button` from the
action's label, emphasis, click, visibility and enablement, and drops `ActionModel::icon`. A dialog's or drawer's
`actions` part with an `icon` draws no icon, while the same part in a screen's head does (`buildHeadTrail`).

**Decision.** `actionBarOf` passes the icon. `Button::icon` is a string set once, so it takes `action.icon`'s value at
lowering; an icon that changes later is reported once, as a head action's is. It lands in I-S with G-g's `iconOnly`,
which edits the same function. Test: in G-g's tests.

## S10: `MorphText`'s binding loop on Qt 6.8.1

**Problem (reported; cause read, not reproduced here).**

- **The report.** Qt 6.8.1 prints `MorphText.qml:7: Binding loop detected for property "implicitWidth"` three times in
  the test "an elided mono text's tooltip is its full text, and a node's own tooltip wins"
  (`tests/qt_quick/test_qt_quick_widgets.cpp:1318`). The test passes, and Qt 6.12 prints nothing.
- **The likely cause.** For a one-line text, `implicitWidth` is `Math.max(metrics.advanceWidth(root.text),
  label.implicitWidth) + …`. `label.implicitWidth` is the width of `label.text`, which is `root.fitted`.
  `root.fitted` elides by `root.controlWidth`, which follows the widget's width, which a layout derives from
  `implicitWidth`. That is a cycle in the binding graph, which 6.8.1 detects and 6.12 does not report.

**Decision.** The lane reproduces it first on `build/lg-qt681` (`ctest -R` over that test, reading the log), then
breaks the cycle where it is:

- a one-line text's `implicitWidth` reads `metrics.advanceWidth(root.text)` alone, not `label.implicitWidth`. The
  label of a one-line text never asks for more than its whole text's advance, so dropping the `Math.max` changes no
  width;
- a wrapping text keeps `label.implicitWidth`, which does not depend on `fitted`.

If the reproduction shows a different cycle, the lane fixes that one and says so in its commit.

**Test.** `tests/qt_quick/test_qt_quick_widgets.cpp`, the same test case: around its two `scene.settle()` calls, a
scoped `qInstallMessageHandler` collects warnings, and the test checks that none contains "Binding loop".

- **What it proves.** On Qt 6.8.1 the check fails if the loop returns. On 6.12, which does not report this loop, it
  passes either way.
- **Control:** run it on `build/lg-qt681` with today's `implicitWidth`; it fails. The lane records that run's output.

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
- **The attention banner** (`attention`) takes `actionStyle: link` and the spans `"1 field needs attention: "`, the
  action run "Containers per pull", and `"."`, with the action's label "Containers per pull" (G-r).
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
  `pageEdge: true`, `headJoined: true` and `divided: true`: the button row in a `bar` column padded `{"lit": [2, 4]}`,
  then the rows' sections in a column padded 4. The root's rule draws the line under the block.
- **The selection bar's** "6 wells selected · A3–A8" bolds the count alone: `selectionText` becomes `spans`, with
  `ui::strong` on "6 wells selected" and the range plain, and the text's role `normal`.
- **"To place"**: the `forEach` takes `divided: true`, and "+ 9 more" takes `alignSelf: center`.
- **Compact well rows** read "**A1** · Standard 1, 100 ng/mL" as one text. `runLine` replaces the fixed-width name and
  the line with one text whose `spans` are the strong name (or range), " · " and the line.
- **The sources' width** `fixed(30)` is rechecked against the 785 px page (G-h).

### Results (`results.cpp`, lane I-S)

- `leadPlacement: end` on expanded. "In progress" is hidden on compact (R2, G-a).
- **Compact's head** (medium):
  - `titleSpans` is `cond(compact, spans({"Sample ", n, " of 8 · ", mono(sample)}), [])`. The title stays "Assay and
    impurities · Paracetamol 500 mg" as the tab's caption;
  - ‹ Previous and Next › move into `pinned`'s top bar, which always shows, so it joins the head (G-n);
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
- **The expanded splitter** is `{29, 74}` (G-h).
- **The compact worksheet** (medium):
  - its root is a `column` with `pageEdge: true`, `headJoined: true` and the page's height;
  - the screen's `eyebrow`, `titleSpans` and four `status` parts take the sheet line, the worksheet title and the
    summary tags on compact (each tag a `status` part reading `pick(review, "compactSummary").at(i)`, visible while
    engaged);
  - the back link sits in a `bar` column padded `{"lit": [1, 4]}`;
  - the record tabs stretch and take `inset: 4`, `pageInset: 0` and `stripSurface: "bar"` (G-f);
  - each page is `scroll(column(padding 4, [...]))`, so the page's own padding lies inside its scroll;
  - the decisions sit in a `bar` row padded `{"lit": [2, 4]}`, divided from the tabs.
- **Compact's flagged rows** are a divided `forEach` with `.bleed()`, a direct child of the Results page's padded
  column (G-f). Each row is a `column` padded `{"lit": [2, 4]}` on the `outOfSpec` surface while flagged, in place of
  the padded panels.
- **Compact's All/Flagged** control drops its "Show" caption and stretches.
- The splitter's grip comes from G-p.

### Audit (`audit.cpp`, lane I-V)

- **Expanded** (medium). The root is a `row` with `pageEdge: true`, `divided: true` (G-q), the page's height and
  `align: stretch`:
  - the facets in a `column` on `bar`, padded `{"lit": [3, 3]}`, `fixed(29)` (rechecked against the 785 px page,
    G-h);
  - then the chips and the trail in a `column` padded 4.
- **Compact** (medium). The root is a `column` on `bar` with `pageEdge: true`, `headJoined: true` and `divided:
  true`:
  - Filters, Export CSV and the count in a column padded `{"lit": [2, 4]}`;
  - the entries a divided `forEach`, each entry padded `{"lit": [3, 4]}`;
  - Load 25 more padded 4.
- **The pager** uses `nav.back` and `nav.forward` (G-m).

### Samples (`samples.cpp`, lane I-V)

- **The expanded bulk bar** (medium). Its row takes `padding: ui::padding(0, 2)` inside the panel's step 1, so its text
  is 12 px from the border and its buttons 4 px from the top.
- **The compact search, Filters and Sort** (medium) move into `pinned`'s `top`. They have no `visible` condition, so
  the bar joins the head (G-n).
- **"·"** precedes "Select all 312": the selected text's spans end with " ·".
- **The pager** uses `nav.back` and `nav.forward` (G-m).

### Console (`console.cpp`, lane I-S)

- `leadPlacement: end`. "Connected" is hidden on compact. `headSurface` is `darkHead` on compact, and the compact Scan
  is `ghost` and `iconOnly` (G-a, G-g).
- **The alert** takes `actionStyle: button`, and on compact also `actionStretch: true` (G-r).
- **"The 6 most urgent of 38."** goes. The "On hold" note stays.
- **The two bottom panes** are one card. The splitter sits in a `panel` with `padding: 0`, its sizes `{53, 45}` (G-h),
  and each pane is a column padded 2 in place of its own panel.
- **The sample pager** uses `nav.back` and `nav.forward` (G-m).

### Instrument (`instrument.cpp`, lane I-V)

- The root takes `headJoined: true` at both widths (G-n). The strip's height comes from G-c.

### The gallery's bundle and app document (`lims_gallery.cpp`, lane I-S)

- `collapsible.marker: none`;
- the `darkHead` surface file and its index entry;
- `kScanIcon` painting `stroke="currentColor"`;
- `principalPlacement: "head"`.

## Totals

**New document keys and members:**

- the screen's `leadPlacement` and `headSurface`; the app document's `principalPlacement`;
- every node's `bleed` (a child of a `column`, a `panel` or a tab);
- `column` and `row`: `headJoined`; `divided` on a non-wrapping `row`;
- `tabs`: `pageInset`;
- an `actionButton` part's `iconOnly`;
- a `banner`'s `actionStyle` and `actionStretch`; a run's `action` (a `link` banner's spans only).

**Theme:**

- the keyword token `collapsible.marker` and `Theme.collapsibleMarker`;
- the built-in surface `toolbarJoined`, with its own scheme;
- `Theme.iconGlyphs`, `Theme.iconSvgs` and `Theme.iconSourceIn(name, color)`.

**New pure virtuals**, each with every implementation (Qt Quick, terminal, `RecordingBackend`):

- `BannerWidget::setActionStyle(BannerActionStyle, bool)`;
- `TabsWidget::setPageInset(int)`.

**Other API:**

- `LayoutHints::bleed`; `Banner::actionStyle` and `actionStretch`; `TextSpan::action`; `BannerActionStyle`;
- `LeadPlacement`, `PrincipalPlacement`; `kExpandedWidthUnits`;
- `ScreenHead::leadPlacement()`, `headJoined()` and `headSurface()`, each with a default;
- `Screen::leadPlacement()`, `headJoined()`, `headSurface()` and `shownRoot()`; `AppRoot::principalPlacement()`;
- `HeadAction::iconOnly`, `ActionModel::iconOnly`;
- `Workspace`: `HeadParts::inlineLead`, `activeLeadPlacement()`, `activeHeadOneLine()`, `activeHeadJoined()`,
  `activeHeadSurface()`;
- `ShellFrame`: `headInlineSlot()`, `setLeadInline`, `setHeadOneLine`, `setHeadJoined`, `setHeadSurface`,
  `setPrincipalInHead`, and the compact form's gaps (R1);
- the client: `_headInline` (a `Mounted`) and `_headLayout` (an effect);
- Author:
  - `Node::bleed()`;
  - `ContainerOptions::headJoined`;
  - `TabsOptions::pageInset`;
  - `ActionButtonOptions::iconOnly`;
  - `BannerOptions::actionStyle` and `actionStretch`; `ui::actionRun`;
  - `Scope::leadPlacement` and `headSurface`.

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
- **The terminal's dark head.** A named head surface is not inverted on the terminal (G-g).

## Decisions

Taken with the user on 2026-10-30:

- **G-h:** `expanded` starts at 108 units.
- **G-c:** tabs are the control height, as a plain rule in every application.

Taken by the coordinator on the review:

- **G-g:** the dark head is the console's own choice (`headSurface`), not the theme index's for every compact head; a
  joined head is `toolbarJoined` on both forms.
- **G-f:** `tabs` gets `pageInset`, and review's compact pages scroll their own padding.
- **G-r:** the link is drawn inside the message's rich text, with an accessible proxy; the keys are the banner's.
- **G-m:** the glyphs come from a `Theme.qml` table that `theme.cpp` writes.

Taken at this design's defaults:

- **R1:** the 44 px square stays; the compact top line takes the design's spacing.
- **G-a:**
  - one `leadPlacement` for the subtitle and the badges together, with the `end` lead between the tools and the
    actions;
  - the inline and end copies are content-sized, and the frame's inline slot holds the stretch;
  - the principal's place is the app document's, default `sidebar`.
- **G-b:** a one-line head is centred; a taller one keeps its tops aligned.
- **G-d, G-e, G-l, G-o, G-p:** rules for every application, since each matches the conventional look the defaults
  already aim at.
- **`bleed`:** horizontal only, against the direct parent, on both frontends.
- **G-j:** a theme switch, off by default.
- **G-n:** `pinned` joins the head only with a top bar that always shows.
- **G-g's icons:** `currentColor` is replaced with the control's foreground, and an SVG without it is drawn as it is.

## Review response

Every finding of the design's review was taken.

**Blockers.**

- **B1. The compact head's surface.**
  - `compactToolbar` is gone. The top bar is `toolbarJoined` while joined, on both forms; otherwise the screen's
    `headSurface` while it names one; otherwise `toolbar`.
  - The dark head is the console's choice, of the gallery's `darkHead` surface, on compact only.
  - G-g states what 1000 px (expanded, white, sidebar), 390 px and `medium` windows get.
- **B2. Bleeding under tabs.** `tabs` gains `pageInset` (default `inset`), with `TabsWidget::setPageInset`. Review's
  compact pages are `scroll(column(padding 4, […]))` with `pageInset: 0`, and the flagged rows bleed against that
  column, inside the scroll's clip.
- **B3. The link.**
  - The link is an anchor in the message's rich text, fired by `onLinkActivated`, so it wraps with the sentence.
  - An `actionProxy` keeps a Button role, the name, focus and the press action.
  - A run's `action` places it inside the sentence, so stability's "." follows it.
  - The keys are the banner's (`actionStyle`, `actionStretch`), not the shared part's. The tests read the anchor and
    the proxy.

**Should-fix.**

- **S1.** `toolbarJoined` has its own `MorphScheme`, bound to `barScheme`'s roles, with the mirror test.
- **S2.** Icons that paint `currentColor` are drawn in their control's foreground (`iconSourceIn`). The gallery's scan
  icon uses it. This is implemented, not a residual.
- **S3.** `actionBarOf` passes `icon`, and it is listed under Defects this batch fixes.
- **S4.** G-m's glyphs come from `Theme.iconGlyphs`, written by `theme.cpp`. I-Q owns both.
- **S5.**
  - The `end` and `inline` copies are content-sized, with no minimum and no stretch.
  - `Workspace::activeLeadPlacement()` is added.
  - The client's `_headInline` is mounted in `showActiveHead` and reset in `resetHead`.
  - Totals list them.
- **S6.** G-h lists the four existing tests it edits, with their new values, and says that editing them is part of the
  item. It adds the 785 px fit check, resizes review's and console's splitters, and names the other fixed sizes to
  recheck.
- **S7.**
  - `MorphStack.gapLength` adds the rule on both axes, and the divider is placed for rows.
  - `backend.hpp`'s `setDivided` doc, `backend_contract.md`'s `StackWidget` row and `qt_quick/backend.md` are in G-q's
    Spec list.
- **S8.** The lane tables name regions in `backend_contract.md`, `tests/test_ui_author.cpp` and
  `tests/test_ui_recording_backend.cpp`.
- **S9.** R1's premise is corrected: `10` compact's ☰ is a 44 px unframed square with the title at 54. The fix is the
  compact line's padding and gap. The 32 px slot and the enlarged pointer target are gone.
- **S10.** The 6.8.1 binding loop is an I-Q item, with its likely cause, the fix, and a warning-capturing check whose
  control is a run on `build/lg-qt681`.
- **S11.** `pinned` writes `headJoined` only when no node of its top bar has a `visible` condition.

**Nits, all taken.**

- **`bleed`:**
  - its natural width ignores the bleed;
  - `bleedInset` is a QML binding to the parent's `childBleedInset`;
  - it reaches the direct parent's padding only, on both frontends.
- **G-e:** the sheet title is `PlainText`, named `sheetTitle`.
- **`MorphChevron`:** rotated, antialiased `Rectangle`s.
- **G-a:** the terminal test builds its head through a `Workspace` harness.
- **Stability:** the "." follows the link.
- **Plate and audit:** the compact roots are `divided`.
- **The final re-grab** adds samples with nothing selected (H7) and the expanded pager scrolled into view (Lanes).

## Defects this batch fixes

- **An icon-only push button whose icon the theme does not name draws nothing** (G-m). Read: `MorphPlainContent`
  draws an `Image` only when its source is ready, and `theme.md` documents the blank. The terminal draws an empty
  bracket for a name with no glyph.
- **An inset tabs node puts 28 px, not its inset, between its strip and its page** (G-c). Reproduced in the grab
  (y = 97 to 124).
- **A dialog's or drawer's footer action drops its `icon`** (S3). Read: `actionBarOf`, `interpreter.hpp:7322–7335`.
- **A themed icon cannot follow its control's colour** (G-g). Read: `MorphPlainContent` draws the SVG as it is.
- **`MorphText`'s `implicitWidth` binding loop on Qt 6.8.1** (S10). Reported by the comparison; cause read, to be
  reproduced by the lane.

None is filed, since each is fixed by the change that touches it (AGENTS.md: "A fix smaller than the issue describing
it").

## Lanes

Three lanes, each in its own worktree, each owning the files it changes. Implementation runs on Sonnet. Each lane
wraps every build, test and clang-tidy run in `flock <scratchpad>/morph-build.lock` and builds with `-j10` at most.

### Framework files

| Lane | Items, in order | Owns |
|---|---|---|
| I-Q, Qt Quick component looks | S10, G-j (theme keyword), G-l, G-o, G-p, G-d, G-e, G-m (with `iconGlyphs`), G-g's tinted icons | `theme.hpp` (`kKeywords`, `kThemeTokens`); Qt Quick `theme.cpp`, `MorphText.qml`, `MorphButton.qml`, `MorphMenu.qml`, `MorphPlainContent.qml`, `MorphChevron.qml` (new), `MorphTextInput.qml`, `MorphBadge.qml`, `MorphDialog.qml`, `MorphDrawer.qml`, `MorphSectionHeader.qml`, `MorphCheckbox.qml`, `MorphCheckIndicator.qml`, `MorphSteps.qml`, `MorphSplitterHandle.qml`, `src/qt_quick/CMakeLists.txt`; terminal `leaf_widgets.*` |
| I-V, vocabulary and containers | G-q, G-c and `pageInset`, `bleed`, G-r, then G-n's key (`headJoined`, `pinned`) and `toolbarJoined`'s frontends | `view.hpp`; `mount.hpp`; `testing/recording_backend.hpp`; `testing/backend_conformance.hpp`; Qt Quick `MorphWidget.qml`, `MorphStack.qml`, `MorphPanel.qml`, `MorphTabs.qml`, `MorphBanner.qml`, `widget.cpp`, `containers.cpp`, `leaves.cpp`, `kinds.hpp`; terminal `widget.cpp` (`invertsSurface`), `container_widgets.*`, `layout.*`, `display_widgets.*` (the banner), `list_widgets.*` (`TabsImpl`) |
| I-S, shell and head | G-h (with its four tests and the splitters), R1, G-a (with R2 and G-i), G-b, G-g (`headSurface`, `iconOnly`, S3), G-n (screen, head, workspace, frame, client) | `shell_frame.hpp`; `workspace.hpp`; `host.hpp`; `src/client/shell.cpp`; `tests/qt_quick/test_qt_quick_frontend.cpp`; `tests/tui/test_tui_frontend.cpp` |

`tests/ui_echoing_backend.hpp` forwards only the `create…` calls, so no lane changes it.

**Landing order: I-Q, then I-V, then I-S.** The lanes work in parallel. Each lands on `feat/lims-gallery` with one
commit per item, green on its own. I-V rebases onto I-Q, and I-S onto I-V, before landing.

- **I-V waits for nothing** from I-Q in its framework code. Its screens' pagers (G-m) are written with framework icon
  names that already exist, and are verified after I-Q lands. I-V's `runsHtml` change (G-r) and I-Q's do not meet:
  `runsHtml` is in `MorphWidget.qml`, which I-V owns.
- **I-S waits for:**
  - I-V's `headJoined` key, `ContainerOptions::headJoined` and `pinned`'s writing it, for `Screen::headJoined()` and
    its tests;
  - I-V's `toolbarJoined` in `Theme.qml`, `MorphWidget` and the terminal, for the frame's surface tests on Qt Quick
    and the terminal;
  - I-V's `bleed`, `pageInset` and banner keys, for its screens (receipt, review, console);
  - I-Q's `iconSourceIn`, for console's white scan icon.

  I-S writes the code that depends on them after its rebase. Its headless tests (`test_ui_shell_frame`,
  `test_ui_workspace`) record surface names and need nothing from the other lanes.

### Shared files

Every file two lanes touch has a named region per lane. A later lane rebases onto the earlier lane's version. Within
a region, rows and cases are appended, never reordered.

| Shared file | I-Q's region | I-V's region | I-S's region |
|---|---|---|---|
| `Theme.qml` | `collapsibleMarker` beside `iconOnlySquare`; `iconGlyphs`, `iconSvgs` and `iconSourceIn` beside `iconSource` | `toolbarJoinedScheme`, and the `toolbarJoined` case of `scheme()` and `draws()` | — |
| `document.hpp` | — | `kContainerKeys` (`headJoined`), `kTabsKeys` (`pageInset`), the every-node keys (`bleed`) and their parent check, the banner's keys and the runs' `action`, the `row`'s `divided` refusal | the `Document` struct and its top-level loading (`leadPlacement`, `headSurface`, `principalPlacement`), the app document's refusal list, the `actionButton` part (`iconOnly`) |
| `interpreter.hpp` | — | the lowering of common layout (`bleed`), banners and runs (`actionStyle`, `action`), tabs (`pageInset`) and stacks (`divided`) | `LeadPlacement`, `PrincipalPlacement`, `ActionModel::iconOnly`, `lowerActionParts`, `actionBarOf`, `HeadAction`, `Screen::leadPlacement()`, `headSurface()`, `headJoined()` with `shownRoot()`, `AppRoot::principalPlacement()` |
| `author.hpp` | — | `ContainerOptions` (`headJoined`), `pinned`, `TabsOptions::pageInset`, `Node::bleed()`, `BannerOptions`, `actionRun` | `Scope::leadPlacement`, `Scope::headSurface`, `ActionButtonOptions::iconOnly` |
| `backend.hpp` | — | `BannerWidget::setActionStyle`, `BannerActionStyle`, `TabsWidget::setPageInset`, `StackWidget::setDivided`'s doc | `widthClassOf`, `kExpandedWidthUnits` (G-h) |
| `docs/spec/ui/backend_contract.md` | — | the `StackWidget` row (a divided row), "Layout hints", the `BannerWidget` and `TabsWidget` rows | "The width class" paragraph (G-h) |
| `docs/spec/ui/document.md` | the `dialog` row (G-e) | "Every node", the `column, row`, `tabs` and `banner` rows, the runs' keys, "A bar that stays in view", the load-time rules | "The document", "The head", the `actionButton` table |
| `docs/spec/ui/theme.md` | the closed set, the On Qt Quick table, Framework names and Icons | the Surfaces table | — |
| `docs/spec/ui/author.md` | — | containers, `pinned`, `tabs`, `bleed`, `banner` | `Scope`, the app document, `ActionButtonOptions` |
| `docs/spec/qt_quick/backend.md`, `docs/spec/tui/frontend.md` | the Button, Menu, Text input, Badge, Text, Dialog, Drawer, Checkbox, Steps, Splitter, Collapsible and Panel rows | the Column, Row, Stack, Layout, Tabs and Banner rows, and the surfaces | the top bar's surface |
| `tests/test_ui_author.cpp` | — | `pinned`'s `headJoined`, `bleed()`, `pageInset`, the banner's keys | `leadPlacement`, `headSurface`, `iconOnly` |
| `tests/test_ui_recording_backend.cpp` | — | `bleed`, `pageInset`, `actionStyle` recorded | the width-class bounds (G-h) |
| `tests/test_ui_document.cpp`, `tests/qt_quick/test_qt_quick_containers.cpp`, `tests/qt_quick/test_qt_quick_widgets.cpp`, `tests/qt_quick/test_qt_quick_theme.cpp` | new `TEST_CASE`s appended at the end, and S10's check inside its existing case | appended after I-Q's | appended after I-V's; G-h's edit of the width-class case at `test_qt_quick_widgets.cpp:1088–1097` |
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
| `plate.cpp` | I-S | G-a (`inline`), G-n, G-h's recheck |
| `review.cpp` | I-S | R3, G-a's head members, G-f (`bleed`, `pageInset`), G-n, G-h's splitter |
| `console.cpp` | I-S | G-a, G-g (`headSurface`, `iconOnly`), G-r, G-m (the pager), G-h's splitter |
| `lims_gallery.cpp` | I-S | G-j's and G-g's theme entries, the scan icon, G-a's `principalPlacement` |

I-Q and I-V verify their items with uncommitted edits to screens they do not own, such as I-Q's
`collapsible.marker: none` in the bundle. I-S runs the final re-grab after every lane has landed and records the result
in `examples/lims_gallery/design/visual-review.md`. It covers:

- all ten screens at 1000×830 and 390×844;
- samples with nothing selected, at 390 (H7's hidden bulk bar);
- samples' and audit's expanded pagers, scrolled into view.
