# LIMS gallery batch C: text, fields and small controls

Batch C of the LIMS gallery initiative (`examples/lims_gallery/design/batches.md`): how a lab form reads. Identifiers
in a monospace face and a bold run inside a sentence; a dialog's description; where the required mark sits; help text
tied to its input; a group of check boxes with a legend; a unit inside an input; a date format a regulated lab accepts;
an outlined destructive button, an outlined tag and a segmented control; label-over-value pairs, a toned value and a
list entry of several lines. It lands on `feat/lims-gallery` as one commit per item, in the order below, and the
gallery drops each workaround in the commit that makes it unnecessary.

Every claim below was checked against the code at `b080a5265` (the branch with batch A landed). The gallery's screens
are cited from that commit (`git show b080a5265:<path>`), since another lane is editing them in the worktree. Each
item says whether its premise was **reproduced** (run on this revision), or **read** (inferred from the code). Where
the gap log was wrong or incomplete, the item says so.

The batch adds one mechanism where it can, and closed-choice values where it cannot:

- **Spans** are the one mechanism for emphasis inside text. A span is a run of text, plain or `strong`, in the body or
  the `mono` face. A `text`, a `banner` and a key-value value take them. Monospace for a whole line is the `mono` text
  role, and its face is the `font.mono` token: both are batch B's (its G2), and this batch cites them.
- **A field's description and tone** are the one mechanism for help text and an evaluation of a value. Every input
  takes them, and a `dialog`, a `drawer` and the new `group` take `description` too. An input's and an overlay's
  description is announced as its accessible description. A group's is a line under its children, read in order,
  since a group lowers to existing kinds and `Common` carries no accessible description.
- **Closed choices grow** where a node already chooses its look: a button's `emphasis`, a badge's and a select's
  `style`, a key-value's `fieldLayout`, a text's `role`.

