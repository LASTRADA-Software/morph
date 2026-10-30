# LIMS gallery batch G: component looks and text

Batch G of the LIMS gallery visual review (`examples/lims_gallery/design/visual-review.md`, items G1 to G11, and the
"Screen fixes" list), plus G12, the select's dropdown, which the user reported after the review. It covers the look of
badges, the stepper, pinned bars, banners, collapsible sections, table rows and cells, the plate's wells, toned text
and runs, tab overflow, inputs and selects.

The aim is the review's: ui/1 itself produces each mockup's look. So every difference is a vocabulary or frontend
change, and a screen changes only where the vocabulary already says what the design says. Batch F (layout, theme
tokens, containers, F1 to F14) lands first. G builds on the tokens and container options F introduces, and names
each by its F item and F's name for it.

Every claim below was checked against `d5292bc0a` (`feat/lims-gallery`, built as `build/linux-everything`). Each
item says whether its premise was **reproduced** (run here, or measured in the review's grabs) or **read** (inferred
from the code). The review's measurements come from its grabs (`lg-cmp/app/*.png` at 1000×830 and 390×844, and the
`lg-vc*-*.png` grabs) and are quoted as the review's.

## Probes

Five throwaway probes were run with `qml6` (Qt 6.12.0, offscreen) and kept out of the tree (`lg-dG-probe1.qml` to
`lg-dG-probe5.qml` in the session scratchpad).

**1. Wrapping and the placeholder.** A Basic `TextField` with a weight-600 placeholder, and a 60 px `Text` holding
`S-26-104170` under each wrap mode:

```
PROBE placeholder italic=false family=Sans Serif weight=600 style=
PROBE Wrap lines=2 W=57.5
PROBE WordWrap lines=1 contentW=79.0625 implicitW=79.0625
PROBE NoWrap+elide lines=1 truncated=true
```

The grab of the same field shows the placeholder **slanted**, although `font.italic` is false.

**2. Weight to face.** The same text at weights 400 to 700, read back through `fontInfo`:

```
PROBE Sans Serif 400 -> italic=false style=Regular family=Nimbus Sans [UKWN]
PROBE Sans Serif 500 -> italic=false style=Italic family=Nimbus Sans [URW ]
PROBE Sans Serif 600 -> italic=false style=Bold Italic family=Nimbus Sans [URW ]
PROBE Sans Serif 700 -> italic=false style=Bold family=Nimbus Sans [UKWN]
PROBE monospace 600 -> italic=false style=Bold family=Nimbus Mono PS
```

On this machine, fontconfig holds two foundries of Nimbus Sans, and Qt matches a 500 or 600 request to the URW
foundry's italic faces. Every 500 and 600 weight therefore draws italic here.

**3. A dashed outline.** A `Canvas` strokes a rounded rectangle 44×40 with radius 19.25 after `setLineDash([3, 2])`.
It draws dashes 4 to 5 px long, with gaps of 2 to 3 px, along the top edge. The dash lengths are multiples of the
line width, as `QPen`'s are.

A `QtQuick.Shapes` `PathRectangle` draws the same outline, but it is not an option:

- morph does not link `Qt6::QuickShapes` (`src/qt_quick/CMakeLists.txt:83,131`);
- `PathRectangle` needs Qt 6.8, and morph requires 6.5 (`CMakeLists.txt:706`).

So this batch takes the Canvas route, which `MorphChart.qml:150` already uses for dashes under the 6.5 floor. Qt 6.5
itself was not run.