| Item | Where the fix lands | Verdict on the gap log |
|---|---|---|
| Plain text | the Qt Quick text items | Not in the log. Reproduced: Qt Quick draws a document's text as markup when the text looks like markup |
| G2, G3 | document, interpreter, view tree, the four backends (the `mono` role and `font.mono` are batch B's) | Right; ids sit inside sentences and key-value values, so a run is needed as much as a role. The gap log cites `theme.md:130` for `font.family`, which is at `:132` |
| G24, G39 | document, interpreter, `KeyValue`, the four backends | Right |
| G8 | `MorphField`, the terminal's field marks | Right |
| G10 | document, interpreter, `FieldState`, the four backends, forms engine | Right, except that no Qt Quick input is outlined for its errors either |
| G6 | document, interpreter, `Dialog`/`Drawer`, the four backends | Right; and the Qt Quick dialog presents no accessible name either |
| G17 | document, interpreter (lowered, no new widget), author | Right |
| G9 | document, interpreter, author | Right; the view tree and every backend already draw a unit. Reproduced: an `icon` on a multiline input loads and its mount throws |
| G14, G33, G53 | document, view tree, the four backends | Right. Reproduced: a select's unknown literal `style` draws a dropdown with no report |
| G52 | document, interpreter, `Button`, the four backends | Right |
| G13 | expressions, interpreter, `DateTimeInput`, both frontends | Right; and nothing ever sets the expression formatter's date hook |
| G11 | document, `TextInput`, both frontends | Right; the narrowest item, and the first to drop if the review wants a smaller batch |

Order of landing, after batch B: plain text first, since spans depend on it; then G2/G3, G24/G39 (which read spans), G8 (which
G17's legend needs for its mark), G10, G6, G17, G9, the closed choices (G14, G33, G53), G52, G13 and G11.

Batch B lands first and defines the `mono` text role, the `font.mono` token and `Theme.monoFamilies` (its G2). A
`mono` span draws in that face. Batch C builds on B's G41 too: G52 changes the same button row of `document.md` and
the same **Button style and selection** paragraph of the contract.

The conformance suite grows from 45 cases (after batch B, whose cases are 41–45) to 50, and its probe gains one
reading operation, `descriptionOf`.

## Plain text: Qt Quick draws a document's text as markup (#1058)

**Cause (reproduced).** No Qt Quick `Label` or `Text` in `src/qt_quick/qml` sets `textFormat`
(`grep -rn textFormat src/qt_quick/qml` finds nothing), so each is `Text.AutoText`. AutoText draws a string that
`Qt::mightBeRichText` takes for markup as styled text. A throwaway QML file, run with `qml6` (Qt 6.12.0, offscreen)
and deleted, printed:

```
qml: default textFormat 2 (AutoText=2)
qml: tagged auto width 121.65625 as plain width 472.5 links  linkCount? linkAt api
```

The string was `<b>Approved</b> and <font color='red'>red</font> <a href='https://evil.example'>link</a>`. As
AutoText it is drawn as 121 px of styled text, and the same string as `Text.PlainText` is 472 px of literal
characters. So a sample name, a comment or a reason that holds `<font color=…>` or `<a href=…>` is drawn as markup by
`MorphText`, `MorphBadge`, `MorphKeyValue`, `MorphBanner`, a field's caption and messages, a dialog's title and the
others. Whether an `<img>` in such a string is fetched was not tested: no network in the probe.

The Basic style's own text items are AutoText too, and they show document text: a button's label, a menu entry (an
`ItemDelegate`, `MorphMenu.qml:103`), a check box's label, and every widget's tooltip (the attached `ToolTip`,
`MorphWidget.qml:183-185`, whose content item is a plain `Text`). A second throwaway `qml6` run mounted each with the
same kind of string and printed:

```
qml: Button textFormat 2 width 202.546875
qml: ItemDelegate textFormat 2 width 202.546875
qml: CheckBox textFormat 2 width 236.546875
qml: ToolTip contentItem textFormat 2 width 202.546875
qml: plain width 372.359375
```

This item must land before spans. G3's requirement is that emphasis is never markup read out of data, and today any
data string can be.

**Fix.**

- Every `Label` and `Text` in `src/qt_quick/qml` that shows a document's or a value's text sets
  `textFormat: Text.PlainText`. The only exception is the spans path below, which is `Text.RichText` built from
  escaped text.
- The Basic style's own content items are covered too: `Button`'s and `ItemDelegate`'s `IconLabel`, `CheckBox`'s
  and `RadioButton`'s `CheckLabel`, `ComboBox`'s delegates, `TabButton`'s, and the `ToolTip` that `MorphWidget`
  attaches. `IconLabel` and `CheckLabel` have no `textFormat` of their own, so each such control gets a `contentItem`
  whose text item is `PlainText`, and `MorphWidget` gives its tooltip a `PlainText` content item. A `ComboBox`'s
  closed display text is a `TextField`, which never reads markup.
- The terminal draws every string as cells and has no markup. It is unchanged.

**Spec.** `backend_contract.md`'s Widgets list gains: "Every setter's text is shown as the characters it holds: a
backend never interprets a document's or a value's text as markup." `qt_quick/backend.md`'s Widgets section says
every text item is `PlainText`, and spans are the one `RichText` path.

**Tests** (`tests/qt_quick/test_qt_quick_widgets.cpp`).

- Mount one of each kind whose text props hold `<b>x</b>`: a text, a button, a check box, a radio select and a
  dropdown (opened, so its delegates exist), a badge, a banner, a key-value, an open dialog's title, a panel's
  title, a tab, a field's caption and its errors, a step, an open menu's entry, and a widget whose tooltip is shown.
- Walk the scene, overlays included. Every `QQuickText` whose `text` contains `<b>` has `textFormat` `PlainText`,
  and its `implicitWidth` equals that of a `PlainText` `Text` with the same string and font.
- The walk also counts the items it checked, and the count is at least the number of strings mounted, so a popup
  that never opened cannot pass by being absent.
- **Control:** set one `Label` back to the default (remove its `textFormat`), and the walk names it. Remove the
  tooltip's content item, and the walk names the tooltip.

**Risks.** A deployment that put markup into document text on purpose loses it. Nothing in the tree does:
`git grep -nE '<(b|i|u|font|a|br|p|span|code|img)( [^>]*)?>' -- examples tests/ui src include/morph/ui` finds only
HTML pages the forms web demo and the bookmarks exporter write, which are not document text. Spans replace it.

## G2, G3: monospace identifiers and inline emphasis

**Problem (read; the gap log's reading stands).**

- A `text` node's `role` is one of `normal`, `muted`, `heading`, `section`, `error` and `success`
  (`docs/spec/ui/document.md:164`; `TextRole`, `include/morph/ui/view.hpp:199-206`).
- The theme has one family, `font.family` (`docs/spec/ui/theme.md:132`). Batch B's G2 adds the `mono` role and
  `font.mono`, which cover a whole value in the mono face.
- `text`'s `text` and `banner`'s `text` are single strings (`document.md:164,184`), and a key-value item's `value`
  is turned into one string (`interpreter.hpp:6413-6431`).

The mockups set identifiers in a monospace face in three shapes:

- a whole value, such as a serial number in a key-value (03) or an id in a list (05, 07);
- an id inside a sentence: "Lot `L-26-0381`" and "investigation `OOS-2026-031` linked" (Main), both inside
  key-value values;
- a bold id: `WS-2026-0412` at weight 600 (Main compact, 05, 09, 10).

Bold runs sit inside a banner ("**Password incorrect.** 3 of 5 attempts left…") and inside a muted line ("Signing at
… as **Approved**."). So a role alone covers only the first shape, and emphasis needs runs.

**Decision: spans for runs; a whole line is batch B's `mono` role.**

- A **span** is a run of text with two independent flags, `strong` and `mono`. The node's role still gives the
  colour and size: a muted line with a `mono` span is muted in the mono face.
- **A whole line** in the mono face is the `mono` text role of batch B's G2, on a `text` and on a table column alike.
  A `mono` span is the same face for a run inside a sentence, which a role cannot express.
- **Why not markup in the string**, such as `**bold**` or MessageFormat 2's `{#b}…{/b}`: a markup syntax inside a
  string is parsed, so data interpolated into it can open a run. Spans are a list the document builds. Data is only
  ever the text of a run, and is never parsed. A translated sentence with an emphasised placeholder is the one case
  spans do less well, since a run list fixes the order. Decision 1 covers it.
- **Why `strong` and not italics or colour:** the designs need weight and face only. A colour inside a sentence is a
  status, which a badge or a tone carries.

**Vocabulary.**

```json
{"kind": "text", "role": "muted",
 "spans": {"list": ["Signing at 24-Oct-2026 09:40 CEST as ", {"obj": {"text": {"ref": "meaningLabel"}, "strong": true}}, "."]}}
{"kind": "banner", "tone": "err",
 "spans": {"list": [{"obj": {"text": "Password incorrect.", "strong": true}},
                    {"fn": "format", "args": [" {0} of 5 attempts left before the account locks.", {"ref": "attempts"}]}]}}
{"kind": "text", "role": "mono", "text": {"row": "id"}}
```

- `text` and `banner` take `spans` (an expression). A node may give `text`, `spans`, or both. A client that knows
  `spans` draws them whenever the node has them, and does not read `text`. `text` beside `spans` is what a client
  older than spans draws, so a server gives both when an older client may read the document (Risks, below).
- A key-value item may give `spans` beside or in place of `value` (G24, G39), with the same rule.
- `spans` evaluates to a list. Each element is either a string, which is a plain run, or a record
  `{text, strong?, mono?}`. A run's `text` is shown as `textOf` shows any value, so a number or a quantity formats as
  it does elsewhere. `strong` and `mono` are booleans and default to `false`.
- A `spans` written as a `list` expression is checked element by element at load: a literal element must be a
  string, and an `obj` element must have `text` and no keys but `text`, `strong` and `mono`. A literal `strong` or
  `mono` must be a boolean. Anything else is refused at its path. A value computed at run time that has another shape
  is reported once for the node's `spans` path, and the bad element is dropped.

**Author API** (`include/morph/ui/author.hpp`).

- `TextOptions` and `BannerOptions` gain `AnyHandle spans`, declared right after `text`, so that a call can name
  `.spans` before `.role` (designated initialisers follow declaration order).
- Three helpers build the elements:
  - `ui::strong(AnyHandle text)`: `{"obj": {"text": text, "strong": true}}`;
  - `ui::mono(AnyHandle text)`: the same with `mono`;
  - `ui::styled(AnyHandle text, SpanStyle{.strong, .mono})`: both flags. It is not named `span`, which `Node::span`
    already means: a grid child's column span.
- `ui::spans({…})` is `ui::list` under a name that reads at the call:
  `ui::text({.spans = ui::spans({"Lot ", ui::mono(lot)}), .role = "muted"})`.
- Each helper gets complete Doxygen.

**Theme.** No new token. A `mono` span draws in batch B's `font.mono` face (its G2).

**Lowering** (`interpreter.hpp`).

- `text`: when the node has `spans`, `Text::spans` binds to a slot that evaluates the list and converts each element,
  and `text` is not lowered. Otherwise the node lowers as now.
- `banner`: the same, into `Banner::spans`.
- The conversion is one function, `spansOf(Value, FormatContext, report)`, shared by `text`, `banner` and key-value
  items.

**View tree** (`view.hpp`).

- `struct TextSpan { std::string text; bool strong = false; bool mono = false; operator== }`, with complete
  Doxygen.
- `Text` and `Banner` gain `Prop<std::vector<TextSpan>> spans`. The mount applies it only when it is bound or not
  empty, and then does not apply `text`. A node that says nothing of spans records nothing new.

**Contract** (`backend.hpp`).

- `TextWidget::setSpans(std::vector<TextSpan> const&)` and `BannerWidget::setSpans(…)` are pure. Each replaces what
  the widget shows with the runs, in order. A backend draws `strong` in its bold weight and `mono` in its monospace
  face, and draws every run's text as its characters.
- They are pure, as batch A's `widthClass` was, so a backend cannot silently drop the emphasis. There are four
  implementations: `qt_quick`, `tui`, `RecordingBackend` and the test-only `EchoingBackend`.

**Per backend.**

- **Qt Quick.**
  - The C++ `TextImpl::setSpans` builds the label's text as `Text.RichText`:
    - the whole inside one `<span style="white-space:pre-wrap">`, since rich text otherwise folds a line feed and a
      run of spaces into one space, where a plain text keeps both;
    - each run's text through `QString::toHtmlEscaped()`;
    - `strong` as `<b>` (`Theme.strongFont` is `Font.Bold`, the weight `<b>` gives);
    - `mono` as `<span style="font-family:'…'">` naming batch B's `Theme.monoFamilies`, each family name escaped
      for a quoted CSS string.
  - `MorphText` gains `richText`. While it is not empty, the label shows it as `Text.RichText`; otherwise it shows
    `text` as `PlainText`. `setSpans` writes `text` as the runs' text joined and `richText` as the HTML; `setText`
    clears `richText`. A plain text never goes through HTML, and the accessible name (`defaultName: text`) and the
    probe's `textOf` stay the characters shown, never the HTML.
  - `MorphBanner` does the same through `BannerImpl`.
  - Verified with a throwaway `qml6` run (offscreen, Qt 6.12.0, deleted):
    - `StyledText` has no font-face tag: `<tt>`, `<code>` and `<font face='monospace'>` all drew at the proportional
      width (26.56 px for ten `i`), so the spans path needs `RichText`;
    - `RichText` with a `font-family` span drew the run in the mono face (71.88 px);
    - an escaped `&lt;img src=x&gt;&lt;b&gt;L-26&lt;/b&gt;` inside a mono span drew as one line of literal characters
      (177.9 px, against 152.8 px for the same characters proportional and 186.9 px all mono);
    - rich text folds whitespace, and `pre-wrap` keeps it:

      ```
      qml: plain nl h 34 rich nl h 17 rich br h 34 rich prewrap h 34
      qml: spaces plain w 29.984375 rich w 16.671875 prewrap w 29.984375
      qml: height plain 17 rich 17 width plain 42.1875 rich 42.1875
      ```
- **Terminal.**
  - `TextImpl` keeps a list of runs. `paint` draws each run in the role's style, with `bold` set for a `strong` run.
  - `mono` changes nothing, since every cell is monospace. The face is what the design asks for, and the terminal
    already has it.
  - The banner does the same.
  - Lines still break only at `\n`, as a text does today (`leaf_widgets.cpp:87-107`), and a run's `\n` breaks the
    line inside it.
- **`RecordingBackend`.** It records `spans` as a list. Each run is its escaped text, followed by `;strong` and
  `;mono` when set: `spans=[Lot ,L-26-0381;mono]`. Inside a span, `;` is escaped, as in a menu. A node with both
  records `spans` and no `text`.

**Spec.**

- `document.md:164`: `text` gains `spans`. `:184`: `banner` gains `spans`. A new paragraph under the table defines a
  span list, and says that a node may give `text` beside `spans` as an older client's fallback.
- `view_tree.md:104,124`: `Text` and `Banner` gain `spans`, and `TextSpan` is defined.
- `backend_contract.md`: the Interfaces table gains `setSpans` on `TextWidget` and `BannerWidget`, and the
  RecordingBackend's properties gain `spans` with its format.
- `qt_quick/backend.md:142`: Text draws spans as escaped rich text.
- `tui/frontend.md`'s Widgets table: a span is bold when `strong`, and `mono` draws as normal text.

**Tests.**

- `tests/test_ui_document.cpp`: a `text` with `spans` alone loads, and so does one with both `text` and `spans`. A
  literal span element `{"strong": true}`, with no `text`, is refused.
- `tests/test_ui_interpreter_control.cpp`, on `RecordingBackend`: the Main banner above records
  `spans=[Password incorrect.;strong, 3 of 5 attempts left before the account locks.]`. Setting `attempts` to 2
  re-records it. A `spans` whose second element is `7` records the run `7`. One whose element is `{"x": 1}` reports
  once and drops it. A `text` with `text: "fallback"` and spans `["Lot ", …]` records `spans=[Lot ,…]` and no
  `text` property. **Control:** lower `text` as well, and a `text=fallback` line is recorded.
- **Conformance case 46** (`backend_conformance.hpp`, all three backends): a Text given spans `["Lot ", {L-26-0381,
  mono}, {" <b>x</b>", strong}]` reads, through `textOf`, `Lot L-26-0381 <b>x</b>`. Each probe's `textOf` for a Text
  with spans is the runs' text joined, which on Qt Quick the C++ side keeps beside the HTML. **Control:** a backend
  whose `setSpans` does nothing reads the empty text and fails.
- `tests/qt_quick/test_qt_quick_widgets.cpp`:
  - a Text whose spans are the one plain run `<b>x</b> & <i>y</i>` has the `implicitWidth` of a `PlainText` label of
    the same characters, so the run's markup is drawn as characters. **Control:** drop `toHtmlEscaped`, and the
    width drops to that of the styled `x & y`.
  - a run `a\nb` draws two lines (`implicitHeight` twice a one-line text's), and a run `a     b` is as wide as the
    same `PlainText`. **Control:** drop the `pre-wrap` span, and both checks fail.
  - the Text's `Accessible.name` is the runs' text joined, with no `<`.
  - a `mono` run is wider than the same characters in the body font when the run is ten `i`s. The test first
    `REQUIRE`s an installed family whose `QFontInfo::fixedPitch()` is true, so a machine without a monospace font
    fails rather than passing on a proportional fallback. **Control:** build the span without the `font-family`
    style, and the widths are equal.
- `tests/tui/test_tui_display.cpp`: a strong run's cells are bold and its neighbours' are not.

**Gallery.**

- The e-signature banner and the "Signing at … as **Approved**" line use spans.
- Every id becomes `role: mono`, or a `mono` span inside its sentence.
- The summary key-value gives its values as spans (G24).

**Risks.**

- On Qt Quick a text with spans is a `QTextDocument`, so it costs more than a plain label. Spans are for a few
  sentences per screen, not table cells. A table column's `mono` role is a role, not a span, and stays plain text.
- `RichText` could lay out with its own margins. The probe above measured a one-line rich text at the plain text's
  height and width (17 px, 42.19 px), and the widget test keeps that as a check.
- A client older than this batch ignores `spans` (an unknown key, `document.hpp:1007`), and ui/1 has no
  must-understand key a newer client accepts: every `!` key is refused (`refuseMustUnderstand`,
  `document.hpp:1010-1017`). So a node gives `text` beside `spans` whenever an older client may read it, and that
  client draws the sentence without emphasis instead of an empty text. A node with `spans` alone draws empty there.

## G24, G39: label-over-value pairs, and a toned value

**Problem (read).**

- `keyValue` takes `items` only (`document.md:188`), each a `{label, value}` turned into two strings
  (`interpreter.hpp:6413-6431`; `KeyValueItem`, `view.hpp:1038-1047`).
- Both frontends draw each caption beside its value (`MorphKeyValue.qml`; `KeyValueImpl::paint`,
  `src/tui/display_widgets.cpp:316-324`).
- 03 compact draws a stacked list: a small muted caption above each value, the pairs divided by rules. 06 draws
  "180 needed, 150 in stock" red and bold while stock is short. Main and 03 set ids inside values in the mono face
  (G2).

The gallery's workarounds:

- `instrument.cpp:302` builds a column of a muted text over a text per pair, without dividers.
- `stability.cpp:341-361` builds rows of a caption and a `text` whose role is `error` while short. Its comment also
  cites the polish loop that batch A fixed (G34).

**Vocabulary.**

```json
{"kind": "keyValue", "fieldLayout": "stacked",
 "items": {"list": [
   {"obj": {"label": "Serial number", "spans": {"list": [{"obj": {"text": "DEAB-902114", "mono": true}}]}}},
   {"obj": {"label": "Containers", "value": {"ref": "containersLine"},
            "tone": {"if": [{"ref": "enough"}, null, "err"]}}}]}}
```

- `fieldLayout` is a literal closed choice: `inline` (the default, each caption beside its value) or `stacked` (each
  caption above its value). It reuses the key and the values a `form` already has (`document.md:195`), with the same
  meaning: where a caption sits relative to what it names. A key-value defaults to `inline`, a form to `stacked`, as
  each does today.
- An item may give `spans` beside or in place of `value` (G2), and a `tone` (`ok`, `warn`, `err`, `info`;
  `neutral` or `null` for none).
- An item may give both `value` and `spans`: a client that knows spans draws them, and an older one the `value`.

**Author API.** `KeyValueOptions{AnyHandle items; std::optional<FieldLayout> fieldLayout}` and
`ui::keyValue(KeyValueOptions)`. `ui::keyValue(items)` stays. The author's `FieldLayout` enum already exists for forms
and is reused.

**Lowering.** `checkKindValues` checks `fieldLayout` on `keyValue`, as it does on forms (`document.hpp:3032-3036`).
The `keyValue` branch reads each item's `spans` through `spansOf` and its `tone` through `toneNamed`, the strict
reading a table row's tone already has (`ScreenCore::rowToneOf`, `interpreter.hpp:7313-7338`), shared: `null` is no
tone, and a name that is none of the five is reported once for the node and drawn as none. `toneOf`, which a banner
and a badge use, reports nothing and accepts aliases, so it is not reused here.

**View tree.**

- `KeyValueItem` gains `std::vector<TextSpan> spans` (when not empty, shown instead of `value`) and
  `Tone tone = Tone::Neutral`, where `Neutral` is no tone, as on a `Badge`. `operator==` still compares every member.
- `KeyValue` gains `CaptionPlacement captions` (`Beside`, the default, or `Above`), set once. The name is not
  `layout`, which `Common` already uses for `LayoutHints`.

**Contract.** `KeyValueWidget::setCaptionPlacement(CaptionPlacement)` is called once, only for `Above`. A toned value
is drawn in the tone's colour and in the strong weight. A value with spans draws them as a text does.

**Per backend.**

- **Qt Quick.** `MorphKeyValue` gains `captionsAbove`.
  - Beside: the grid is unchanged (batch A's G34 fix).
  - Above: a `ColumnLayout` of pairs. Each pair is the caption in `Theme.bodyFont`, `scheme.muted`, over the value.
    A 1 px `scheme.border` rule follows every pair but the last, with `Theme.spaceOf(1)` above and below it.
  - Each pair stays one accessible node, named `"<caption>: <value>"` (`qt_quick/backend.md:108`); a value with
    spans names it by the runs' text joined.
  - A toned value takes `toneOf(tone)` and `Theme.strongFont`. A value with spans uses the same `RichText` builder as
    `TextImpl`.
- **Terminal.**
  - Above: each pair is two rows, the caption muted and the value below it, with no rule. A rule would cost a row per
    pair on a 24-row screen, and the muted caption already separates the pairs.
  - A toned value draws in `toneStyle(tone)` with `bold`. Spans draw as on a text.
  - `probeText` is unchanged (`caption: value` per line), with a value with spans as the runs' text joined.
- **`RecordingBackend`.**
  - `captions=Above`.
  - The `items` format is extended: a value with spans is written as its span list in brackets, and a tone other than
    `Neutral` follows as `;tone=<Name>`. For example `[Serial number:[DEAB-902114;mono],Containers:180 needed;tone=Err]`.
    Text inside `items` also escapes `;`, as text inside a menu does, so a value cannot forge a `;tone=`.
  - An item with neither is written as today, except for that escape.

**Spec.**

- `document.md:188`: `keyValue` gains `fieldLayout`, and items gain `spans` and `tone`.
- `view_tree.md:128`: `KeyValue` and `KeyValueItem`.
- `backend_contract.md`: the `KeyValueWidget` row, and the `items` and `captions` formats.
- `qt_quick/backend.md`: the stacked layout and its rules.
- `tui/frontend.md`: the stacked layout and the bold toned value.

**Tests.**

- `test_ui_document.cpp`: `fieldLayout: "grid"` on a `keyValue` is refused.
- `test_ui_interpreter_control.cpp`: the vocabulary example records `captions=Above` and
  `items=[Serial number:[DEAB-902114;mono],Containers:…;tone=Err]`. Setting `enough` to `true` drops the `;tone=Err`.
- `test_qt_quick_widgets.cpp`:
  - at `Above`, the second caption's `y` is greater than the first value's `y + height`;
  - there is one rule between two pairs and none after the last;
  - a toned value's colour is `Theme.error`.
  - **Control:** ignore `captionsAbove`, and the caption sits level with its value.
- `test_tui_display.cpp`: a stacked pair takes two rows, and a toned value's cells are bold in the error colour.

**Gallery.** `instrument.cpp`'s compact details become a `stacked` key-value, its serial number a `mono` span.
`stability.cpp`'s summary rows become a key-value with a toned containers value, and the comment about the polish loop
goes. The e-signature summary keeps `inline`, its ids as spans.

**Risks.** The `items` log format changes for items that use the new members, and for a caption or value holding
`;`. The commit greps the golden logs for a `;` inside `items=` and updates any it finds.

## G8: the required mark follows the caption

**Problem (read; the gap log's grab stands).**

- On Qt Quick, `MorphField`'s marker is anchored to the field's right edge at `y: root.captionHeight`
  (`src/qt_quick/qml/MorphField.qml:54-61`), and `controlWidth` subtracts its width (`:22`). So the `*` sits in a
  gutter right of the control, a line below the caption.
- On the terminal, `paintMarks` draws `*`, `!` and `~` at the right edge of the control's first row
  (`src/tui/widget.hpp:345-411`; called on `control` after `paintFieldCaption`, `field_widgets.cpp:360,424`,
  `list_widgets.cpp:379`).
- Every mockup draws "Meaning \*" as the caption's last character, in the error colour.

**Rule.** The required mark follows the caption, wherever the field draws one:

- An input that draws its caption (`showLabel`) draws `*` right after the caption's text, on the caption's line, in
  the error colour, separated by a space. The control takes the field's whole width.
- An input that draws no caption keeps the mark beside its control, as today. It has nothing else to follow.
- A `text` with `required` draws `*` after the end of its last line. That is how G17's legend gets its mark.
- The other marks (`!` errors, `~` stale) stay where they are on the terminal. On Qt Quick they are not marks: errors
  are messages, and stale is opacity.

No document vocabulary changes. `required` already exists on every input and on text (`document.md:159`).

**Per backend.**

- **Qt Quick.**
  - `MorphField`'s marker binds `x` and `y` to the caption's while `captionHeight > 0`:
    `x: captionLabel.contentWidth + Theme.spaceOf(1)`, `y: 0`. Otherwise it keeps today's anchors.
  - `controlWidth` subtracts the marker's width only while the marker sits beside the control, and `implicitWidth`
    counts the marker beside the caption (`caption + space + marker`) instead of beside the control.
  - For `MorphText`, the marker sits at the end of the label's last line. A `Text` has no `positionToRectangle`
    (that is `TextEdit`'s; a throwaway `qml6` run printed `has positionToRectangle undefined`), so the label records
    its last line's `x + implicitWidth` and `y` from `onLineLaidOut`, and the marker binds to them. The label wraps
    within `width − space − marker width`, so the mark always fits after the last line.
  - The caption label's `elide` keeps the mark visible. The caption elides within `width − space − marker width`.
- **Terminal.** `TuiField` gains `captionMarks()` and `controlMarks()`.
  - While there is a caption, `paintFieldCaption` (a free function, `widget.hpp:312`) takes the caption's marks and
    draws the caption followed by ` *` in the error style when required, and the control row's marks lose the `*`.
    `withFieldCaption` counts the two cells in the caption's width.
  - A `text` draws ` *` after its last line's end. Its `naturalSize` counts the two cells while it is required, so
    in a row, where it is given its natural width, the mark does not overwrite its last characters, as today's
    right-edge mark does. Given less than that, it draws the mark at the right edge.
- **`RecordingBackend`.** No change: it records `required=true`. Where the mark sits is a frontend's drawing.

**Spec.**

- `view_tree.md:158`, the `required` row: "a mark that the field must be given, after its caption when the field
  draws one, else beside its control".
- `backend_contract.md` gains a **Required mark** bullet with the same sentence.
- `tui/frontend.md:195` changes "marks at the right edge of its first row" for the `*` of a captioned field.

**Tests.**

- `test_qt_quick_widgets.cpp`: a captioned required text input. The marker's `y` equals the caption's, its `x` is
  within one unit after the caption's `contentWidth`, and the control is as wide as the field. An uncaptioned one
  keeps the marker at the right edge. **Control:** restore the anchors, and the marker's `y` is `captionHeight`.
- `test_tui_fields.cpp`: a captioned required input 30 columns wide draws `Meaning *` on row 0, and no `*` on row 1.
  **Control:** paint the marks on the control again, and row 1 ends in `*`.
- `test_qt_quick_widgets.cpp`: a required two-line `text`. The marker's `y` is the second line's, and its `x` is
  after the second line's end. **Control:** bind the marker to the label's `contentWidth` instead, and its `x` is
  that of the first, longer line's end.
- `test_tui_display.cpp`: a required `text` "Condition on receipt", in a row at its natural width, draws
  `Condition on receipt *`. **Control:** leave `naturalSize` without the mark's cells, and the last character is
  overwritten.

**Gallery.** Nothing to remove. The e-signature, receipt and results captions now draw as the designs do. The
receipt's "Condition on receipt \*" becomes part of G17's group.

**Risks.**

- A captioned field's control gains the marker's width back, so it is a few pixels wider on Qt Quick.
- **The forms engine is unchanged.** Its cell draws the caption and its control the mark (`form_view.hpp:440-475`), so
  a form's required mark stays beside the control. Decision 2 covers it.

## G10: help text under an input, and a value's evaluation

**Problem (read).**

- An input's own keys are its `label` and `showLabel` (and a text input's `placeholder`), and the field decorations
  are `readonly`, `required`, `stale` and `errors` (`document.md:159-166`, `kFieldKeys` at `document.hpp:688-693`).
  There is no help or description key, and `a11y` has `name` and `role` only.
- The gallery puts a muted `text` under the input, in a column with it (`receipt.cpp`'s `sampleCard` and
  `expandedLayout`, at `b080a5265`). Assistive technology does not associate it with the field.
- 08 needs a result field "outlined red while it is out of specification", with "Out of spec: above 0.20" under it.
  An out-of-specification result is a valid entry that must be saveable.
- `errors` cannot carry it. An element's errors make it invalid, and a dialog with an invalid element is not
  `ready` and refuses a `run` (`docs/spec/ui/interpreter.md:225-233`; `valid` at `interpreter.hpp:4511-4515`).

**Correction to the gap log.** It says the 08 field "is outlined only while its text cannot be read, through
`errors`". No Qt Quick input is outlined for its errors: `MorphInputBackground` takes `scheme`, `focused` and
`readOnly` only, and no user passes errors to it (`MorphInputBackground.qml:8-11`; the five users in
`MorphTextInput.qml:64`, `MorphTextArea.qml:38`, `MorphSelect.qml:52`, `MorphDateTimeInput.qml:98`,
`MorphFilePicker.qml:40`). Errors show only as red messages under the control. Read, not grabbed. The Main mockup's
password field is outlined red with a ring while refused, so this item draws that too.

**Forms engine precedent.** The forms engine already has help: `formView` draws `state.help()` as a muted `Text`
under the control (`include/morph/forms/engine/form_view.hpp:428-435,471-473`). That text is likewise not associated
with the control.

**Decision: a field `description` and a field `tone`, drawn by the input.**

- **`description`** is a line that describes the field. It is drawn muted under the control, after any errors, and
  announced as the control's accessible description, after the errors (`aria-describedby`). The name is the
  accessibility concept, and the same key describes a dialog (G6) and a group (G17).
- **`tone`** is an evaluation of the value that does not make it invalid: `ok`, `warn`, `err` or `info`; `neutral`
  or `null` for none. A toned field is outlined in the tone's colour, and its description is drawn in the tone's
  colour instead of muted.
- An input with errors is outlined as the `err` tone, whatever its `tone`. So errors and an evaluation share one
  outline, and only errors block. The description keeps its own tone's colour (muted when untoned): the messages
  above it are already in the error colour.
- The input draws both itself, rather than the document placing a text, because the association is the point. A
  sibling node cannot be tied to the control for assistive technology, and it cannot draw the control's outline.

**Vocabulary.**

```json
{"kind": "textInput", "label": "Impurity B", "showLabel": true, "unit": "%",
 "value": {"ref": "impurityB"}, "description": {"ref": "impurityBEvaluation"},
 "tone": {"if": [{"ref": "impurityBOutOfSpec"}, "err", null]}}
```

- `description` (an expression; text) and `tone` (an expression; a tone name) on `textInput`, `checkbox`, `select`,
  `dateTimeInput`, `slider` and `filePicker`.
- They are not on `text`. The loader refuses them there ("a text is not described; put the line in the text"), since
  `text` shares the field keys (`kField`, `document.hpp:935`).
- A tone name that is none of the five is reported once for its path and drawn as none, through the same strict
  reading as a key-value item's tone (G24).
- Author: `Node::description(AnyHandle)` and `Node::tone(AnyHandle)`, beside `readonly`, `required` and `errors`
  (`author.hpp:1969-1979`), each with complete Doxygen.

**Lowering.** `fieldOf` (`interpreter.hpp:5870-5882`) binds `field.description` (`stringOf`) and `field.tone` (a
`Prop<Tone>` through `toneNamed`, `Neutral` for `null`, `neutral` and an unknown name) for the six inputs.

**View tree.**

- `FieldState` gains `Prop<std::string> description` and `Prop<Tone> tone = Tone::Neutral`. `Neutral` is no
  evaluation, as it is a badge's plain look; a field has no neutral outline distinct from none, so an
  `std::optional` would add a state no backend draws.
- The mount applies each only when it is bound or not empty/`Neutral`, as it applies `errors` (`view_tree.md`, Per
  node). A new field has neither.
- The mount refuses (`std::logic_error`) a `Text` with either, so a hand-built tree cannot reach a setter the loader
  keeps documents from.

**Contract.**

- `FieldWidget::setDescription(std::string_view)` and `FieldWidget::setTone(Tone)` are pure.
  - `setDescription` shows the line under the control, after the messages, and makes it the accessible description,
    after the messages.
  - `setTone` outlines the control in the tone and draws the description in the tone's colour.
  - A field with errors is drawn as `err`.
- `TextWidget` derives from `FieldWidget` (`backend.hpp:137`), so `TextImpl` implements both on every backend as
  unreachable: it reports a contract breach and does nothing.

**Per backend.**

- **Qt Quick.**
  - `MorphField` gains `description` and `tone` (an `int`, the `Tone` value; 0, `Neutral`, for none).
  - A `descriptionLabel` under `messages` is muted, or `toneOf(tone)` while toned, wraps, is `PlainText`, and is
    visible while the description is not empty. `implicitHeight` counts it.
  - `accessibleDescription` becomes the messages and the description joined by `\n`.
  - `MorphInputBackground` gains `tone`: 0, or 4 (`Tone::Err`) while the field has errors. While toned, the border
    is `toneOf(tone)`, focused or not, so a field does not stop looking refused when the user tabs into it; focus
    shows as the border's 2 px width. A 2 px ring outside the border is `toneBgOf(tone)`, which is the mockup's
    `box-shadow: 0 0 0 2px`; it is a sibling `Rectangle` with negative margins, since a `Rectangle` draws nothing
    outside itself.
  - Every user of the background passes `tone: root.errors.length > 0 ? 4 : root.tone`.
  - A check box, which has no input background, draws its indicator's border in the tone. A radio select draws each
    indicator's border in it, and a segmented one (G53) its track's border.
  - A slider draws its handle's border in the tone.
- **Terminal.**
  - The field grows one row for a description, drawn under the control in the muted style, or in `toneStyle(tone)`
    while toned. Like a text, it breaks only at `\n`, and a longer line is clipped at the field's width; the whole
    line is in the hover text.
  - The terminal has no outline, so a toned field's control row adds the `!` mark for `err` and `warn`, which already
    means "look at this field". `ok` and `info` add nothing.
  - `TuiField` gains `accessibleDescription()`, the errors and then the description joined by `\n`, and
    `hoverText()` is the tooltip followed by it. The probe reads `accessibleDescription()`, so a tooltip never has to
    be cut off the hover text.
- **`RecordingBackend`.** It records `description` (escaped text) and `tone` (`Ok`, `Warn`, `Err`, `Info`, or
  `Neutral` after a tone is cleared).

**Forms engine.**

- `formView` passes a field's help as the control's `description` (`fieldState`, `form_view.hpp:141-150`), instead
  of adding a muted `Text` in `fieldCell` and `inlineCell`, for the built-in controls. The help is then associated
  with the control on every kit.
- A control an `Overrides::Render` draws (`form_view.hpp:452-455`) is whatever node the override returns, and need
  not read `fieldState`; an Object's panel and an ObjectArray's rows are not one control. For those the cell keeps
  the muted `Text`, so no help disappears.
- Under the inline layout the control's column already starts at the caption's width, so the help still lines up
  under the control.
- The cell's `captionSpace` spacer for help goes.

**Spec.**

- `document.md:159-166`: the six inputs take `description` and `tone`, and text refuses them.
- `view_tree.md`'s `FieldState` table gains both rows, and the Per node paragraph says when they are applied.
- `backend_contract.md`: the `FieldWidget` row, a **Description and tone** bullet with the rules above, and the
  RecordingBackend's properties.
- `interpreter.md`'s Errors bullet: "a field's `tone` is not an error: it changes the field's look and nothing of
  `valid` or `ready`".
- `qt_quick/backend.md` and `tui/frontend.md`'s Field state.
- `docs/spec/forms/views.md`: help is the control's description.

**Tests.**

- **Conformance case 47.** A text input whose description and errors are bound. `descriptionOf(field)` reads
  `Required range 2–8 °C` alone, then `Outside 2–8 °C\nRequired range 2–8 °C` once an error is set, then the
  description alone once it is cleared.
  - The probe's new `descriptionOf(widget)` is what assistive technology reads as the widget's description. Qt Quick
    reads the control's `Accessible.description`, the terminal the field's `accessibleDescription()`, and
    `RecordingBackend` its `errors` and `description` joined.
  - The default answers none, and the case fails on a probe that cannot read one, as the layout cases do with
    `boundsOf`.
  - **Control:** a backend whose `setDescription` does nothing fails on the first read.
- `test_ui_interpreter_control.cpp`:
  - a dialog holding a required text input with `tone: err` and a value is `ready`, and a `run` from it is not
    refused;
  - the same input given `errors: ["x"]` makes the dialog not ready.
  - **Control:** route `tone` into `errors` in `fieldOf`, and the first case is refused.
- `test_ui_document.cpp`: `description` on a `text` is refused.
- `test_qt_quick_theme.cpp`, under `brandTheme()`:
  - a text input with `tone: err` has a background border of the brand `color.err` and a ring of `color.err.bg`,
    and keeps the border's colour while focused;
  - one with errors has the same;
  - an untoned one has `color.border`.
  - **Control:** drop `tone` from `MorphInputBackground`, and the first two read `color.border`.
- `test_tui_fields.cpp`: a toned `err` field's description row is in the error style, and its control row ends in
  `!`.
- `tests/test_forms_*` (the form-view tests that assert the help `Text`): the help is now the control's
  `description`, and there is no separate muted `Text`. A field drawn by an override keeps its muted help `Text`.
  **Control:** pass the help as `description` for an override too, and its help is gone.

**Gallery.**

- Receipt: "Required range 2–8 °C" and the scan field's "The field keeps focus…" become `description`s, and their
  wrapping columns go.
- Results: the out-of-specification evaluation becomes the field's `tone` and `description`. The tag beside the
  field and the row tint stay as the expanded table draws them. The compact field drops its tag.
- E-signature: the refused password's outline now draws, through `errors`, with no gallery change.

**Risks.**

- A form's help moves from a `Text` node into the control. A kit that restyles the input and wants the help keeps it
  by reading `description`. A test that found the help `Text` by test id changes.
- Inputs with errors gain a red outline on Qt Quick. That is a visible change to every screen that shows errors, and
  the intended one.

## G6: a dialog's description line (#1062)

**Problem (read).** `dialog` and `drawer` have `title` and no description (`document.md:177`; `Dialog`,
`view_tree.md:117`). A screen's head has `subtitle` (`document.md:63`). The gallery puts a muted `text` first in the
dialog's content (`esignature.cpp`'s `expandedLayout`, `receipt.cpp`'s `rejectDialog`, at `b080a5265`). That text
scrolls with the body, sits under no divider, and is not the dialog's accessible description.

**Decision: `description`, the same key and meaning as a field's.** A dialog's description is one muted line in its
head, under the title, divided from the body by a rule. It is the dialog's accessible description, as
`aria-describedby` makes it for an HTML dialog. It is not `subtitle`, because a screen's subtitle sits beside the
title as a breadcrumb or context line and describes nothing.

**Vocabulary.** `dialog` and `drawer` take `description` (an expression; text), evaluated in the enclosing scope,
like `title`. Author: `DialogOptions::description`.

**Lowering.** `lowerDialog` (`interpreter.hpp:6997-7064`) binds `description` like `title`.

**View tree and contract.**

- `Dialog` and `Drawer` gain `Prop<std::string> description`, applied only when bound or not empty.
- `DialogWidget::setDescription(std::string_view)` and `DrawerWidget::setDescription(…)` are pure. Each shows the line
  under the title, in the head, which does not scroll with the content, and makes it the overlay's accessible
  description.

**Per backend.**

- **Qt Quick.** `MorphDialog` and `MorphDrawer` gain `description`.
  - Under `titleLabel`, a `Label` in `Theme.bodyFont`, `Theme.cardScheme.muted`, wrapping, is visible while not
    empty.
  - A 1 px `Theme.cardScheme.border` rule spans the popup's content width under the head while the description shows.
    A dialog without a description keeps today's look.
  - Qt refuses the `Accessible` attached property on a `Popup`, which is not an `Item`. A throwaway `qml6` run
    printed `QML Popup: Accessible attached property must be attached to an object deriving from Item or Action`.
    So the popup's content column carries the overlay's accessible node: `Accessible.role: Accessible.Dialog`,
    `Accessible.name` the title (or the node's `a11y.name`), and `Accessible.description` the description. Today the
    popup presents no name at all (`MorphDialog.qml` sets no `Accessible`), and a described dialog with no name
    is no better, so the title becomes the dialog's name in the same change.
  - The head sits outside the `ScrollView`. The scroll area's height cap (`MorphDialog.qml:120-121`) subtracts the
    description's and the rule's heights, and their spacing, as it subtracts the title's.
- **Terminal.** `OverlayCore` gains a description. It draws the description muted on the first row inside the frame,
  under the title in the border, then a `─` rule row, and lays the content out below. The content area loses two
  rows while a description is set. The description is one row, clipped at the frame's width like any terminal text.
- **`RecordingBackend`.** It records `description`.

**Spec.**

- `document.md:177`: `dialog` and `drawer` gain `description`.
- `view_tree.md:117` and the Drawer row.
- `backend_contract.md`'s `DialogWidget` and `DrawerWidget` rows, and RecordingBackend properties.
- `qt_quick/backend.md:156`: the head, the rule, and the accessible description.
- `tui/frontend.md`'s "Focus, dialogs and drawers".

**Tests.**

- **Conformance case 48.** An open dialog with a bound description: `descriptionOf(dialog)` reads it and follows the
  binding. Qt Quick's probe reads the content column's `Accessible.description`. **Control:** a backend whose
  `setDescription` does nothing fails.
- `test_qt_quick_overlays.cpp`:
  - an open dialog's description label lies under the title and above the scroll area;
  - the rule is visible;
  - with a content taller than the window, the description stays at the same `y` after scrolling the content to its
    end, and the footer is still inside the window;
  - the content column's `Accessible.name` is the title.
  - **Control:** put the label inside the scroll's column, and the `y` check fails; leave the cap without the
    description's height, and the footer is pushed past the window.
- `tests/tui/test_tui_dialog.cpp`: the description's row and the rule row are drawn, and the first child starts two
  rows lower.

**Gallery.** The e-signature dialog and the reject dialog move their first muted text into `description`.

**Risks.** None outside overlays. A dialog that does not set a description is drawn and laid out as today.

## G17: a group of check boxes with a legend and a required mark

**Problem (read).**

- ui/1 has no group node. `required` on a `checkbox` marks that box alone, and an element's `Required` error fires
  when its value is not engaged (`interpreter.md:225-228`).
- The receipt draws "Condition on receipt \*" as a plain `text` above the boxes, `*` included as a character
  (`receipt.cpp`'s `conditions`, at `b080a5265`). It enables the receive button only while the three mandatory boxes
  are checked.

**Decision: a `group` node that the interpreter lowers to existing kinds.** It needs no new widget and no backend
change.

- A group is a labelled set of inputs: the `fieldset` and `legend` of HTML, `role="group"` in ARIA.
- Its legend is a `text` carrying `required`, so G8 draws the mark after it.
- Its description and errors are lines under its children.
- It is an element, so a **check** can target it. "The three mandatory boxes are checked" is a check whose message
  shows on the group and keeps a dialog from `ready`, through rules that already exist (`interpreter.md:225-234`).
- `required` on a group is the mark only. A group has no value, so no `Required` error is derived from it. Which
  combinations of its children are required is the application's rule, as `view_tree.md:158` already says of
  `required`. A group derives no "at least one" rule (decision 3).

**Vocabulary.**

```json
{"kind": "group", "id": "conditions", "label": "Condition on receipt", "required": true, "gap": 1,
 "description": "Container, seal and label must be checked to receive the sample.",
 "children": [{"kind": "grid", "columns": 2, "gap": 1, "children": ["…four checkboxes…"]}]}
```

with, in the screen's `checks`:

```json
{"id": "mandatoryConditions", "assert": {"fn": "and", "args": [{"ref": "intact"}, {"ref": "sealed"}, {"ref": "labelled"}]},
 "message": "Container, seal and label must be checked.", "targets": ["conditions"]}
```

- Keys: `label` (an expression, the legend), `required` (a condition), `description` (an expression), `errors` (an
  expression, a list of strings, as on an input), `gap` (a spacing step) and `children`.
- `kKinds` gains `{"group", kGroupKeys}`. The group is not a scope. Its element values are the common ones
  (`visible`, `enabled`, `valid`, `errors`, `shownErrors`).
- **When its errors show.** `shownErrors` follows the policy as for any element (`interpreter.md:230-231`), and a
  group counts as touched once any element inside it is touched. Without that, a group has no value to touch, and
  under the default `touched` policy a check's message on it would show only after a submit, however long the user
  had been ticking its boxes.
- The author's accessibility rule (`docs/spec/ui/author.md:238-241`) gains a row: a `group` needs a `label` or an
  `a11y.name`.
- Author: `ui::group(GroupOptions{label, required, description, errors, gap}, Nodes children)`.

**Lowering.** `group` lowers to a `Column` (gap `gap`):

1. a `Text` of `label`, with `field.required` bound to `required`;
2. the children;
3. a muted `Text` of `description`, shown while it is not empty;
4. a `Text` in the `Error` role of each of the group's `shownErrors`, through a `forEach` keyed by position.

The column's `Common` carries the node's own common properties, with `a11y.role` `group` and `a11y.name` the label
unless the node's `a11y` names it. `group` maps to `QAccessible::Grouping` on Qt Quick already
(`src/qt_quick/widget.cpp:191`). It is lowered by the interpreter, as `boundary` and `customize` are
(`view_tree.md`, The palette), so the view tree and the contract do not change.

**Spec.**

- `document.md`: a `group` row in the kinds table, and a sentence under it: `required` marks and derives no error,
  and a check targeting the group shows its message there.
- `interpreter.md`'s Lowering section: what a group lowers to.
- `author.md`: the accessibility row and `ui::group`.

**Tests.**

- `test_ui_document.cpp`: a group whose `gap` is `{"bogus": 1}` is refused, as on a column. `checkAccessibility`
  reports a group with neither `label` nor `a11y.name`, and passes one with either.
- `test_ui_interpreter_relations.cpp`, with the vocabulary example in a dialog under the default `touched` policy:
  - the dialog is not `ready` while `labelled` is false, the group's `errors` holds the check's message, and its
    `shownErrors` is empty;
  - after the user toggles `intact` (a `toggle` on its box), `shownErrors` holds the message and the error `Text` is
    shown;
  - after `set labelled true` the dialog is ready, and the error text is gone.
  - **Control:** drop the group's element registration (its `id` not declared as an element), and loading the check
    is refused because its target names no element. Drop the touched rule, and the second step's `shownErrors` is
    empty.
- `test_ui_interpreter_control.cpp`, on `RecordingBackend`: the group's column has `a11yRole=group`,
  `a11yName=Condition on receipt`, and a first child `Text` with `required=true`.

**Gallery.** The receipt's `conditions` becomes a `group` with the check. The receive button's enablement may keep
its own condition, or read the group's `valid`. The `*` character in the legend's text goes.

**Risks.**

- The legend draws as an ordinary text in the normal role, where the mockup's `.lbl` is 12 px at weight 500, like a
  field caption. A caption role is not in ui/1. The lowering uses `Normal`; `Section` would draw it bold and muted,
  which reads as a list's heading rather than a caption.
- The group's description is read in order, as a line a screen reader reaches by reading on, not as the group's
  accessible description: `Common` carries an accessible name and role only, and a lowered group has no setter of its
  own. That is the intended behaviour of ui/1's group.

## G9: a unit beside a free input (#1060, #1062)

**Problem (read; the gap log stands).**

- The view tree's `TextInput` has `unit` (`view_tree.md:106`). The mount applies it (`view_tree.md`, Per node,
  step 3). Qt Quick draws it as a muted trailing label inside the field (`MorphTextInput.qml:96-105`). The terminal
  draws it before the marks (`field_widgets.cpp:352-359`). `RecordingBackend` records it
  (`recording_backend.hpp:942`).
- Only the forms engine sets it. The document's `textInput` keys have no `unit` (`document.hpp:725-734`), and neither
  does `author::TextInputOptions` (`author.hpp:3187-3207`).
- The gallery puts the unit in the caption ("Temperature on arrival, °C").

**Vocabulary.** `textInput` takes `unit` (an expression; text, bound). Author: `TextInputOptions::unit`.

**Lowering and validation.**

- The `textInput` branch binds `spec.unit` with `textProp("unit")`.
- The mount refuses a multiline input with a unit, places or an icon (`std::logic_error`, `mount.hpp:749-753`).
  So:
  - the loader refuses `unit` beside a literal `mode: multiline` ("a multiline input shows no unit");
  - a computed `mode` that evaluates to `multiline` beside a `unit` is reported once at `/unit`, and the unit is not
    bound, so the mount never throws.
- `icon` has the same rule in the spec (`document.md:166`: "refused on a multiline input") and no check in the
  loader, so today a literal multiline input with an icon loads and its mount throws out of `Interpreter::mount`.
  A throwaway Catch2 case against this revision printed:

  ```
  C1 loaded=1
  C1 mount threw: St11logic_error: morph::ui: a multiline text input holds no number, so it takes no unit, places or icon
  ```

  The check written for `unit` takes `icon` too, with the same refusal and the same report for a computed mode.
- **The unit is announced.** On Qt Quick the field's accessible description ends with the unit, after the errors
  and the description (G10), so a listener hears "4" with its "°C". The name stays the label, so a driver that finds
  the field by name is unchanged. The terminal's `accessibleDescription()` does the same. The forms engine's units
  are announced by the same change.
- `decimals` stays out of the document. It is a number's entry places for a kit to read, which a document's
  `parseDecimal(text, dp)` already decides (`expressions.md`, Entry).

**Spec.**

- `document.md:166`: `textInput` gains `unit` (refused with a literal multiline `mode`).
- `backend_contract.md`'s RecordingBackend property list gains `unit`, `decimals`, `icon` and the caption properties,
  which the recorder already writes but the spec does not list (`backend_contract.md:297`).

**Tests.**

- `test_ui_document.cpp`: `unit` with `"mode": "multiline"` is refused at `/view/unit`.
- `test_ui_document.cpp`: `icon` with `"mode": "multiline"` is refused at `/view/icon`.
- `test_ui_interpreter_control.cpp`: `unit: "°C"` records `unit=°C`. A bound unit follows its state. A computed
  multiline mode with a unit reports once and records no `unit`, and the mount does not throw. **Control:** drop the
  binding, and no `unit` is recorded; drop the report path, and the mount throws.
- `test_qt_quick_widgets.cpp`: a text input with `unit: "°C"` has an `Accessible.description` ending in `°C`.

**Gallery.** The receipt's temperature and the results' compact fields give `unit` (`°C`, `%`, `% LC`), and the units
leave their captions.

**Risks.** The widget path is the forms engine's, already tested on both frontends. A document that gives a
multiline input an icon is now refused at load where its mount threw.

## G14, G33, G53: an outlined destructive button, an outlined tag, a segmented control (#1061)

Three looks that each grow a closed choice the node already has.

**Problems.**

- **G14 (verified by trying, gap log).** `emphasis` is `normal`, `primary`, `danger` or `ghost`
  (`document.md:165`). `danger` is a filled red button on Qt Quick (`backend_contract.md:155-161`), as loud as the
  primary. The mockups' `.btn.danger` is outlined, with red text (`02-Receipt.dc.html`, `09-Review.dc.html`:
  `color:#A61B1B;border-color:#E8B4B4`), so "Receive and scan next" stays the one filled action.
- **G33 (read).** `badge` has `tone`, `text` and `icon` (`document.md:185`). The mockups' priority tags are outlined
  on white (`05-Samples.dc.html`, `.pri` with `.p-stat`, `.p-urg`, `.p-rou`), so a red outline means STAT and a red
  fill means out of specification.
- **G53 (read).** A `select`'s style is `dropdown` or `radio` (`interpreter.hpp:6156`). `radio` draws a column of
  radio buttons. 09's "Show: All 24 | Flagged 2" is a two-segment switch (`.seg`, `role="radiogroup"`).

**Vocabulary.**

| Node | Key | New value | Meaning |
|---|---|---|---|
| `button` (and an `actionButton` part) | `emphasis` | `dangerOutline` | A destructive action that is not the main one: outlined, its label in the error colour |
| `badge` | `style` (new, a literal closed choice) | `filled` (default), `outline` | `outline`: the surface's fill, the tone's colour as border and text |
| `select` | `style` | `segmented` | Every option visible as one row of joined segments, one marked |

- `style` is the key ui/1 already uses for "how this kind is drawn" (a button's `push`/`item`, a select's
  `dropdown`/`radio`), so a badge takes it too.
- `dangerOutline` is an `emphasis`, not a `tone` on buttons. A tone axis would multiply four emphases by five tones
  across two frontends for one look the designs use. The order of the two words reads as the emphasis it is: danger,
  drawn outlined.
- `checkKindValues` (`document.hpp:3002-3041`) checks the new values. A select's `style` is an expression today
  (`kSelectKeys`), so a literal one is checked against the three names, and a computed one that names none is
  reported once and drawn as a dropdown. Today neither is checked: the throwaway Catch2 case of G9 loaded
  `"style": "radios"` and printed `C2 loaded=1` and `C2 style=Dropdown diags=`, with no diagnostic.
- Author:
  - `ButtonEmphasis::DangerOutline`, written as `dangerOutline` by `ui::button` and `actionButton`;
  - `ui::badge(text, tone, icon, BadgeStyle)`, or a `BadgeOptions` overload;
  - `select`'s `style` stays an `AnyHandle`.

**View tree and contract.**

- `ButtonEmphasis` (`include/morph/ui/button_emphasis.hpp`) gains `DangerOutline`, after `Ghost`. `setEmphasis` is
  called for it, as for every value but `Normal`.
- `BadgeStyle{Filled, Outline}` and `Badge::style`, set once. `BadgeWidget::setStyle(BadgeStyle)` is pure and called
  only for `Outline`.
- `SelectStyle` gains `Segmented`, a factory parameter like the others.
- Keyboard, for the contract's **Chords** step 2:
  - a segmented select takes Left and Right, which mark and choose the previous or next option (`onSelect` once), and
    stop at the ends;
  - Space and Enter on it do nothing, so Enter goes on to a dialog's `default`.
  - This is the radio-group behaviour ARIA gives a `radiogroup`. The select is one Tab stop, entered at the marked
    option; its segments take no focus of their own.

**Per backend.**

- **Qt Quick.**
  - `MorphButtonBackground` draws `dangerOutline` as `normal` (surface fill, a 1 px border) with the border in
    `toneBorderOf(Err)`. `MorphButton` draws the label in `scheme.error`.
  - `MorphBadge` gains `outline`. Its fill is `scheme.surface`, and its border and text are `toneOf(tone)`.
  - `MorphSelect`'s segmented look is a `Row` of `Button`s in an exclusive `ButtonGroup` on a `scheme.neutral` track
    with `Theme.radiusControl` and a 2 px inset. The marked segment is drawn on `scheme.surface` with the card's
    shadow, and the others are clear. Each segment is `Accessible.RadioButton` with `focusPolicy: Qt.NoFocus`, and
    the select is `Accessible.Grouping`, as the radio style already is (`MorphSelect.qml:40`); Qt has no
    radio-group role.
  - Its height is `Theme.targetHeight − 8` (24 px of the 32), so it reads as a control inside a toolbar line. While
    `Theme.touchTargets` holds it is `Theme.targetHeight`, since a compact window's controls keep the touch target.
  - A `stretch` segmented select shares its width equally among the segments (09 compact).
- **Terminal.**
  - `dangerOutline` draws in the error colour and **not** bold. That keeps it apart from `danger` (bold red) and
    from `normal`.
  - An outline badge draws `(STAT)` in the tone's style, where a filled one draws `[STAT]`
    (`display_widgets.cpp:201-205`).
  - A segmented select draws one row: each option's label padded by a space, joined by `│`, with the marked one in
    the list's selected-item style. Left and Right choose, and a click on a segment chooses it.
- **`RecordingBackend`.**
  - It records `emphasis=DangerOutline`, `style=Outline` on a badge, and `style=Segmented` on a select.
  - `press` gains a segmented select's Left and Right in step 2: each chooses the neighbour, if any, then calls
    `onSelect`.
  - `choose` works on every style as before.

**Spec.**

- `document.md:165,185,168`.
- `view_tree.md:105,108,125`.
- `backend_contract.md`:
  - **Button emphasis** gains `DangerOutline` on both frontends;
  - a **Badge style** paragraph;
  - **Chords** step 2 gains the segmented select's keys;
  - the RecordingBackend `press` list.
- `qt_quick/backend.md:146` (Select) and the Badge row.
- `tui/frontend.md`'s Widgets table.

**Tests.**

- **Conformance case 49.** A segmented select with three options, `B` requested: `textOf` reads `B`'s label.
  `press(select, "Right")` marks `C` and reports `C` once. A second `Right` reports nothing. `press(select, "Enter")`
  inside a column declaring an Enter chord runs the chord. **Control:** drop the Right handling in a backend, and
  the first press reports nothing.
- `test_ui_document.cpp`: `emphasis: "outline"`, a badge's `style: "ghost"` and a select's `style: "radios"` are
  refused, and the new values load. `test_ui_interpreter_control.cpp`: a computed select style naming none reports
  once and records `style=Dropdown`.
- `test_qt_quick_widgets.cpp`, under `brandTheme()`:
  - a `dangerOutline` button's background is `color.surface`, and its label colour `color.err`;
  - an outline badge's fill is `color.surface`, and its border `color.err` for `err`;
  - a segmented select at `stretch` in 300 px has segments of equal width within 1 px.
  - **Control:** map `DangerOutline` to `Danger`, and the fill reads `color.err`.
- `test_tui_widgets.cpp` / `test_tui_lists.cpp`:
  - a `dangerOutline` button's cells are red and not bold, and a `danger` one's are bold;
  - an outline badge draws `(STAT)`;
  - a segmented select draws `All 24 │ Flagged 2` with the marked one in the selected style.

**Gallery.**

- Receipt and review: "Reject sample…" and "Reject…" become `dangerOutline`. The reject dialogs' confirming "Reject
  sample" stays `danger`: inside the dialog, it is the action.
- Samples: priority tags become `outline` badges.
- Review: "Show" becomes a `segmented` select, and the gallery's `radio` column goes.

**Risks.** A restyled `MorphButton` that switches on `emphasis` values sees a new one. Restyles fall back to `normal`
for an unknown value today (`MorphButtonBackground`'s ternaries), so it draws as an ordinary button.

## G52: a selectable list entry of several lines

**Problem (read; the gap log's grab stands).**

- A `style: item` button is the selectable entry (`document.md:165`). Its `label` is one string, and the tint and
  the bar cover its one line (`MorphButtonBackground.qml:23-40`).
- 09's queue entry is a card of three lines: id and age, test and analyst, and a row of tags. The whole card is
  clickable and tinted while it is current.
- The gallery's workaround is a column of an item button and three further nodes (`review.cpp:437-447`, at
  `b080a5265`), so only the first line takes the click.
- Batch B's G41 is a selectable push button (the plate's wells): `selected` on a push button, which keeps its frame
  and draws an accent ring. It is not about table cells, and it does not overlap this item: content stays an item's,
  and a push button keeps its label.

**Decision: an item may hold content.** A `button` with `style: item` takes `children`, drawn as the entry in place
of its label. The `label` stays required, and becomes the entry's accessible name, not drawn. The entry is still one
button: one focus stop, one activation, one current mark across all its lines.

- **The content is display only.** Inside an item's `children`, at any depth, the loader allows the display kinds
  only: `text`, `badge`, `keyValue`, `progress`, `steps`, `busy`, `spacer`, and the arrangements `column`, `row`,
  `grid`, `forEach` and `switch`. Every other kind is refused, including those whose interaction is a part
  (`banner`'s dismissal and action, `emptyState`'s action, `panel`'s and `collapsible`'s toggle, `scroll`), and so is
  a `keys`, `drag`, `drop` or `autofocus` on any node there. A list of what is allowed, rather than of what is not,
  keeps a kind added later out until it is judged. The refusal reads "an entry's content is read-only; put the
  control beside the entry".
- Nested interactive controls inside a button are the case HTML forbids for the same reason: the inner control's
  activation and the entry's would compete.
- `children` without `style: item` is refused: "only a list entry holds content; a push button has a label".

**Vocabulary.**

```json
{"kind": "button", "style": "item", "selected": {"fn": "eq", "args": [{"ref": "selected"}, {"row": "id"}]},
 "label": {"fn": "format", "args": ["{0}, {1}", {"row": "id"}, {"row": "age"}]},
 "onClick": [{"set": "selected", "value": {"row": "id"}}],
 "children": [{"kind": "column", "gap": 0, "children": [
   {"kind": "text", "spans": {"list": [{"obj": {"text": {"row": "id"}, "mono": true, "strong": true}}, " · ", {"row": "age"}]}},
   {"kind": "text", "role": "muted", "text": {"row": "test"}},
   {"kind": "row", "gap": 1, "children": [{"kind": "forEach", "rows": {"row": "tags"}, "axis": "horizontal",
     "children": [{"kind": "badge", "text": {"row": "text"}, "tone": {"row": "tone"}}]}]}]}]}
```

Author: `ButtonOptions::children` (`Nodes`), checked by the loader as above.

**Lowering.** The `button` branch (`interpreter.hpp:6038-6061`) lowers `children` with `lowerAll` into `Button::child`
when the style is `Item`.

**View tree and contract.**

- `Button` gains `Node child`, only for `Item`. The mount refuses (`std::logic_error`) a child on a push button.
- The mount mounts the child into `ButtonWidget::content()`. That is a `ContainerWidget&` the button owns, asked for
  only when the item has a child, as an overlay's `footer()` is asked for only when an `ActionBar` mounts
  (`backend_contract.md:148`).
- While the button has content, its label is not drawn and is its accessible name.
- A click anywhere on the content activates the button. Content widgets take no input of their own, which the loader
  guarantees for documents. A hand-built tree that puts an input there is the author's mistake, and the backend gives
  the button the click.

**Per backend.**

- **Qt Quick.**
  - `MorphButton` gains a `content` item, a `ColumnLayout` used as the `contentItem` while it holds children, with the
    item's padding.
  - The background's tint and bar span the whole height, since `MorphButtonBackground` already fills the button.
  - Children are inside the `Button`, so a press anywhere on them is the button's.
  - `Accessible.name` is the label.
- **Terminal.**
  - The item becomes a container while it has content. Its rows are its content's rows, laid out as a column.
  - The selected or focused style is painted across every row first, and the content paints over it with its own
    roles' colours, keeping that background.
  - Enter, Space and a click on any row activate it.
- **`RecordingBackend`.** `content()` creates a `Column` child of the button, logged as `create Column#n in
  Button#m`. `click(button)` behaves as before.

**Spec.**

- `document.md:165`: `button` gains `children` (only with `style: item`, display kinds only), in the row as batch B's
  G41 leaves it.
- `view_tree.md:105`: `Button::child`.
- `backend_contract.md`:
  - **Button style and selection**, as batch B's G41 leaves it, gains the content paragraph;
  - the `ButtonWidget` row gains `content()`;
  - RecordingBackend: the `Column` child.
- `qt_quick/backend.md` and `tui/frontend.md`: the item with content.
- `author.md`'s accessibility section: an item with content is named by its label, which carries what a listener
  needs from the content.

**Tests.**

- **Conformance case 50.** An item button with a bound `selected` and a two-text column as its child:
  - `childCount(content)` is 1, and the column's first text reads its text;
  - `click(item)` runs `onClick` once;
  - `click` on the content's first text runs `onClick` once: a click anywhere on the entry is the entry's.
    `RecordingBackend`'s `click` on a widget inside an item's content clicks the item, and the Qt Quick and
    terminal probes click at the text's place, so their own input path decides;
  - inside a hidden container, neither click runs anything, as the hidden-container case requires of any widget.
  - **Control:** a backend that leaves a click on the content to the content (on Qt Quick, the content parented
    outside the `Button`) fails the click on the text.
- `test_ui_document.cpp`:
  - `children` on a push button is refused;
  - a `textInput` two levels inside an item's `children` is refused at its path, and so are a `banner`, and a
    `text` with `keys`;
  - a `badge` there loads.
- `test_qt_quick_widgets.cpp`: an item with a three-line content, selected. The background's height equals the
  button's, the bar spans it, and a mouse press on the third line's text clicks the button. **Control:** put the
  content outside the `Button`, and the press reaches nothing.
- `test_tui_widgets.cpp`: a selected three-row item paints the selected style on all three rows.

**Gallery.** The review queue's entries become items with content. The column workaround goes, and the whole card
takes the click.

**Risks.** A screen reader reads the label, not the content. That is the label's purpose, and the loader's
accessibility rule already requires one on a button (`author.md:239`). So whatever a listener needs from the content,
such as an out-of-specification tag, belongs in the label too; `author.md`'s accessibility section says so for an
item with content.

## G13: a date and time display format

**Problem (verified by trying, gap log; cause read).**

- The `date` and `dateTime` functions have the styles `short`, `medium` and `long` (`expressions.md:147-149`). They
  call `FormatContext::dates` when it is set (`expression.hpp:1525-1538`), and write ISO text otherwise. Nothing
  sets `dates`: `grep -rn '\.dates\b' include src examples` finds only the reader and the table library's own,
  unrelated `services.dates`.
- No client calls `Session::setFormatting` at all: no caller in `src` or `examples` outside its definition
  (`interpreter.hpp:938`) and the session's own `setLocale` and `setTranslations`, which nothing calls either.
- `dateTimeInput` has no format key (`document.md:181`). Both frontends type and show a fixed `yyyy-MM-dd HH:mm`
  (`MorphDateTimeInput.qml:6,104`; the terminal's widths of 12 and 18, `field_widgets.cpp:416`).
- Regulated labs write "24-Oct-2026 09:41". The day-month-name-year order cannot be misread across locales, which is
  why GMP documentation uses it (02, 04, 06).

**Decision: one pattern per application, not one per node.** A lab writes dates one way everywhere: in the audit
trail, the records, the inputs and the banners. A per-node format would be repeated on every screen and drift
between them. So the pattern belongs to the session's formatting, and every date the client shows reads it.

- `FormatContext` gains `std::string datePattern` and `std::string dateTimePattern`. Empty, the default, keeps
  today's ISO forms.
- `date(x)` and `dateTime(x)` format with the pattern when it is set and no `DateFormatter` is, in every style:
  `short`, `medium` and `long` alike, so one screen never mixes two forms of a date. The style chooses a form only
  when no pattern is set. A host's `DateFormatter` still wins, since it is the host's explicit choice.
- `dateTimeInput` shows and reads its value in the pattern. It also reads ISO, so a pasted ISO date still works.
- **Pattern grammar.** The tokens are `yyyy`, `MM`, `MMM`, `dd`, `HH`, `mm`, `ss` and `xxx`. `xxx` is the display
  zone's offset from UTC in the `+02:00` form (`+00:00` for UTC), from `utcOffsetMinutes`. Zone names such as `CEST`
  are not a token: the client holds an offset, not zone data. The literals are `-`, `/`, `.`, `:`, `,` and a space. `MMM` is `Jan` … `Dec`, in English whatever the locale, read without regard to case. That is
  the regulated convention. Month names are not localized (decision 4).
- **What a pattern must hold**, so that every text it writes reads back to the same instant: `yyyy`, `dd`, and one
  of `MM` or `MMM`, each once. A `date` pattern holds no time token. A `dateTime` pattern also holds `HH` and `mm`
  once each, and `ss` and `xxx` at most once each. No two tokens touch without a literal between them, except `MMM` beside a
  number, so `ddMMyyyy` is refused. A pattern that breaks any of this, or holds anything else, is refused where it
  is declared.
- **Reading an offset.** Text read with a pattern that holds `xxx` takes its instant from the offset it names, so
  `24-Oct-2026 09:41 +02:00` is 07:41 UTC whatever the display zone. A pattern without `xxx` reads the text in the
  display zone, as a date input does today.
- **Where an application sets it.** The app document (id `app`) gains `formatting`, a literal record
  `{"date": "dd-MMM-yyyy", "dateTime": "dd-MMM-yyyy HH:mm"}`. `Interpreter::mountApp` merges it into the session's
  `FormatContext` with `setFormatting`. A C++ host may set the same fields directly. A server-driven client gets it
  from the app document, the one document every screen of the bundle shares. This is new vocabulary on the app
  document only (decision 5).

**Shared code.** `include/morph/ui/date_pattern.hpp` (new):

- `ui::checkDatePattern(std::string_view) -> std::expected<void, std::string>`;
- `ui::formatDate(std::string_view pattern, time::DateTime local) -> std::string`;
- `ui::parseDate(std::string_view pattern, std::string_view text, std::int32_t offsetMinutes) ->
  std::optional<time::DateTime>`, which tries the pattern, then ISO.

Each has complete Doxygen. The expression formatter, the interpreter and both frontends call them, so the grammar
lives in one place.

**Lowering.** The `dateTimeInput` branch (`interpreter.hpp:6191-6246`) reads the session's `datePattern` or
`dateTimePattern` by its mode, once and untracked (`formatting()` is a tracked read, which would re-lower the input
on every formatting change), and sets `DateTimeInput::pattern`. A pattern change after mount applies to
inputs mounted later. That is the same rule as `offsetMinutes`, which is also read once (`view_tree.md:121`).

**View tree and contract.**

- `DateTimeInput` gains `std::string pattern`, set once. Empty means ISO.
- `DateTimeInputWidget::setPattern(std::string_view)` is pure. The mount calls it once, only when the pattern is not
  empty, so existing logs are unchanged. The widget shows its value with `ui::formatDate` and reads typed text with
  `ui::parseDate`.

**Per backend.**

- **Qt Quick.**
  - `DateTimeInputImpl` formats and parses in C++ already (`MorphDateTimeInput.qml:6`), and now does so through the
    shared functions.
  - The placeholder becomes the pattern in lower case (`dd-mmm-yyyy hh:mm`), with `xxx` shown as `+hh:mm`.
  - The calendar writes its chosen day through a C++ `formatDay(year, month, day)` exposed on the item, instead of the
    QML string at `:66-69`, so the field's text stays in the one pattern.
  - `chosenDate` parses through the same C++ call.
- **Terminal.** The date input's width becomes the pattern's display width, and its stepping keys (`field_widgets.cpp:
  468-472`) work on the parsed instant as now.
- **`RecordingBackend`.** It records `pattern`. `setDateTime` sets the instant as before.

**Spec.**

- `expressions.md:147-149`: every style reads `FormatContext`'s pattern when it is set, and the grammar, the offset
  token included, is defined.
- `document.md`: the app document's `formatting`, refused on a screen, with each pattern checked at load.
- `view_tree.md:121`: `pattern`.
- `backend_contract.md` **Date inputs**: shown and read in the pattern, and ISO always read.
- `interpreter.md:37` and the app-document paragraph (`:54`): `mountApp` sets the session's patterns.
- `qt_quick/backend.md` and `tui/frontend.md`.

**Tests.**

- `tests/test_ui_date_pattern.cpp` (new):
  - `formatDate("dd-MMM-yyyy HH:mm", 2026-10-24T09:41)` is `24-Oct-2026 09:41`;
  - `parseDate` reads it back, also as `24-oct-2026 09:41`, and reads `2026-10-24T09:41:00Z`;
  - `checkDatePattern("dd-MMMM")` fails, and so do `MMM-yyyy` (no day), `dd-MM-yyyy HH:mm` as a date pattern, and
    `ddMMyyyy`;
  - `parseDate` of `31-Feb-2026` is `nullopt`;
  - with `utcOffsetMinutes` 120, `formatDate("dd-MMM-yyyy HH:mm xxx", …)` of 07:41 UTC is `24-Oct-2026 09:41 +02:00`,
    and of the same instant at offset 0 ends in `+00:00`; `parseDate` of `24-Oct-2026 09:41 +02:00` at a display
    offset of 0 is 07:41 UTC. **Control:** read the text in the display zone, and the instant is 09:41 UTC;
  - `checkDatePattern` refuses `xxx` in a date pattern, and `xxx` twice.
- `tests/test_ui_expression.cpp`: with `dateTimePattern` set, `dateTime(x)`, `dateTime(x, "short")` and
  `dateTime(x, "long")` all write it, and with a `DateFormatter` set the formatter wins. **Control:** apply the
  pattern to `medium` only, and the `long` case writes ISO with seconds.
- `test_ui_interpreter_control.cpp`: an app document with `formatting` mounted before a screen. The screen's
  `dateTimeInput` records `pattern=dd-MMM-yyyy HH:mm`, and a `text` of `dateTime(x)` shows `24-Oct-2026 09:41`.
  **Control:** have `mountApp` skip `formatting`, and both read ISO.
- `test_ui_document.cpp`: `formatting` on a screen is refused, and `{"date": "dd-MMMM-yyyy"}` on the app document is
  refused at `/formatting/date`.
- `test_qt_quick_widgets.cpp`: a date input with the pattern shows `24-Oct-2026 09:41`. Typing `25-Oct-2026 10:00`
  and leaving reports that instant. Choosing the 26th in the calendar writes `26-Oct-2026 10:00`. **Control:** keep
  the QML string builder, and the calendar writes `2026-10-26 10:00`.
- `test_tui_fields.cpp`: the same display and typed edit on the terminal.

**Gallery.** The gallery's app document (or its harness's session) sets the pattern. Receipt, audit and stability show
"24-Oct-2026 09:41" in their inputs, and their `dateTime(…)` texts follow.

**Risks.**

- **Bundles without an app document** keep ISO. That is today's behaviour, and the default for a client that sets
  nothing.
- **An input's text changes form** for an app that sets a pattern, and a test that typed ISO still passes, since ISO
  is always read.
- **Locales that reverse day and month** gain nothing they lacked: the pattern is explicit and names the month.

## G11: a scanner field that stands out

**Problem (read; the gap log stands).** A `textInput` has no size or emphasis (`document.md:166`). Its leading
`icon` draws (verified in the gap log). The receipt's scan field is the screen's focal control: larger text in the
mono face, an accent outline and a soft glow (`02-Receipt.dc.html:34-35`, `.scan`).

**Decision: `emphasis: "prominent"` on `textInput`, mirroring a button's `emphasis`.** The vocabulary already says
"how much it stands out" with that key. A search box, a scan field or a lookup field at the head of a screen is the
same need, and the plate and console designs have one too. The mockup's glow is decoration. The frontend draws the
look its toolkit gives, and the vocabulary promises only that the field stands out. The scanned text is not set mono:
the field is cleared after each scan, and the sample's id is shown in mono where it lands.

- `emphasis` on `textInput` is a literal closed choice, `normal` (default) or `prominent`. It is refused on a
  multiline input, since a text area's size is its own.
- Author: `TextInputOptions::emphasis` (`InputEmphasis{Normal, Prominent}`).

**View tree and contract.** `TextInput::emphasis` (`InputEmphasis`) is set once.
`TextInputWidget::setEmphasis(InputEmphasis)` is pure and called only for `Prominent`. A prominent input is drawn so
that it stands out as the focal control: on Qt Quick larger and outlined in the accent, on the terminal in bold.

**Per backend.**

- **Qt Quick.** `MorphTextInput` gains `prominent`.
  - The text font is `Theme.fontOf(Theme.headingPixels, Font.DemiBold)`.
  - The height is `Math.max(Theme.targetHeight, Theme.touchHeight)`.
  - `MorphInputBackground` gains `prominent`, which draws a 2 px `scheme.accent` border at all times and, while
    focused, a 3 px ring of `Qt.alpha(scheme.accent, 0.2)`. A tone or errors (G10) win over the accent: the border
    and ring take the tone's colours, since a refused scan must look refused.
- **Terminal.** The field's text is bold. The input background is unchanged.
- **`RecordingBackend`.** It records `emphasis=Prominent`.

**Spec.** `document.md:166`, `view_tree.md:106`, `backend_contract.md`'s `TextInputWidget` row and RecordingBackend
properties, `qt_quick/backend.md:144`, and `tui/frontend.md`.

**Tests.**

- `test_ui_document.cpp`: `emphasis: "prominent"` with `mode: multiline` is refused.
- `test_qt_quick_theme.cpp`: a prominent input's height is at least `control.touchHeight`, and its border is 2 px in
  `color.accent`. **Control:** ignore `prominent`, and the border is 1 px `color.border`.
- `test_tui_fields.cpp`: the prominent field's text cells are bold.

**Gallery.** The receipt's scan field gives `emphasis: prominent`. Its icon, `autofocus` and the `focus` command
after each scan stay.

**Risks.** None outside inputs that ask for it. If the review prefers a smaller batch, this item can be dropped as a
whole: the gallery's workaround is functional, and the gap is Low.

## Not in this batch

- **Numeric inputs aligned to the end.** 08's result fields right-align their numbers (`.input.num`). Batch B's G47
  aligns a column's cells, a text input in a cell included, to the end; a standalone field outside a table is not
  aligned by it. 08's expanded fields sit in table cells and are covered there. The compact form's standalone fields
  stay start-aligned, and no gap asks for more.
- **A caption text role.** Field captions, a group's legend and a stacked key-value's caption are each drawn by their
  own component at the body size. The mockups draw captions at 12 px, weight 500. A `caption` role would let a
  document draw the same look in a `text`. No gap asks for it, and the theme has no size token for it.
- **Spans on other props.** Labels of controls, titles, badges and captions take plain text only. They are names, not
  prose, and the designs put no runs in them.

## Decisions

Each question this design raised, and its answer.

1. **Emphasis in translated text.** A span list fixes the order of its runs, so "as **{meaning}**" cannot be
   reordered by a translator. MessageFormat 2's markup (`{#strong}…{/strong}`) solves that inside a pattern, and is
   outside `expr/1`'s message subset (`expressions.md`, Messages). **Decided:** spans in ui/1. `t` with markup
   returning spans is a later `expr` version. A translated sentence emphasises whole sentences or splits around the
   placeholder.
2. **The forms engine's required mark.** A form's cell draws its caption and the control draws the mark, so G8's rule
   does not reach forms. **Decided:** leave forms as they are in this batch. A follow-up lets a stacked cell's input
   draw its own caption (`showLabel`), so one rule covers both. The inline layout keeps the mark beside the control.
3. **A group's `required`.** It is a mark only, and the requirement is a check. Should a group of check boxes derive
   "at least one checked" itself? **Decided:** no. The receipt needs three specific boxes, which a derived rule would
   not express, and a check already says it in one line.
4. **Month names.** `MMM` is English. **Decided:** English, the regulated convention. A localized `MMM` would read
   back differently per locale, which the pattern exists to avoid.
5. **Where the date pattern comes from.** **Decided:** the app document's `formatting`, merged by `mountApp`. The
   alternative, a bundle manifest member, is read by the client before any document mounts, and would need a new
   manifest field and a client path that sets formatting, which the client does not have today.
6. **A mono size.** The mockups set mono 1 px smaller than the body (12 px against 13 px), since a monospace face
   looks larger. **Decided** in batch B's G2, which owns the face: body size, no token. A `font.size.mono` can join
   the closed set later "with the components that read it" (`theme.md`).
7. **G11 at all.** **Decided:** keep it, last. Drop it if the review wants batch C smaller.
8. **Which tones a field takes.** **Decided:** all four. `ok` outlines a within-spec result green, which 08 does not
   draw but a lab may want. The gallery uses `err` and `warn` only.