**4. The same, at `QT_SCALE_FACTOR=2`, and a Basic combo box's inset.** At scale 2 the Canvas reports `canvasSize
QSizeF(44, 30)` and draws dashes 9 device px on and 6 off along a 2 px stroke. That is 4.5 by 3 logical px, rendered
at device resolution, so a dashed outline stays crisp on a dense screen.

The same probe holds a Basic `ComboBox` whose `background` alone is replaced, as `MorphSelect` replaces it:

```
PROBE combo contentItem.leftPadding=12 dpr=1 canvasSize=QSizeF(44, 30)
PROBE combo contentItem.leftPadding=12 dpr=2 canvasSize=QSizeF(44, 30)
```

The review's `lg-rG-combo.qml` gives the same 12 px.

**5. Expanded state.** The `Accessible` attached properties `expandable` and `expanded` are settable and read back on
6.12:

```
PROBE expandable=true expanded=false
```

## The items

| Item | Where it lands | Verdict on the review |
|---|---|---|
| G1 | badge look; `radius.badge` | Right (reproduced in the review's grabs). The neutral tag's colours are F9's, not G's |
| G2 | `steps` look and `labelled`; `stepperParts` | Right. **Incomplete:** `labelled: false` blanks the labels, so the indicator loses its accessible names (read) |
| G3 | a `bar` surface; `pinned` writes it; `padding` on `column` and `row` | Right. Needs F7's `divided`, and a container inset, which G adds |
| G4 | banner look; the dialog's title and footer rule | Right. 10-8's "`banner.text` takes no spans" is **wrong**: `banner` has `spans` (`document.md:239`) |
| G5 | one section header for `collapsible` and a collapsible `panel` | Right. **The card form is already in ui/1:** `panel {collapsible}` (read) |
| G6 | table row marks; themed select boxes | Right (reproduced in the review's grabs) |
| G7 | mono text never wraps; a column's `wrap`; a mono column's `fixed(n)` in mono characters | Right (reproduced: the probe's `Wrap` breaks the ID, and `WordWrap` and `NoWrap` do not) |
| G8 | `shape`, `size`, `mono` on button and badge; `border.style` per surface; a round node's surface | Right. This **reverses** batch D's "no node carries a shape" |
| G9 | `tone` on text and on runs; `subtitleSpans`, `descriptionSpans` | Right |
| G10 | a Qt Quick tab overflow menu; `moreLabel` | Right. Batch D declined it in ui/1 and said a frontend may add it; this is that frontend change, plus one label |
| G11 | `mono`, `align`, `prefix` on `textInput`; the prominent weight | **The italic is environmental** (reproduced: weight 600 matches an italic face here). The fix is still the framework's: draw the theme's strong weight |
| G12 | the select's field inset, its popup and its options | Reported by the user, not by the review. The field's value sits **12 px** in today (probe 4), not 4 px |

## The shape of the batch

Four rules hold throughout.

- **A look is a token or a rule, never a screen's choice of pixels.** Where a mockup gives px, the design states the
  token or derived rule that yields them at the default scale, and quotes the px.
- **Keys are shared across kinds.** One key means the same thing on every kind that takes it, and no key is the
  gallery's alone:
  - `mono` on `button`, `badge` and `textInput` is the `mono` text role's face;
  - `shape: round` on `button` and `badge` is one corner rule;
  - `tone` on `text` and on a run is the input's `tone` colour.
- **The strong weight is the theme's one emphasis weight.** The mockups use 600 for tags, numbers in circles and
  titles. Every `MorphUi` text draws emphasis at F12's `font.weight.strong` (`Theme.strongWeight`, default 700), and
  G never asks for a literal 500 or 600. Probe 2 shows why: such a request can land on an italic face where the
  installed family lacks that weight. A theme chooses 600 only when its family has a 600 face.
- **The terminal keeps its cells.**
  - A purely visual change (a radius, a circle, a dash, a colour) changes nothing on the terminal, and each item
    says so.
  - A change that alters what a user can read or reach (labels, elision, density) is drawn on both frontends.

## Dependencies on batch F

The names below are F's, from `2026-10-29-lims-gallery-batch-f-design.md` and the Decisions of both designs.

| G item | Needs | F item | F's name | Without it |
|---|---|---|---|---|
| G1, G2, G8 | a small type step in the strong weight | F12 | `font.size.small`; `Theme.smallPixels`, `Theme.smallFont`, `Theme.smallStrongFont` | tags and circle numbers stay at body size |
| every strong text | the strong weight | F12 | `font.weight.strong`, `Theme.strongWeight`; the gallery keeps 700 (Decisions) | — |
| G9 | a display role for KPI numbers | F12 | `role: "display"` (`TextRole::Display`), `font.size.display` | the KPI "7" is a heading-sized err text |
| G1, G2, G6, G8, G12 | a control border distinct from the divider | F11 | `color.controlBorder`, `Theme.controlBorder` | outlines use `color.border` (`#DDE1E6`) and nearly vanish |
| G3, G4, G5 | a hairline colour | F11 | `color.divider`, `Theme.divider` (the gallery's `#EDF0F3`) | `color.border` |
| G1, G8, G11, G12 | controls white on the page fill | F10 | `color.controlSurface` (a button's, an input's, a select's fill) | chips, wells, inputs and selects take `#F6F7F9` |
| G5 | a framed section on the card fill | F10 | a panel draws on `card` | the card is the page colour |
| G1, G4, G6 | tone parts read from a bundle | F8 | `color.<tone>.bg`, `.border`, `.dot` | the gallery's tone fills stay the Theme.qml defaults |
| G1 | the neutral tone's fill and text | F9 | `color.neutral` with `.bg`, `.border`, `.dot` | the neutral tag keeps `muted` on a faint `muted` fill |
| G12 | a hovered option's fill | F9 | `color.hover` | the Basic palette's highlight |
| G3, G5 | a hairline between a column's shown children | F7 | `divided: true` on `column` and a vertical `forEach` (`setDivided`; the terminal draws no rule) | bars and plate sections have no separator |
| G7 | table cell padding | F7 | `table.cellPaddingX` (8 px), `table.cellPaddingY` | a mono `fixed(n)` counts no padding |
| G4, G10 | ghost buttons in the accent | F13 | the ghost emphasis's accent text | the banner action and More read black |
| G6, G8 | the selected fill | F13 | `color.selection` | as today |
| G10, G12 | a themed popup that stays inside the window | F3 | `MorphMenu`'s popup background and placement; `elevation.popup` | menus and lists can overflow the window |
| G3 | a stretching root and splitter panes with no page inset | F5, F6 | (layout) | bars are inset, not full-bleed |
| G8 | a wrapping row for the plate's legend | F4 | `wrap` on `row` | the legend may clip on compact |

Three points are settled with batch F (Decisions):

- **The strong weight.** `font.weight.strong` stays 700 in the gallery.
- **The panel title rule.** G5 removes it, and F11 drops its test of the rule's colour.
- **The dialog's title size and footer rule.** These are E3 and E5, which F hands over; G owns them (in G4).

G does not read F9's `color.hover` for the stepper, which has no hover state in the design. It reads it for G12's
options.

G adds one token of its own (`radius.badge`), one built-in surface (`bar`), and one per-surface token
(`border.style`). None of them overlaps an F item.

## Parts, lanes and the order of shared files

### Framework files

The twelve items split into three lanes, each owning its components' QML and widget files:

| Lane | Items, in order | Owns |
|---|---|---|
| G-C, structure | G2, G3, G5, G10 | `MorphSteps`, `MorphCollapsible`, `MorphPanel`, new `MorphSectionHeader`, `MorphTabs`; `author.hpp`'s `stepper` and `pinned`; terminal `StepsImpl`, `CollapsibleImpl`, `TabsImpl`, and the stack's padding (G3) |
| G-B, text and tables | G9, G4 (with the dialog's title and footer rule), G7, G6 | `MorphText`, `MorphBanner`, `MorphKeyValue`, `MorphDialog`, `MorphDrawer`, `MorphTable`, `MorphTableRow`, `MorphTableRowBackground`, `MorphCheckbox`, new `MorphCheckIndicator`; `fields.cpp`, `leaves.cpp` (runs); `workspace.hpp`'s head; `host.hpp`'s `ScreenHead`; terminal `paintRuns`, `TextImpl`, `TableImpl` |
| G-A, controls | G1, G8, G11, G12 | `MorphBadge`, `MorphButton`, `MorphButtonBackground`, `MorphTextInput`, `MorphInputBackground`, `MorphSelect`, `MorphMenu` and `MorphDateTimeInput` (the shared popup background), new `MorphDashedOutline` and `MorphPopupBackground`; terminal `BadgeImpl`, `ButtonImpl`, `TextInputImpl` |

**Landing order: G-C, then G-B, then G-A.** The lanes work in parallel. Each lands on `feat/lims-gallery` after F,
rebased on the lane before it, one commit per item, green on its own.

The order decides every file two lanes touch. The later lane rebases onto the earlier one's version:

| Shared file | First | Then | What each adds |
|---|---|---|---|
| `Theme.qml` | G-C (`barScheme`, `draws("bar")`) | G-B (`monoAdvance`), then G-A (`radiusBadge`, `borderStyle`) | appended properties only |
| `MorphScheme.qml` | — | G-A (`borderDashed`) | — |
| `MorphWidget.qml` | G-B (`runsHtml`) | G-A (the surface background's shape and dash) | separate functions and items |
| `theme.hpp`, `theme.md`, `src/qt_quick/theme.cpp` | G-C (the `bar` surface row) | G-A (`radius.badge`, `border.style`) | rows of the closed set and the Surfaces table |
| `document.hpp` (`kKinds`), `view.hpp`, `backend.hpp`, `author.hpp`, `recording_backend.hpp`, `EchoingBackend` (`tests/test_ui_common.cpp`), `backend_conformance.hpp`, Qt Quick `kinds.hpp`, terminal widget headers | G-C | G-B, then G-A | each lane only its own kinds' rows, cases appended at the end |
| `document.md`, `view_tree.md`, `backend_contract.md`, `qt_quick/backend.md`, `tui/frontend.md`, `author.md` | G-C | G-B, then G-A | each lane only its own kinds' rows |
| `MorphDialog.qml` | F (F14's scrim) | G-B | — |
| `MorphSelect.qml` | F (F9's segmented track) | G-A | — |
| `MorphMenu.qml` | F (F3, F9) | G-A (moves F3's inline background into `MorphPopupBackground`) | — |

### Screens

Each screen file has one owning lane, which makes every edit on it. It does so after the items it uses have landed,
which the landing order guarantees, because an owner never lands before a lane it uses:

| Screen file | Owner | Uses |
|---|---|---|
| `stability.cpp` | G-A | G2, G3, G5 (G-C); G11's `prefix`, `unit` |
| `plate.cpp` | G-A | G8 (wells, legend, markers); G5's sections (G-C); G9's subtitle runs (G-B); the repeated line |
| `receipt.cpp` | G-A | G11's scan field; G4 and G9's banner runs and mono IDs (G-B) |
| `lims_gallery.cpp` (the theme bundle) | G-A | G8's well surfaces; `neutral` as a fill |
| `console.cpp` | G-B | G9 (KPIs, chain of custody); the compact card; G5's sections (G-C); G11's `align` (G-A is later, so console's value inputs move in a G-A follow-up commit) |
| `instrument.cpp` | G-B | G9 (IDs, breadcrumb); G5's sections and the booking caption (G-C) |
| `results.cpp`, `review.cpp`, `samples.cpp`, `audit.cpp`, `esignature.cpp` | G-B | G4, G6, G7, G9; G3's footer; results' `align` in the same G-A follow-up |

The one exception to "after the items it uses" is G11's `align` on console's and results' value inputs. G-A makes
that change itself, in one commit after G-B has landed. It edits two lines in each file and nothing else of them.

G-C therefore commits no screen code. It verifies its items with uncommitted screen changes, and the final re-grab
pass (below) runs after all three lanes.

## G1: a badge is a tag

**Problem (reproduced in the review's grabs; read).** Every tag in the gallery is a 24 px pill with a border, in the
body font. Every mockup draws an 11 px/600 tag with radius 4 and no border (`.tag{font-size:11px;font-weight:600;
padding:1px 6px;border-radius:4px}`; one file adds `border:1px solid transparent`, which makes it 2 px taller). The
cause is in `MorphBadge.qml`:

- `radius: height / 2` (`:40`);
- a filled badge is outlined in `toneBorderOf` (`:42`);
- the label is drawn in `Theme.bodyFont` (`:63`);
- the height is the label plus `2 × space.1` (`:35`).

The design has two other forms:

- **The outlined tag** is white with a coloured border (`.pri`, `.p-stat{color:#9B1C1C;border-color:#D98C8C}`).
- **The filter chip** is white with a 1 px `#C3C9D2` border and radius 4, 26 px tall, with a leading inset of 8 px
  and a muted 26×24 `×` (`.chip{padding:0 0 0 8px}`). Audit draws its chips as removable badges (`audit.cpp:279`).

**Decision: no new key.** A badge's one look becomes the tag, and its two existing variants take the design's
forms.

- **Filled** (the default):
  - the fill is the tone's (`toneBgOf`), with **no border** (`border.width: 0`);
  - the text is the tone's colour, in F12's small step and the strong weight (`Theme.smallStrongFont`);
  - the radius is the new token `radius.badge` (default 4);
  - the inset is 1 px above and below, and `1.5 × space.1` (6 px) at each side;
  - at an 11 px step, the tag is about 17 px tall.
- **Outline** (`style: outline`):
  - the fill is the surface's;
  - the border is 1 px in the tone's outline colour (`toneBorderOf`, `color.<tone>.border`), or F11's
    `controlBorder` for a neutral badge;
  - the text is the tone's colour, in the filled tag's font, radius and inset.

  Today the outline's border is `toneOf`, the text colour. The design's `#D98C8C` lies between the two, nearer
  `toneBorderOf` (an RGB distance of about 78, against 170 for `toneOf`, for err). `toneBorderOf` is also the colour
  the theme already names for a tone's outline, so G1 uses it.
- **A removable badge is a chip**, whatever its style:
  - the fill is `color.controlSurface` (F10);
  - the border is 1 px in `controlBorder` when neutral, and the tone's outline colour otherwise;
  - the text is `scheme.text` when neutral, and the tone's colour otherwise, in the **body** font at regular weight,
    since a chip is a control's label, not a status;
  - the height is `Theme.targetHeight − 6` (26 px at the default 32), the radius is `radius.badge`, and the leading
    inset is `space.2` (8 px);
  - the remove button is square, as tall as the chip less 2 px, and draws its `×` in `muted`.

**Theme.** `radius.badge` (a dimension, default 4 px) joins `kThemeTokens` (`theme.hpp:991`, which grows from 46 to
47 entries) and maps to `Theme.radiusBadge`.

**Per backend.**

- **Qt Quick:** `MorphBadge`, as decided.
- **Terminal:** unchanged (`[text]`, `(text)`, `[text ×]`). A cell has no radius, size or weight.
- **`RecordingBackend`:** nothing changes, since no key changed.

**Spec.**

- `document.md:240`, the `badge` row: an outline badge has "the surface's fill, with the tone's outline colour as
  border and its colour as text".
- `theme.md`:
  - the closed set and the On Qt Quick table gain `radius.badge`;
  - `:206`, "a badge and a banner fill and outline with them", becomes "a filled badge fills with them, an outline
    badge, a chip and a banner outline with them".
- `qt_quick/backend.md:179`, the Badge row: a tag in the small strong step with radius `radius.badge`, filled
  without a border; a removable badge is drawn as a chip.

**Tests.** Each has its control.

- `tests/qt_quick/test_qt_quick_theme.cpp:194-199` checks today that a filled `ok` badge's `border.color` is
  `successBorder`. With `border.width: 0` that check would still pass while drawing nothing. So it changes:
  - the colour check moves to an `outline` `ok` badge;
  - the filled badge's check becomes `border.width == 0`.

  **Control:** put the filled border back, and the width check fails.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - A filled `ok` badge's `fill` radius is `Theme.radiusBadge`. Its label's pixel size is `Theme.smallPixels`, its
    weight is `Theme.strongWeight`, and the badge is at most `label.implicitHeight + 2` tall. **Control:** put
    `radius: height / 2` back, and the radius check fails.
  - A removable neutral badge's border is `controlBorder`, and its height is `targetHeight − 6`. **Control:** draw it
    as a tag.
- `tests/test_ui_theme_values.cpp`: a bundle with `radius.badge` of 2 px reads 2, and
  `tests/qt_quick/test_qt_quick_theme.cpp` sees that value on `Theme.radiusBadge`. **Control:** leave the path out of
  `kThemeTokens`, and the value reads the default 4.

**Gallery.** No screen change. Every tag already goes through `layout::tag` and `layout::outlineTag`
(`screen_layout.hpp:61-70`), and audit's chips are already removable badges.

**Verify.** Re-grab `samples`, `console`, `instrument` and `audit` at 1000×830 and 390×844:

```
build/linux-everything/examples/client/morph_examples_client --app lims-gallery --screen lims-gallery.samples \
  --grab-size 1000x830 --grab lg-G1-samples.png
```

The tags are at most 18 px tall with square-ish corners, and the chips are 26 px tall and white, with the control
border. Compare with `05-Samples.png` and `04-Audit.png`.

**Risk.** Every application's badges change from pills to tags (Decisions).

## G2: the stepper's look, and its parts

**Problem.**

- **The look (reproduced in the review's grabs; read).** `MorphSteps.qml:14-45` draws one `Label` per step: a glyph
  from `marker()`, then the label. The result is `✓ ● ○ ○`, with no numbers and no connectors, and every label is in
  `scheme.text`. `MorphSteps` sets no `Accessible` property, so a step is read only through its label's text. The
  design (`06-Stability.dc.html`):
  - 22 px circles with a 1.5 px `#C3C9D2` outline and a 12 px/600 number;
  - **done**: filled `#17784A`, with a white ✓;
  - **current**: an accent outline and an accent number, with the label in the text colour;
  - **upcoming**: a grey outline and number, with a muted label;
  - 2 px connectors that share the width (`flex:1; min-width:20px`): accent after a done step, and `#D2D7DE`
    otherwise.
- **The labels (read).** `stepper`'s `labelled = false` writes an empty `label` for each step (`author.hpp:4709`). So
  an unlabelled indicator names no step to assistive technology, and the terminal's probe text for the current step
  is `""` (`StepsImpl::probeText`, `display_widgets.cpp:321`).
- **The footer (read).** `stepper` returns one `column` of indicator, switch and footer (`author.hpp:4753`), and
  stability passes the whole wizard to `pinned` as its content (`stability.cpp:580`, `:617`). So the indicator and the
  footer scroll with the step: a screen cannot hand the parts to `pinned`'s bars.
- **Continue's label (read).** `continueLabel` is one text for every step, so the design's "Continue to tests" cannot
  be said. The compact footer's Continue fills the width (`flex:1`), and the helper's footer has a spacer.

**Decision.**

1. **The look**, on Qt Quick, from the existing `items` and `current`. A step's number is its position plus one, and
   "current" is `index === current`. A step whose state is `active` but which is not `current` draws as upcoming.
   - **The circle** has a diameter of `round(1.7 × bodyPixels)` (22 px at 13 px) and a 1.5 px border: F11's
     `controlBorder` when upcoming, the accent when current. A done circle is filled with the accent, and a failed one
     with `error`.
   - **The mark inside** is the number in `Theme.smallStrongFont` (the accent when current, `muted` when upcoming),
     or a white ✓ (done) or ✗ (failed).
   - **The label** sits `space.2` after the circle, in the body font: `scheme.text` while current, `muted` otherwise.
   - **A connector** sits between two steps. It stretches (`Layout.fillWidth`, at least 3 units wide), is 2 px tall,
     and is in the accent when the step before it is done, and `controlBorder` otherwise. The design's `#D2D7DE` is
     nearer the gallery's control border `#C3C9D2` (an RGB distance of about 24) than its divider `#EDF0F3` (about 42).
   - **The indicator** fills its row's width (`fillsAcross`, as it does now), so the connectors take the spare width.
2. **`steps` takes `labelled`** (a condition, default `true`). While it is false, the indicator draws only the marks
   and the connectors. Each step keeps its label as its accessible name:
   - Each step's item has `Accessible.role: Accessible.ListItem` and `Accessible.name` set to the label and the
     state ("Conditions and timepoints, current"; "Product and lot, done"; "Tests per timepoint"). The drawn number,
     ✓ or ✗ is `Accessible.ignored`.
   - The indicator itself is `Accessible.List`, named by the current step's label, as the terminal's probe text is.

   `stepper` passes its option to this key instead of blanking the labels. `StepperOptions::labelled` becomes an
   `AnyHandle` condition, so one stepper follows the width class; a `bool` still converts.
3. **`ui::stepperParts(StepperOptions)`** returns `StepperParts{Node indicator; Node body; Node footer;}`: the
   `steps` node, the `switch`, and the footer row.
   - `ui::stepper` is the `column` of the three.
   - A screen places the parts with `pinned({parts.body}, {.top = {parts.indicator}, .bottom = {parts.footer}})`.
   - A `stepper` node is still not proposed, for batch D's reason: every part is an existing node.
4. **`StepperStep::continueLabel`** (an optional text) is Continue's text while that step is current. It defaults to
   the options' `continueLabel`, or `finishLabel` on the last step.
5. **`StepperOptions::continueStretches`** (a `bool`, default `false`): the footer has no spacer, and Continue
   stretches to the spare width, as the compact design draws it.

**Vocabulary.**

- The `steps` node gains `labelled` (a condition).
- In the view tree, `Steps` gains `Prop<bool> labelled = true`.
- `StepsWidget::setLabelled(bool)` is pure, and the mount calls it only when the document gives `labelled`.

**Per backend.**

- **Qt Quick:** as decided.
- **Terminal:**
  - Labelled, the list is as now, one row per step.
  - Unlabelled, the marks sit in **one** row, joined by `─` that fills the width (`[x]──[>]──[ ]──[ ]`), so the
    indicator's `naturalSize` drops from N rows to 1.
  - The probe text is the current step's label in both.
- **`RecordingBackend`:** records `labelled=false`.

**Spec.**

- `document.md`, the `steps` row (`labelled`);
- `view_tree.md`, Steps;
- `backend_contract.md`, `StepsWidget`;
- `author.md`: `stepper`, `stepperParts`, the new options;
- `qt_quick/backend.md:181` (the look and the accessible names) and `tui/frontend.md:102` (the unlabelled row).

**Tests.**

- **Conformance case** "an unlabelled steps indicator still names its current step". Steps `Product` (done) and
  `Conditions` (active), current 1, with `setLabelled(false)`: `textOf` is `Conditions`. **Control:** a backend that
  clears the labels while unlabelled (as the helper's old blanking does) reads `""`.
- `tests/qt_quick/test_qt_quick_widgets.cpp`: four steps, current 1.
  - One circle is filled with the accent; one has an accent border and the text "2"; two have `controlBorder`.
  - There are three connectors of equal width, greater than 0, the first in the accent.
  - The last mark's right edge equals the indicator's right edge, within 1 px.

  **Control:** connectors without `Layout.fillWidth` leave the marks packed at the start, and the edge check fails.
- `tests/qt_quick/test_qt_quick_widgets.cpp`, accessibility: with `setLabelled(false)`, the second step's
  `Accessible.name` is `Conditions, current`. **Control:** name the item by its drawn number.
- `tests/tui/test_tui_display.cpp`: unlabelled four steps at 30 columns draw one row whose last cell is a mark.
  **Control:** the row-per-step list.
- `tests/test_ui_author.cpp`:
  - With the default `labelled`, `stepper` writes the same JSON as before. **Control:** reorder the parts, and the
    test fails. With `labelled: false`, the JSON keeps the labels and adds `labelled: false` to `steps`, which is the
    one intended change.
  - `stepperParts` returns the three nodes.
  - A step's `continueLabel` appears in Continue's `cond`.
  - `continueStretches` drops the spacer and sizes Continue to stretch.

**Gallery** (stability, G-A).

- **Expanded:** the parts go into `pinned`, the indicator in `top` and the footer in `bottom` (G3 gives the bars
  their look).
- **Compact:** `top` holds the "Step 2 of 4" line, the step title and the unlabelled indicator. `bottom` holds the
  footer with `continueStretches`.
- Step 1's `continueLabel` is "Continue to tests".

**Verify.** Re-grab `stability` at 1000×830 and 390×844. The circles are about 22 px, the connectors span the row,
and the footer sits at the window's bottom with a rule above it. Compare with `06-Stability.png`.

## G3: `pinned` bars look like bars

**Problem (reproduced in the review's grabs; read).** `pinned` (`author.hpp:3907-3921`) writes its bars as plain
children of its column, with no fill of their own and no separator. The review measured stability's footer on the
page's `#F6F7F9`, with no rule. The design draws each bar white, with a 1 px `#E1E5EA` rule on the side facing the
content, inset 12 px by 20 px (`06`, `05`, `08`, `02`).

**Decision.**

- **A built-in surface, `bar`:** background `color.surface`, no border, no radius, no shadow. It differs from
  `toolbar`, which outlines all four sides and would draw lines at the window's edges. A theme index's `bar`
  replaces it, as with any built-in surface.
- **What `pinned` writes.** Each non-empty bar list becomes one `column` of its nodes on `PinnedOptions::surface`
  (default `"bar"`), with `padding: 2`. `.surface = ""` writes the same column with no surface, a choice today's
  helper does not have, since it does not wrap the bars at all. Empty bars are left out, so no stray rule is drawn.
- **The outer column is `divided` (F7)**, so a hairline separates the top bar, the content and the bottom bar.
- **`padding` on `column` and `row`** (F7 adds `gap` to panels, dialogs, drawers and collapsibles, not `padding` to
  stacks). It is a spacing step, 0 to 5, as a panel's padding is, and it insets the children on every side. Console's
  KPI tiles (`.kpi{padding:10px 12px}`) and the plate's selection bar use it too. In the view tree, `Column` and `Row`
  gain `int padding = 0`, and `StackWidget::setPadding(int)` is pure.
- **The step is 2, not the design's 12 px.** Step 3 (12 px) costs a terminal row above and below
  (`tui/frontend.md:173-176`), so every pinned footer would gain two blank rows. Step 2 is 8 px on Qt Quick and 0
  rows on the terminal. The residual is an 8 px inset where the design has 12 by 20.

**Per backend.**

- **Qt Quick:** `Theme.qml` gains `barScheme` and `draws("bar")`, and the surface background in `MorphWidget` draws no
  border for it. `MorphStack` insets by `Theme.spaceOf(padding)`.
- **Terminal:**
  - `bar` is not inverted (only `sidebar` and `toolbar` are, `widget.cpp:212`).
  - The bars have no separator, since F7's divided column draws no rule on the terminal. Their place at the column's
    ends is what marks them.
  - A stack's padding maps to cells as a panel's does.
- **`RecordingBackend`:** records the surface name, as for any surface, and `padding=2`.

**Residual colour.** The rules draw in the gallery's `color.divider`, `#EDF0F3`, lighter than the mockup's `#E1E5EA`.
The mockups use three near values for rules (`#E1E5EA`, `#DDE1E6`, `#EDF0F3`), and the theme has one divider. That is
a theme value, not a vocabulary gap.

**Spec.**

- `theme.md`, Surfaces: the `bar` row.
- `author.md`, `pinned`: the bar column, the surface option, the divided column.
- `document.md`'s "A bar that stays in view": `pinned` draws the bars as bars, and a hand-made column draws them as
  written.
- For `padding`: the `column, row` row in `document.md`; `view_tree.md`; `backend_contract.md` (`StackWidget`); and
  the Column and Row rows of both frontends.

**Tests.**

- `tests/test_ui_author.cpp`: `pinned(content, {.bottom = …})` writes a divided column of the scroll and a `bar`
  column with padding 2. With `.surface = ""` it writes that column with no surface. **Control:** the old helper
  writes no bar column, and the test fails.
- `tests/qt_quick/test_qt_quick_containers.cpp`: a 400×300 page holding `pinned` with a bottom bar. A pixel just
  inside the bar reads `Theme.surface`, and the row of pixels at the bar's top edge reads `Theme.divider`.
  **Control:** a bar with no surface reads the page colour.
- **Layout case** "a column's padding insets its children": a column with padding 3 holding a text; the text's
  bounds start one step in. **Control:** a backend that ignores `padding`.

**Gallery.** No screen code: receipt, samples, stability, results and review already put their bars in `pinned`.
Results also moves its expanded footer into `bottom` (Screen fixes).

**Verify.** Re-grab `stability`, `samples`, `results` and `receipt` at both sizes. Each bottom bar is white with a
rule above it. Compare with the mockups' footers.

## G4: a banner's look, and the dialog's title and footer

### The banner

**Problem (reproduced in the review's grabs; read).**

- `MorphBanner.qml:24-29` fills a `Rectangle` with no `radius`.
- The message `Label` (`:48-55`) has no colour, so it draws in the palette's black.
- The action is the Basic style's grey `Button` (the review's S6-9).

The review measured square corners and black text on esignature and receipt. The design has:

- radius 6;
- the message in the tone's colour (`.b-err{color:#8A1C1C}` and `.b-warn{color:#6B4100}`, close to the gallery's
  `#9B1C1C` and `#7A4A00`);
- an inset of 8 px by 10 px.

**Decision.**

- **The frame** (`objectName: "frame"`) has radius `Theme.radiusControl` and is inset `space.2` vertically and
  `space.3` horizontally: 8 px by 12 px, against the design's 10 px at the sides.
- **The message**, runs included, is in `toneOf(tone)`; a neutral banner's is in `scheme.text`. Strong runs stay
  strong in that colour.
- **The action** is drawn as a ghost button (`MorphButtonBackground` with `ghost`, its label in F13's accent), at the
  message's end. An inline link run, such as the design's "Containers per pull", is not in this batch (see Not in
  this batch).

**Per backend.** Qt Quick only. The terminal already draws the whole banner in `toneStyle`
(`display_widgets.cpp:120-128`).

**Spec.** `qt_quick/backend.md:181`: the banner's frame, colour and action.

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`, for an `err` banner:

- `frame`'s radius is `Theme.radiusControl`;
- the `message` label's colour is `Theme.error`;
- the `action` is drawn on a ghost `MorphButtonBackground`.

**Control:** each change reverted alone fails its check.

**Gallery** (G-B). Receipt's "Already received" banner uses `spans`: a strong "Already received:" and the mono ID.
Esignature's banner uses a strong run (Screen fixes).

**Verify.** Re-grab `esignature`, `receipt` and `results`: rounded banners with toned text. Compare with `Main.png`
and `02-Receipt.png`.

### The dialog's title and footer rule

**Problem (reproduced in the review's grabs; read).** These are the review's E3 and E5, which batch F hands to G.

- **E3:** the dialog's title is drawn in `Theme.strongFont` at body size (`MorphDialog.qml:153-156`), where the
  design draws 16 px/600.
- **E5:** the footer has no rule above it, where the design draws a divider above Cancel and Sign.

**Decision.** No key changes.

- The title of a dialog and of a drawer is drawn in `Theme.headingFont` (F12's heading size, at the strong weight).
- The footer area of a dialog and of a drawer draws a 1 px `Theme.divider` rule above it (`objectName:
  "footerRule"`) while it holds an action.
- The terminal is unchanged, since a cell has one size, and its dialog frame is not changed by this.

**Tests.** `tests/qt_quick/test_qt_quick_overlays.cpp`:

- An open dialog's title has `headingPixels`.
- A dialog with actions shows `footerRule`, and one without actions does not.
- The same two checks for a drawer.

**Control:** `strongFont`, and no rule.

**Verify.** Re-grab `esignature` and `results` with their dialogs open, at 1000×830.

## G5: collapsible sections

**Problem.**

- **Plain (reproduced in the review's grabs; read).** `MorphCollapsible.qml:24-45` draws a borderless `ToolButton`
  in the body font, indented by its own padding, with no least height. The design's sections are 44 px rows in the
  strong weight, flush with the content (`07` compact, with `border-bottom:1px #DDE1E6` between sections).
- **Cards (read).**
  - Instrument, stability and console draw their compact sections as framed cards.
  - ui/1 already has that form: a `panel` with `collapsible`, `collapsed` and `onToggle` (`document.md`, `panel`;
    `view_tree.md:113`). The screens use `collapsible` instead.
  - The collapsible panel's own header is a separate chevron `ToolButton` before a bold `Label`
    (`MorphPanel.qml:41-63`), and every panel draws a rule under its header (`:66-73`). The designs draw none (the
    review's I13 and S6-16).
  - The two kinds report opposite things: a panel fires `toggle` with `collapsed` (`MorphPanel.qml:51`), and a
    collapsible fires it with `open` (`MorphCollapsible.qml:40`).
  - The design's compact card is `padding: 0 12px 8px`, with its `summary{min-height:44px}` flush at the card's top
    (`03-Instrument.dc.html:109-111`).

**Decision: no new key.**

- **One header**, a new `MorphSectionHeader.qml`, drawn by `collapsible` and by a collapsible `panel`:
  - The whole title area is the toggle, at least `Theme.targetHeight` tall (32 px; 44 px on compact).
  - The title is in `Theme.strongFont`, flush with the content's leading edge.
  - The chevron (`nav.expand` or `nav.collapse`, or today's glyph) sits at the trailing edge in `muted` (Decisions).
  - The `header` slot sits between the title and the chevron and keeps its own clicks.
- **Accessibility.** The toggle stays the node's `focusTarget`, with:
  - `Accessible.role: Accessible.Button`, named by the title;
  - `Accessible.expandable: true`, and `Accessible.expanded` following the open state (probe 5);
  - Space and Enter toggling it, as a click does.
- **Insets.** A collapsible panel's header is inset horizontally by the panel's padding and not vertically. The
  padding applies above the content, at its sides and below it, which puts the 44 px header flush at the frame's top,
  as in the design.
- **No header rule.** A panel draws no rule under its header. A screen that wants one writes F7's `divided`.
- **Which form.** The card is the collapsible panel, which a screen chooses. The plain form is `collapsible`,
  separated by the enclosing column's `divided`. The two kinds keep their own `toggle` meanings, which are documented
  and wired today.

**Per backend.**

- **Qt Quick:** as decided.
- **Terminal:** the title is bold, and a panel draws no rule under its header. Otherwise it is as now (`▾ title`); the
  chevron stays first, where the cursor reads it.
- **`RecordingBackend`:** nothing changes.

**Spec.**

- `qt_quick/backend.md:183` and the Panel row: the shared header, its accessible role and state, the chevron's place,
  the collapsible panel's insets, no header rule;
- `tui/frontend.md:107`: the bold title.

**Tests.** `tests/qt_quick/test_qt_quick_containers.cpp`:

- A collapsible's toggle is at least `Theme.targetHeight` tall. Its title font is bold, and its title's x equals its
  content's x.
- A titled panel draws no 1 px rule between its header and its content: no item 1 px tall lies between the header's
  bottom and the content's top, whatever its name.
- A collapsible panel's header top is the frame's top (plus the 1 px border).
- The toggle's `Accessible.expanded` flips with a click, and Space on it toggles the section.

**Control:** the old `MorphCollapsible` fails the height and flush checks, and the old `MorphPanel` fails the rule and
the header-top checks.

**Gallery.**

- Instrument compact (`instrument.cpp:355-363`, G-B), stability compact (`stability.cpp:595`, `:611`, G-A) and console
  compact (G-B) use `panel({…}, {.title, .collapsible = true, .collapsed = !open, .onToggle})` in place of
  `collapsible`.
- The handlers invert the event, since a panel reports `collapsed`. Stability's `conditionSection`, for example,
  becomes `cond(!Handle<bool>{event()}, rowId, "")`.
- A gallery test (`examples/lims_gallery/tests`) clicks a closed section and finds it open. **Control:** the handler
  without the inversion leaves it closed.
- Plate compact (G-A) keeps `collapsible`, inside a `divided` column (`plate.cpp`, `compactLayout`).

**Verify.** Re-grab `instrument`, `stability`, `console` and `plate` at 390×844. The sections are framed cards (or
divided rows, on plate) with 44 px strong headers. Compare with the compact halves of `03`, `06`, `10` and `07`.

**Risk.** The rule under every panel title goes away, in every application.

## G6: table row marks and select boxes

**Problem (reproduced in the review's grabs; read).**

- **The marks.** `MorphTableRowBackground.qml:59-73` fills a selected row with `selection`, which hides a toned
  row's fill, and a toned row always draws a 3 px leading bar in the tone's dot colour (specified in
  `qt_quick/backend.md:306`). The review measured samples' selected rows at `#DCEFE3` with no bar, and out-of-spec
  rows with a red bar and a `#FDECEA` tint.

  The designs (`05`, `08`, `10`) give the two marks the other way round: a selected row has a 3 px accent bar, and
  an out-of-spec row a pale `#FFF5F5` tint with no bar. Samples (multiple selection with a select column) draws no
  fill on a selected row. Console (single selection) draws `#EAF5EE` and the bar.
- **The boxes.** `MorphTableRow.qml:199` and `MorphTable.qml:364` instantiate the Basic style's `CheckBox`, not the
  themed box that `MorphCheckbox` draws.

**Decision: no new key.**

- **A selected row** draws the accent's 3 px leading bar (`selectionBar`). It also takes `scheme.selection` as its
  fill, **unless the table has a select column**, whose box already shows the state. That rule reproduces both
  designs.
- **A toned row** draws a tint and no bar. The tint is the tone's fill at half strength over the table's surface
  (`Qt.tint(scheme.surface, Qt.alpha(toneBg, 0.5))`): `#FDE7E7` over white is `#FEF3F3`, beside the design's
  `#FFF5F5`. It is derived rather than a token, so a tone keeps the parts F8 loads and gains none.
- **The marks compose**: the tint, then the selection fill when one is drawn, then the bar.
- **Both select boxes** are drawn by a new `MorphCheckIndicator.qml`: the indicator `MorphCheckbox` draws today,
  factored out, with a partial state (a dash) for select-all, and F11's `controlBorder` while unchecked.

**Per backend.** Qt Quick only. The terminal's marks (`*`, `[x]`, `>` and the tone's style) already differ by kind
(`table_widget.cpp:397-410`).

**Spec.** `qt_quick/backend.md:306`, the Table row: the selection bar, the fill rule, the tint, the themed boxes. The
sentence "a toned row … draws a 3 px leading bar in its `dot` colour; a selected toned row shows the selection colour
and keeps the bar" is replaced.

**Tests.** `tests/qt_quick/test_qt_quick_table.cpp`.

- Today's `toneBar` checks (`:366-383`) change: `toneBar` goes, and the same rows check the tint and that no bar is
  drawn for a tone.
- A selected row of a table with a select column shows `selectionBar` and a transparent fill. Without a select
  column, its fill is `Theme.selection`.
- `selectBox` and `selectAll` are `MorphCheckIndicator`s whose unchecked border is `controlBorder`.

**Control:** the old background fails the first two checks.

**Gallery.** None: samples, results, console and review already set `rowTone` and the selection.

**Verify.** Re-grab `samples` and `console` at 1000×830, and compare the selected and out-of-spec rows with `05` and
`10`.

## G7: table cells that keep identifiers whole

**Problem.**

- **Wrapping (reproduced by probe 1; read).** `MorphText.qml:41` sets `wrapMode: Text.Wrap`, which is
  `WrapAtWordBoundaryOrAnywhere`. A token wider than its line is broken anywhere: the probe's 60 px `S-26-104170` takes
  2 lines, as the review's grabs show ("S-26-10417 / 0" on samples, review and console; "WS-2026-041 / 2" on audit).
- **The width (read).** A `fixed(n)` column is `n × Theme.unitWidth` (`MorphTable.qml:90`), the body font's average
  character width. A mono run's characters are each the mono face's advance, which is wider here (Nimbus Mono PS). So
  `fixed(12)` does not hold an 11-character mono ID (`samples.cpp:501`).

**Decision.**

- **A `mono` text never wraps.** It is one line, elided at its end when it does not fit. The mono role is "an
  identifier" (`document.md:218`), and an identifier broken across lines reads as two.
  - While the text is elided, its full text is its tooltip, unless the node gives its own `tooltip`, which wins.
  - Its accessible name and probe text are always the full text.
  - This holds for a `text` whose `role` is `mono`. A mono **run** inside `spans` is not a mono text: rich text is
    not elided, so it wraps with its sentence. Samples' card ID and receipt's IDs are runs, and each sits in a line
    wide enough for it.
- **A table column takes `wrap`** (a literal `bool`, default `true`). With `false`, the column's default text cells
  are drawn on one line and elided, as above. Like `role`, it styles the default cell only. "99.4 % LC" is the use.
- **A mono column's `fixed(n)` holds n mono characters, plus the cell's padding.** For a column whose `role` is
  `mono`, a unit is the mono face's advance (`Theme.monoAdvance`, from `FontMetrics` on `Theme.monoFont`), and F7's
  `table.cellPaddingX` is added on both sides. A column's `role` decides this width unit **even when the column has
  its own `cell`**, so a column that draws a mono text in its own cell (audit's Record) says `role: "mono"` to get it.
  Every other `fixed(n)` column is unchanged: n body units and no padding, as today. Leaving the rest unchanged keeps
  every other application's fixed columns at their width.

**Vocabulary.** The table column part gains `wrap`, and the view tree's `TableColumn` gains `bool wrap = true`.
Columns already reach the backend whole (`setColumns`), so there is no new virtual. `document.md`'s "role … has no
effect on one with its own `cell`" becomes "role styles the default cell, and a `mono` role also sets the column's
width unit, with or without its own `cell`".

**Per backend.**

- **Qt Quick:** in `MorphText` (role 6, and a `singleLine` flag that the row sets from the column) and in
  `MorphTable.columnWidths`, which reads `column.role` from the column record it already receives.
- **Terminal:** a cell or a mono text cut at its width draws `…` in its last cell. A fixed width is already n cells,
  and every face is the cell's.
- **`RecordingBackend`:** records `wrap=false` on the column.

**Spec.**

- `document.md:235`, table columns: `wrap`, and what `fixed` counts in a mono column;
- `document.md:218`: the mono role never wraps;
- `view_tree.md`, TableColumn;
- `qt_quick/backend.md` and `tui/frontend.md`: Text and Table.

**Tests.**

- `tests/qt_quick/test_qt_quick_table.cpp`: a `fixed(11)` mono column holding `S-26-104170` shows it with
  `truncated == false`, and its row is one line tall. **Control:** measure the column in `Theme.unitWidth`, and the
  label is truncated. Without the mono rule it would instead wrap and pass the first check, which is why this is a
  Qt Quick test, not only a layout case.
- **Layout case** "a column with `wrap: false` keeps its cells to one line": "99.4 % LC" in a `fixed(5)` normal
  column; the row is as tall as one holding `x`. **Control:** wrap anyway.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - An elided mono text's tooltip is its full text, and a node's own `tooltip` wins over it. **Control:** no tooltip.
  - A mono run inside a 60 px `text` with `spans` still wraps (two lines), which keeps the runs rule as stated.
- Corpus: `wrap` on a column loads, and a non-literal `wrap` is refused.
- **Checked unchanged:** `test_qt_quick_table.cpp`'s fixed-width cases (`:53`, `:486`, `:507`, `:565`, the wide-table
  case at `:594`, which compares `columnsWidth` with `80 × unit`) and the conformance table cases
  (`backend_conformance.hpp:2002`, `:2043`, `:2079`) use normal columns only, so their widths do not change.

**Gallery** (each in its owning lane, G-B).

- The ID columns keep `fixed(n)`, with n the ID's length: samples, review, console.
- Audit's Record column gains `role: "mono"` beside its own `cell` (`audit.cpp:327-333`), and keeps `fixed(13)` for
  `WS-2026-0412`.
- Columns of values with units take `wrap: false`.
- The headers that elide ("Specifica…", "Previou…") take content width (Screen fixes).

**Verify.** Re-grab `samples`, `review`, `console` and `audit` at 1000×830. No ID breaks, and the rows are one line.

**Risks.**

- A mono text that used to wrap is now elided. A long code block in `mono` is not an identifier, and should be spans
  or a text area.
- A mono `fixed(n)` column grows by `2 × table.cellPaddingX` plus the mono face's extra width, so a table of several
  mono columns may reach `hflick` sooner. Only mono columns change.

## G8: the plate's wells, legend and markers

**Problem (reproduced in the review's grabs; read).**

- **Shape.** Batch D left shapes out of ui/1 ("no node carries a shape"). The gallery draws wells as push buttons on
  surfaces (`plate.cpp:344-370`, `lims_gallery.cpp:72-91`, "no surface draws a dashed outline").
- **A square behind the well.** `MorphWidget.qml:297-304` draws a node's surface background as a full-size
  `Rectangle` (`z: -1`) with radius 0 for every surface but `card`. It is drawn whenever `Theme.draws(surface)` holds,
  which includes the gallery's `sample`, `empty`, `control`, `standard` and `blank`. A rounded button background would
  sit on that square.
- **The colours after F.** F10 fills a normal button with `color.controlSurface`, and F11 outlines it in
  `color.controlBorder`. The gallery's well surfaces set only `color.surface`, `color.border` and `color.text`. So
  after F, the QC, standard and blank wells would draw white with the generic control border, and their colour would
  survive only in the square behind them.
- **Padding.** A push button's 16 px side padding (`MorphButton.qml:100-101`) leaves about 20 px of a 54 px track for
  its label. So `STD1` to `STD8`, and at 1000 px `S10` and above, elide to "S…".
- **The design** (`07`):
  - wells are 44×40 circles with a mono 10 px/600 label;
  - sample wells are white, with a 1.5 px `#9AA2AE` outline;
  - empty wells have a **dashed** `#C3C9D2` outline;
  - a selected well has a 3 px accent ring outside;
  - the legend's chips are 26 px circles;
  - each compact line starts with a 36 px circular marker.

**Decision.** There are three keys, shared by `button` and `badge` (`mono` also by `textInput`, G11), and one
per-surface token. No `well` node: a well is a selectable, focusable, droppable button, which ui/1 already has. What
it lacks is a shape and a density that any control may want: a round icon button, a dense toolbar.

- **`shape`** on `button` and `badge`: `default` or `round`, a literal name.
  - The corners are half the height.
  - A content-width node is at least as wide as it is tall, so a short label gives a circle and a longer one a
    stadium (the design's 44×40 well).
  - **A round node draws its surface background with its own shape.** `MorphWidget`'s surface `Rectangle` takes the
    node's radius (half its height) while the node is round, so no square shows behind a circle.
  - A selected push button's ring follows the shape.
- **`size`** on `button`: `normal` or `small`, a literal name. A small button is dense: its side inset is `space.1`,
  and its label is in F12's small step. It keeps the control height, and the touch height on compact, so its target
  is never smaller.
- **`mono`** on `button`, `badge` and `textInput`: a literal `bool`. The label or the text is drawn in the mono
  role's face.
- **`border.style`** (DTCG `strokeStyle`; `solid` or `dashed`, default `solid`) is read per surface, like every token.
  - On a surface whose style is `dashed`, the **control outlines** are stroked with dashes in place of their solid
    border: the 1 px `border` of `MorphButtonBackground`, of `MorphBadge`'s fill, and of `MorphInputBackground`.
  - The surface's own background `Rectangle` draws no border on such surfaces (`border.width: 0` except for `card`
    and `toolbar`), so it has nothing to dash. A dashed `card` is not drawn dashed in this batch, since no design
    asks for one.
  - The dashes are drawn by a new `MorphDashedOutline.qml`: a `Canvas` that strokes a rounded rectangle with
    `setLineDash([3, 2])` (probes 3 and 4), and calls `requestPaint()` when its width, height, radius or colour
    changes.
  - Any other `strokeStyle` value is refused at `addFile` (`border.style: only solid and dashed are drawn`). That is
    the vocal choice over drawing it solid.
- **The selection ring** is 3 px wide, outside the border (2 px today), with radius `radius + 3`.

**The selected tint.** A selected push button that is not filled keeps its tint (`MorphButtonBackground.qml:33`;
after F13, `scheme.selection`). The design draws none, but the tint is what tells selection from focus while a
theme's focus and accent colours are one colour, as the gallery's are (Decisions).

**Residuals.**

- The well outline stays 1 px, against the design's 1.5 px.
- The well is 32 px tall (the control height), against the design's 40 px.
- The compact marker is 44 px (the touch height), against the design's 36 px.

All three follow from sizing by the control's height, not by px.

**Vocabulary.**

- In the view tree, `Button` gains `shape`, `size` and `mono`, and `Badge` gains `shape` and `mono`, all set once.
- `ButtonWidget::setShape`, `setSize` and `setMono`, and `BadgeWidget::setShape` and `setMono`, are pure. The mount
  calls each only when the document gives the key.
- Author: `ButtonOptions::shape`, `size` and `mono`; `BadgeOptions::style`, `shape` and `mono`.
- Theme: `border.style` joins the closed set as a new kind, `Stroke`, with its own `ThemeValues::strokes` map, and
  `MorphScheme.borderDashed`.

**Per backend.**

- **Qt Quick:** as decided.
- **Terminal:**
  - `small` drops the push button's inner spaces: `[S1]` in place of `[ S1 ]` (`leaf_widgets.cpp:212`).
  - `shape`, `mono` and `border.style` draw nothing new: a cell has no corners, every face is mono, and a terminal
    button has no outline.
- **`RecordingBackend`:** records `shape=round`, `size=small` and `mono=true`.

**Spec.**

- `document.md`: the `button` and `badge` rows (and `textInput` for `mono`, G11);
- `view_tree.md`;
- `backend_contract.md`: the `ButtonWidget` and `BadgeWidget` rows;
- `theme.md`: the closed set (`border.style`), and Surfaces (a dashed surface; a round node's surface background);
- `qt_quick/backend.md` and `tui/frontend.md`.

**Tests.**

- **Layout case** "a small push button is narrower than a normal one with the same label", on both frontends.
  **Control:** a backend that ignores `size`.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - A round badge `QC` has radius `height / 2`, and its width is at least its height.
  - A round button on a drawn surface: its surface `Rectangle`'s radius is half the button's height, and a pixel at
    the button's corner reads the page colour, not the surface's. **Control:** put `radius: 0` back.
  - A small mono button `STD1` in a 6-unit grid track does not elide its label (`truncated` is false). **Control:**
    with `size: normal`, it elides.
  - A button on a dashed surface shows a visible `MorphDashedOutline`, and its `MorphButtonBackground` border width is
    0. **Control:** read the token and ignore it.
  - The dashed test runs once more under `QT_SCALE_FACTOR=2`, where the outline's `canvasSize` is the item's size and
    the stroke has gaps (as probe 4 showed).
- `tests/test_ui_theme_values.cpp` and `tests/test_ui_theme.cpp`: `border.style: dashed` on a surface layer reads
  `dashed`, and `dotted` is refused at `addFile`. **Control:** accept any string.
- Corpus: `shape` and `size` take only their names, and `mono` only a literal `bool`.

**Gallery** (plate and `lims_gallery.cpp`, G-A).

- **Wells:** `button({…, .shape = Shape::Round, .size = ButtonSize::Small, .mono = true})` on fixed 6-unit tracks, in
  place of stretch (`plate.cpp:375-383`, whose comment then goes).
- **Surfaces.** Each well surface sets the control roles a button reads after F, from `07`'s `.w-*` CSS. The
  surface's `color.surface` stays as today (it is what the round surface background fills).

  | Surface | `color.controlSurface` | `color.controlBorder` | `color.text` | `border.style` |
  |---|---|---|---|---|
  | `sample` | `#FFFFFF` | `#9AA2AE` | `#1C1E21` | — |
  | `empty` | `#FFFFFF` | `#C3C9D2` | `#5F6673` | `dashed` |
  | `control` (QC) | `#FFF1D6` | `#D19100` | `#6B4100` | — |
  | `standard` | `#E6ECFF` | `#5B7BE0` | `#1F3FA8` | — |
  | `blank` | `#EEF0F3` | `#7A828F` | `#363C46` | — |

- **Legend:** round mono outline badges, each on its well's surface. A neutral outline badge's border is
  `controlBorder` and its text is `scheme.text`, so the surface's roles colour it. The legend row wraps (F4).
- **Compact lines** start with the same well button (on compact, a 44 px circle), so a tap selects, as the grid does.

**Verify.** Re-grab `plate` at 1000×830, 1280×900 and 390×844:

- every well label reads in full;
- the wells are circles or stadiums, with no square behind them;
- the empty wells are dashed, and the selected wells ringed;
- the standard, QC and blank wells keep their colours.

Compare with `07-Plate.dc.html`'s CSS. Its PNG shows template placeholders for the grid.

## G9: tones on text and runs, and runs in heads and descriptions

**Problem (read; reproduced in the review's grabs).**

- **Text.** A text's role is one enum that decides its size, weight and colour together (`MorphText.qml:42-49`), and
  a `text` refuses `tone`. That is stated in `document.md` ("A `text` refuses both") and in
  `tests/test_ui_document.cpp:349`, and each frontend throws on it: `TextImpl::setTone` in Qt Quick
  (`fields.cpp:65-68`), in the terminal (`leaf_widgets.cpp:99`) and in `RecordingBackend` (`recording_backend.hpp:948`).
  So console's "Past due **7**" can be heading-sized or red, not both; the review measured it at body size and regular
  weight.
- **Runs.** A run has only `strong` and `mono` (`view.hpp:493-504`). So the chain of custody's coloured dots, and a
  red run inside a sentence, cannot be said.
- **The head and the dialog.** The head's `subtitle` is a plain string (`workspace.hpp:776-795`, `host.hpp:64`), and
  so is a dialog's `description`. So instrument's breadcrumb with a mono ID, plate's mono worksheet ID and results'
  change line with bold values cannot be drawn.
- **A theme applied later (read, not run).** Qt Quick builds a run list's HTML in C++, with the mono families read
  once at `setSpans` (`fields.cpp:73`, `leaves.cpp:247`, and `:369` for a key-value item). A theme applied after the
  mount would leave the mounted runs in the old family. G9 fixes this as part of the change below, so it is not filed
  separately.

**Decision.**

- **`text` takes `tone`** (`ok`, `warn`, `err`, `info`; `neutral` or `null` for none).
  - The tone is the text's colour, whatever its role. The role keeps the text's size, weight and face.
  - `role: error` and `role: success` stay, as a normal text in that colour.
  - With F12's `display` role, the KPI is `{role: display, tone: err}`.
  - `text` still refuses `description`.
- **A run takes `tone`.** A run is `{text, strong, mono, tone}`, with `tone` as on `text`.
  - At load, an `obj` run may have the key `tone`, and a literal tone must be a known name.
  - A computed tone of no known name is reported once at the node's `spans`, and the run is drawn untoned.
  - This holds wherever runs are drawn: `text`, `banner`, `keyValue` values, and the two new places below.
- **The head takes `subtitleSpans`**, an expression of runs, which a client that knows it draws in place of
  `subtitle`. `subtitle` beside it is what an older client draws, as `text` beside `spans` is.
  - `ScreenHead::subtitleSpans()` is a virtual with a default that answers no runs, so a host screen's head compiles
    unchanged.
  - The workspace's head `Text` takes the runs.
- **`dialog` and `drawer` take `descriptionSpans`**, likewise.
  - `DialogWidget::setDescriptionSpans` and `DrawerWidget::setDescriptionSpans` are pure.
  - The accessible description is the runs' joined text.
- **On Qt Quick, a run list's HTML is built in QML.** C++ writes `runs`, with each run's text already escaped, and a
  `MorphWidget.runsHtml(runs)` binding builds the markup from `toneOf`, `Theme.monoFamilies` and
  `Theme.strongWeight`. So a theme applied later restyles the mounted runs.

**Per backend.**

- **Qt Quick:** as decided. `TextImpl::setTone` writes the tone.
- **Terminal:**
  - A run's tone is its style's foreground (`toneStyle`, in `paintRuns`, `widget.cpp:821`).
  - A toned text is drawn in the tone's foreground, with its role's boldness.
  - The head and the dialog's description draw runs as a text does.
  - `TextImpl::setTone` draws instead of throwing.
- **`RecordingBackend`:** records `tone=err` on a text and on a run, the head's runs through its text, and
  `descriptionSpans`. `setTone` records instead of throwing.

**Spec.**

- `document.md`:
  - the opening paragraph's "A `text` refuses both" becomes "a `text` refuses `description`";
  - the `text` row (`tone`);
  - Spans (`tone`);
  - The head (`subtitleSpans`);
  - the `dialog, drawer` row (`descriptionSpans`).
- `view_tree.md`, `TextSpan`.
- `backend_contract.md:274`: "a text refuses the last two" becomes "a text refuses `setDescription`"; and the dialog
  and drawer rows.
- `workspace.md`, the head.
- `interpreter.md`, the spans lowering.

**Tests.**

- **Conformance:** the existing case "a Text with spans reads as its runs' characters" gains a toned run.
  **Control:** a backend that drops a run carrying a tone fails it.
- `tests/test_ui_document.cpp:349`: the test "an input takes a description and a tone, and a text refuses them" is
  split. A text with `tone` now loads, and a text with `description` is still refused.
- Corpus:
  - a run `{"text": "7", "tone": "err"}` loads;
  - a literal run tone `pink` is refused;
  - a computed one is reported once and drawn untoned;
  - `subtitleSpans` and `descriptionSpans` load.
- `tests/test_ui_mount.cpp`, on `RecordingBackend`: the tone reaches `setTone` on a text. **Control:** the mount
  skipping it.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - A `heading` text with `tone: err` has `headingFont` and the colour `Theme.error`.
  - After `applyTheme` sets a new `color.err`, a mounted toned run's markup carries the new colour. **Control:** the
    C++-built HTML keeps the old one.
- `tests/tui/test_tui_widgets.cpp`: a toned run's cells have the tone's foreground.
- `tests/test_ui_workspace.cpp`: a head with `subtitleSpans` mounts a head text with those runs.

**Gallery** (G-B, except plate's subtitle, which G-A writes).

- **Console:**
  - the KPI values use `role: display`, and "Past due" adds `tone: err`;
  - each chain-of-custody line is runs: a toned `●`, a strong step name, then the plain rest.
- **Instrument:** `subtitleSpans` "Instruments › ", then a mono `EQ-0142`.
- **Plate:** the subtitle with the mono worksheet ID.
- **Results:** the dialog's `descriptionSpans`, "S-26-104170 · Impurity B: **0.12 %** → **0.21 %**", with the ID
  mono and the values strong.

**Verify.** Re-grab `console` at 1000×830 (the red 22 px "7", the coloured dots), then `instrument` and `results`.

## G10: tabs that do not fit go into "More"

**Problem (reproduced in the review's grabs; read).** `MorphTabs.qml` is a flickable Basic `TabBar`, which batch D made
scroll the chosen tab into view. On the instrument at 390 px, "Audit trail" is off the right edge and reached only by
flicking; at 320 px, "Bookings" is too. The design (`03` compact) draws Overview, Calibration, Maintenance and
"More ▾".

Batch D declined an overflow menu in ui/1, and said that a frontend may draw one with no vocabulary change. This item
is that frontend change, plus the menu's label.

**Decision.**

- **Where More lives.** More is a sibling of the `TabBar`, at its trailing end in the same row, and not a
  `TabButton` inside it. So `bar.count` stays the tab count, and `bar.currentIndex` stays the chosen tab's index,
  which `select(index)` and `highlighted` rely on (`MorphTabs.qml:16-20`).
- **What fits.** The bar shows the leading tabs that fit beside More whenever the tab buttons' widths exceed the
  row's. An overflowed `TabButton` is `visible: false`, so it leaves the bar, the focus chain and the accessibility
  tree.
  - The fit is computed against More at its **widest** label: the wider of `moreLabel` and the longest overflowable
    tab's label, plus `▾`. More's label then changes without changing what fits, so there is no feedback loop.
  - It is recomputed on every width change, so a wider window brings the tabs back.
  - A row whose tabs all fit hides More and is as today. The bar's `keepChosenInView` stays: it still matters while
    no tab is hidden, and it is a no-op once every shown tab fits.
- **The menu.** More opens a menu of the hidden tabs, using G12's `MorphPopupBackground` and F3's placement, with each
  entry named by its tab's label. Choosing an entry chooses that tab (`fire("select", index)`), exactly as a click on
  its button does.
- **More's label.** While the chosen tab is one of the menu's, More shows that tab's label ("Audit trail ▾") with the
  active underline. Otherwise it shows `moreLabel` and `▾`.
- **Keyboard and accessibility.**
  - More is `Accessible.role: Accessible.Button`, named by its shown label.
  - Right on the last visible tab moves the focus to More, and Left on More moves it back.
  - Enter, Space or Down on More opens the menu.
- **`tabs` gains `moreLabel`** (an expression; default `More`, translated with `t`, in English like `dismissLabel`).
  `TabsWidget::setMoreLabel` is pure.
- **Terminal:** keeps batch D's `‹ ›` markers, which reach every tab with the arrows and a click (Decisions). It stores
  `moreLabel` and draws nothing from it.
- **`RecordingBackend`:** records `moreLabel`.

**Spec.**

- `document.md`, the `tabs` row (`moreLabel`);
- `view_tree.md`, Tabs;
- `backend_contract.md`, `TabsWidget`;
- `qt_quick/backend.md`, Tabs: the overflow, More's place, label, keys and role;
- `tui/frontend.md`: the label is not drawn.

**Tests.** `tests/qt_quick/test_qt_quick_containers.cpp`, with the instrument's five tabs in a 390 px bar:

- Every shown tab button lies inside the bar, More is visible, and `bar.count` is 5.
- Choosing the menu's "Audit trail" fires `select(4)`, and More's text becomes `Audit trail ▾`, with the same tabs
  shown as before.
- Right on the last visible tab focuses More.
- At 1000 px, More is hidden.

**Control:** without the overflow, the last tab's right edge is past the bar.

**Gallery.** None.

**Verify.** Re-grab `instrument` at 390×844, which shows "Overview  Calibration  Maintenance  More ▾".

## G11: inputs: mono, alignment, prefix, and the prominent weight

**Problem.**

- **The italic (reproduced; environmental).** A prominent `textInput` draws in
  `Theme.fontOf(headingPixels, Font.DemiBold)` (`MorphTextInput.qml:54`). On this machine a weight-600 request
  matches Nimbus Sans **Bold Italic** (probe 2), so the scan field's placeholder is slanted. The framework never asks
  for italic; the face it gets is fontconfig's.
- **The rest (read).**
  - The design's scan field is mono, 600, 16 px (`02`), and the input has no mono face.
  - Values sit at the trailing edge (`.input.num{text-align:right}`, `08`, `10`). A text input has `textAlign` and
    `tabularFigures` (`MorphTextInput.qml:28-31`), but only a table column sets them; there is no document key.
  - Stability's "± 7 days" has a leading "±". The input has a trailing `unit`, and no leading text.
  - **Both rings already exist.** `MorphInputBackground.qml:19-44` draws a 2 px ring in the tone's fill for a toned
    input. For a prominent field it draws a 2 px accent border, and a 3 px ring at `Qt.alpha(accent, 0.2)` while it is
    focused.

**Decision.**

- **`mono`** (G8's key) on `textInput`: the text and the placeholder are in the mono face.
- **`align`** on `textInput` (`start`, the default, `center`, `end`; a literal name) places the text in the field. An
  `end` input uses tabular figures, as an end table column's input does.
- **`prefix`** on `textInput` (text) is drawn muted at the leading edge inside the field, and announced before the
  value. It mirrors `unit`, and is refused beside a literal `mode: multiline`, as `unit` is.
- **The prominent field** draws at the heading size and the theme's strong weight (`Theme.strongWeight`, F12), never a
  literal 600. At the default 700, every platform font has an upright face. With `mono`, it uses the mono face at that
  size and weight.
- **The rings stay as they are.** The prominent ring shows while the field is focused. Receipt's scan field keeps the
  focus after every scan, so it shows whenever the mockup draws it. The toned ring already matches the design.

**Vocabulary.**

- In the view tree, `TextInput` gains `mono`, `align` and `prefix`.
- `TextInputWidget::setMono`, `setAlign` and `setPrefix` are pure, and the mount calls each only when the key is
  given.
- Author: `TextInputOptions::mono`, `align` and `prefix`.

**Per backend.**

- **Qt Quick:** as decided.
- **Terminal:**
  - `align: end` draws the text from the field's right edge while it fits.
  - `prefix` is drawn muted before the text.
  - `mono` draws nothing new.
- **`RecordingBackend`:** records the three keys.

**Spec.**

- `document.md:220`, the `textInput` row;
- `view_tree.md`;
- `backend_contract.md`, `TextInputWidget`;
- `qt_quick/backend.md`: the prominent field's weight;
- `tui/frontend.md`.

**Tests.**

- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - `align: end` gives `horizontalAlignment == AlignRight` and tabular figures;
  - `mono` puts the field's family in `Theme.monoFamilies`;
  - a prominent field's font weight is `Theme.strongWeight`;
  - a `prefix` label sits at the leading edge, and the text starts after it.

  **Control:** each change reverted alone. For the weight check that means `Font.DemiBold`, which reads 600.
- `tests/tui/test_tui_fields.cpp`: an end-aligned "0.21" in a 10-cell field ends at the field's last cell, and a prefix
  "±" sits in its first cell. **Control:** draw from the start.
- Corpus: `prefix` beside a literal multiline is refused, and `align` takes only its names.

**Gallery** (G-A).

- **Receipt:** the scan field has `mono: true`.
- **Stability:** the pull window has `prefix: "±"` and `unit: "days"`; per pull has `unit: "units"`.
- **Results and console:** the value inputs take `align: end` with `unit: "%"`, in G-A's one follow-up commit on
  those two files.

**Verify.** Re-grab `receipt` at 1000×830 (an upright mono placeholder), `results` at 390×844 (the values at the
trailing edge) and `stability`.

## G12: the select's field and its dropdown

**Problem.** The user reported that the combo boxes look plainer than the mockups. The mockups draw every dropdown's
field as `.input` (`height:30px; border:1px solid #C3C9D2; border-radius:6px; padding:0 8px; background:#fff`), and
its open list as `.menu` (`background:#fff; border:1px solid #DDE1E6; border-radius:6px; box-shadow:0 8px 24px
rgba(20,24,31,.14); padding:4px`). A highlighted entry is `#F0F3F7` (`.mi.hl`). What `MorphSelect.qml`'s `ComboBox`
draws today:

- **The value sits 12 px in** (reproduced, probe 4: the Basic style's `contentItem.leftPadding` is 12), against the
  design's 8 px. Text inputs inset by `space.3` (12 px).
- **The style's own popup** (read): the Basic style's frame, with no radius and no shadow. It opens at
  `x: 0, y: control.height` with vertical margins of 6 (the review's probe), and F3's placement does not reach it.
- **The style's highlight** for the hovered option (read), from the Basic palette, not a theme token.
- **The current option in `Font.DemiBold`** (`MorphSelect.qml:98`, read), a literal 600, which is italic on this
  machine (probe 2).
- **The chevron in `text`** (read), where the design's is muted.

The field's outline, fill, focus ring and tone ring are already `MorphInputBackground`'s (`MorphSelect` uses it, with
its `tone`). F10 and F11 put that background on `controlSurface` and `controlBorder`. G12 inherits both and claims no
test for them.

**Decision.** Qt Quick only; the terminal's select is unchanged.

- **The field's inset.** The `contentItem`'s left padding is `space.2` (8 px, the design's), and its right padding
  leaves room for the chevron. The chevron is drawn in `muted`.
- **One popup background.** A new `MorphPopupBackground.qml` holds the themed popup surface: the card scheme's
  surface, a 1 px `Theme.divider` border, `radius.control`, `elevation.popup` (F3, F9), and an inset of `space.1`.
  - It is the item F3 writes inline in `MorphMenu`'s `Popup`, moved into its own file.
  - `MorphMenu.qml`, `MorphDateTimeInput.qml` (its picker popup) and `MorphSelect.qml` (as `ComboBox.popup`'s
    `background`) all use it. Basic's `ComboBox.popup` is a `T.Popup`, and the style itself sets its `background`, so
    replacing it is supported.
  - The popup keeps Basic's `ListView` `contentItem`, wired to `highlightedIndex`, so keyboard navigation and the
    high-contrast outline survive.
- **The list's size and place.**
  - It is at least the field's width (the Basic popup's default).
  - It is at most eight rows tall, and then scrolls.
  - F3's end-alignment applies only once the list is wider than the field, and keeps it inside the window.
- **An option:**
  - it is a row `Theme.targetHeight − 2` tall (30 px at the default; the touch height less 2 on compact);
  - it is inset `space.1`, with radius `Theme.radiusBadge`;
  - while hovered or keyboard-highlighted it fills with `color.hover` (F9);
  - the current option is drawn at `Theme.strongWeight`, with a ✓ in the accent at the trailing edge;
  - no Basic palette colour is used.
- **No placeholder.** `select` has no `placeholder` key, and `ComboBox` has none. A select with no choice
  (`currentIndex: -1`) shows an empty field, as now. This batch adds no key for it.

**Per backend.** Qt Quick only. The terminal's select draws as now, and `RecordingBackend` has no change: no key is
added.

**Spec.** `qt_quick/backend.md`: the Select row (the field's inset, the popup background, the option's look, eight
rows) and the Menu row (the shared popup background).

**Tests.** `tests/qt_quick/test_qt_quick_widgets.cpp`:

- The field's `contentItem.leftPadding` is `Theme.spaceOf(2)`. **Control:** the Basic default reads 12.
- The opened popup's `background` is a `MorphPopupBackground` with `Theme.divider` border and `radiusControl`.
  **Control:** the Basic popup's background is a plain `Rectangle` with radius 0.
- With twelve options, the open list is at most `8 × (Theme.targetHeight − 2) + 2 × space.1` tall, and its view
  scrolls. **Control:** no height cap shows all twelve.
- The current option's weight is `Theme.strongWeight`, and it shows the ✓. **Control:** `Font.DemiBold`.
- A highlighted option fills with `color.hover`. **Control:** the palette's highlight.
- `MorphMenu` and `MorphDateTimeInput`'s popups have a `MorphPopupBackground`. **Control:** F3's inline item.

**Gallery.** None: every select picks the look up.

**Verify.** Re-grab plate's Template and "Fill with", audit's user filter, and samples' Status and Priority, against
their mockups. A grab cannot open a list, so the list's look is checked by the tests only.

## Screen fixes

Each fix lands in the lane that owns the screen (see Parts and lanes), after the item it uses.

| Fix | How, after F and G | Owner |
|---|---|---|
| Monospace IDs: receipt (the sample heading, the session table, the last rejection), instrument (the serial, the worksheet), audit (the compact record line), samples (the card IDs) | Receipt: the section heading as `spans` with a mono run (`receipt.cpp:291`), the session table's ID column `role: "mono"` (`:343`), the rejection line as spans (`:385`). Instrument: the serial as a `keyValue` item with mono `spans` (`instrument.cpp:240`, as compact does at `:330`), and the worksheet as spans (`:271`). Audit: the compact record line as spans (`audit.cpp:390-397`), and `role: "mono"` on the Record column (G7). Samples: the card ID as a strong mono run (`samples.cpp:589`). G7 keeps the cells whole, and G9 adds instrument's breadcrumb | receipt G-A; instrument, audit, samples G-B |
| Units through `unit` (stability) | `windowField` and `perPullField` (`stability.cpp:290-306`) take `unit` ("days", "units"), G11's `prefix: "±"`, and plain labels | G-A |
| Plate compact repeats "ELISA, host cell protein" | The compact layout's muted line (`plate.cpp:667`) keeps only "14 samples to place" | G-A |
| Console compact: the card itself is the selectable item | An item button with `.children = {compactCard()}`, as review's queue does (`review.cpp:452-458`); the "Open …" line goes | G-B |
| Results: the footer in `pinned`'s bottom bar; the change line as the dialog's `description` | `expandedLayout` (`results.cpp:528-537`) passes the footer as `bottom`. The dialog takes `description`, and `descriptionSpans` (G9) for the bold values | G-B |
| Console KPI tiles stretch to equal height | `.align = Align::Stretch` on the `kpis` grid (`MorphGrid.qml:147-154` already fills); `padding: 2` on each tile (G3) | G-B |
| Instrument: no visible "Booking elapsed" | The progress gives no `label`, and carries the name in `.a11y("Booking elapsed")` (`instrument.cpp:272`) | G-B |
| Receipt: the lot line's spans read a null sample | The runs go behind `cond(engaged(sample), …)` (`receipt.cpp:313-314`), so no run is `null` once every sample is handled. The runtime still reports a dropped run, as it does today (`interpreter.hpp:6480-6486`). A hidden node's spans are evaluated and reported like every other hidden node's properties, so this needs no framework change | G-A |
| The gallery theme's `neutral` is a fill (`#E7EAEE`) | `lims_gallery.cpp:42` follows F9's split of `color.neutral` into the neutral tone's colour and its parts | G-A |

## Totals

**New document keys:**

- `steps`: `labelled`
- `tabs`: `moreLabel`
- `text`: `tone`; a run: `tone`
- the head: `subtitleSpans`; `dialog` and `drawer`: `descriptionSpans`
- a table column: `wrap`
- `button`: `shape`, `size`, `mono`; `badge`: `shape`, `mono`
- `textInput`: `mono`, `align`, `prefix`
- `column` and `row`: `padding`

**New pure virtuals**, with four implementations each (Qt Quick, terminal, `RecordingBackend`, `EchoingBackend`):

- `StepsWidget::setLabelled`
- `TabsWidget::setMoreLabel`
- `DialogWidget::setDescriptionSpans`, `DrawerWidget::setDescriptionSpans`
- `ButtonWidget::setShape`, `setSize`, `setMono`
- `BadgeWidget::setShape`, `setMono`
- `TextInputWidget::setMono`, `setAlign`, `setPrefix`
- `StackWidget::setPadding`

`TextWidget::setTone` exists already, and stops throwing on all three backends.

**Other API:**

- **`ScreenHead`** gains `subtitleSpans()`, with a default.
- **Theme:** `radius.badge` (a dimension); `border.style` (a new `Stroke` kind); the built-in surface `bar`.
- **Author:**
  - `stepperParts`, `StepperParts`, `StepperStep::continueLabel`, `StepperOptions::continueStretches`;
  - `StepperOptions::labelled` as a condition;
  - `PinnedOptions::surface`;
  - the new options on `ButtonOptions`, `BadgeOptions` and `TextInputOptions`.
- **New QML files:** `MorphSectionHeader`, `MorphCheckIndicator`, `MorphDashedOutline`, `MorphPopupBackground`.

**Conformance**, numbered after F's:

- one new case (G2's labels);
- one existing case extended (G9's toned run);
- three new layout cases: G3's padding, G7's `wrap: false`, G8's density.

## Not in this batch

- **A link run** (the design's underlined "Containers per pull", inside a banner). A run that is a control needs
  focus, a role and a command per run. The banner's action, drawn as a ghost button by G4, says the same thing.
- **A title with runs** (plate's "Plate **PL-0193**" with the ID mono). The title is the shell's and the catalog's,
  not the head's, so the subtitle carries the ID (G9). A title with runs belongs with F5's shell head.
- **Pixel sizes** for wells (40 px) and compact markers (36 px). ui/1 sizes controls by the control and touch heights
  (G8).
- **The drop-target look** of a well being dragged over (`07`'s `.w-drop`). It is a transient state that no grab
  shows, and the existing drop highlight (`MorphWidget.qml`, `dropArea`) stays.
- **An eyebrow subtitle above the title** (results 8-8, instrument's breadcrumb placement). That is head layout, F4
  and F5.
- **A select's placeholder** (G12). No design gives one text for "no choice yet".

## Where this design differs from the review

1. **G11's italic is environmental.** The review lists "an italic placeholder" as a framework look. Reproduced:
   `font.italic` is false, and a weight-600 request lands on Nimbus Sans Bold Italic on this machine. The framework's
   fix is still needed: draw the theme's strong weight, never a literal 600 (`MorphTextInput.qml:54`,
   `MorphSelect.qml:98`). The gallery keeps `font.weight.strong` at 700 (Decisions).
2. **10-8's "`banner.text` takes no spans" is wrong.** `banner` takes `spans` (`document.md:239`,
   `author.hpp:4565-4584`), so that fix is the screen's.
3. **G5's card form exists.** `panel {collapsible}` is a framed accordion section today. G5 changes a look and the
   screens' choice of kind, not the vocabulary.
4. **G8 reverses batch D** ("Shapes … no node carries a shape"), for the reason given there.
5. **G10 is the frontend change batch D allowed**, plus `moreLabel`. The terminal keeps D's markers.
6. **G2 found more than the look:** `labelled: false` strips the indicator's accessible names (read).
7. **G12's premise is corrected.** The select's value sits 12 px in, not 4 px (probe 4), so G12 moves it **closer**
   to the border, to the design's 8 px.

## Review findings not taken as written

Every finding of the review was checked against the code at `d5292bc0a`. All were accepted. These were taken with a
different change than the one the review proposed:

- **S6 (the rings):** the rings stay focus-only rather than always shown, and G11 drops its ring changes and its ring
  test. Receipt's scan field keeps the focus after every scan, so it shows the ring whenever the mockup draws one.
  Drawing it on an unfocused field would make a focal field look focused when it is not.
- **S10 (the bar's padding):** `pinned` writes `padding: 2` rather than keeping step 3. Two blank terminal rows per
  bar cost more than the 4 px on Qt Quick.
- **S5 (padding on every fixed column):** the padding is added to mono columns only, rather than listing the change as
  a risk for every fixed column. Every other fixed column keeps its width. Adding it to all fixed columns would break
  `test_qt_quick_table.cpp:594`, whose sum check assumes a fixed column is exactly `n × unit` wide.

## Decisions

Taken on 2026-10-29, at this design's defaults:

- a muted chevron at a collapsible section's trailing edge;
- the selected tint kept on a selected well, since the gallery's focus and accent are one colour;
- the terminal keeps ‹ › for tab overflow;
- one `padding` step per container;
- tag-shaped badges with `radius.badge` = 4 px, for every application.

Settled with batch F:

- `font.weight.strong` stays 700 in the gallery;
- G5 removes the panel title rule, and F11 drops its test of the rule's colour;
- G owns the dialog's title size and footer rule (G4).

## Defects this batch fixes

- **Mounted runs keep the mono family of the theme at mount time** (`fields.cpp:73`, `leaves.cpp:247`, `:369`). Read,
  not run. G9 moves the markup into QML, so the fix belongs to that change.
- **An unlabelled stepper's indicator has no accessible names** (`author.hpp:4709`). Read. G2 fixes it.

Neither is filed, since each is fixed by the change that touches it.

Reconciled after both revisions: `MorphPopupBackground.qml` is created by batch F's F3, which lands first and already
puts menus, the date picker and the select's dropdown on it; G12 extends that file rather than creating it.
