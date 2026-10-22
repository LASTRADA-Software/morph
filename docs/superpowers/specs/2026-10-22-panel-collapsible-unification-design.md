# Panel/Collapsible vocabulary unification — design

Addresses #1020. `Panel` (`include/morph/ui/view.hpp` ~623-640) and `Collapsible` (~1088-1101)
independently implement the same state machine — a title, a user-driven open/closed toggle, content
hidden while closed, a header area that stays usable regardless — as two separate struct shapes with
no shared code:

```cpp
struct Panel {
    Prop<std::string> title;
    bool collapsible = false;      // set once: can the user collapse it at all
    Prop<bool> collapsed = false;  // used only when collapsible
    std::function<void(bool)> onToggle;
    Node header;  // shown beside the title
    Node child;
    int padding = 0;
    // ...
};
struct Collapsible {
    Prop<std::string> title;
    Prop<bool> open = true;        // always collapsible — no "collapsible" flag
    std::function<void(bool)> onToggle;
    Node header;
    Node child;
    // no padding
};
```

## What's actually different, not just duplicated

Reading both Qt Quick implementations (`MorphPanel.qml`, `MorphCollapsible.qml`) in full, past the
toggle-logic duplication #1020 names, surfaces a second fact the issue doesn't mention: **the two
widgets render different chrome, not just the same chrome with different field names.**

| | `Panel` | `Collapsible` |
|---|---|---|
| Framing | A bordered, backgrounded card (`Rectangle` with `scheme.surface`/`radiusCard`/`scheme.border`) | None — a bare `ColumnLayout`, inherits whatever surrounds it |
| Padding | `padding` (a spacing step around the card's content) | None |
| Title + toggle | Separate: a `ToolButton` (icon or glyph) beside a `Label` | Combined: one `ToolButton` whose own `text` is the chevron glyph plus the title |
| Can be non-collapsible | Yes (`collapsible = false`): title shown, no toggle, divider still drawn if a header exists | No — always has a toggle; there is no "always open, just titled" mode |
| Divider between header and content | Yes, drawn whenever the header row is shown | No divider |
| `focusTarget` | Not set explicitly (inherits default) | The toggle button |

So "unify the vocabulary" has two independent decisions bundled into it, and they need to be told
apart before any code moves:

1. **Share the toggle/open-state logic** (the actual duplication #1020 points at: `onClicked: { root.X
   = !root.X; root.fire("toggle", root.X) }`, byte-identical in both files bar the property name).
2. **Decide whether `Panel` and `Collapsible` become one widget with a chrome flag, or stay two
   widgets sharing only the underlying toggle behavior.** These are not the same question — (1) can
   be done without touching (2) at all.

## Decision

**Do (1) now, defer (2).** Unifying the toggle behavior is a pure internal refactor with no visible
behavior change and no public API change: factor the open/closed toggle state machine (flip a bool,
fire `toggle` with the new value) into one shared QML type both `MorphPanel.qml` and
`MorphCollapsible.qml` compose or inherit from, keeping every existing visible difference (framing,
padding, divider, combined-vs-separate title/toggle) exactly as it is today. This closes the literal
duplication #1020 names without touching either widget's public `view.hpp` struct, so every existing
call site (`tests/test_ui_mount.cpp:487,618`, `tests/test_ui_view.cpp:127` — the only three
construction sites in the repo today) keeps compiling unchanged.

**Do not** collapse `Panel::collapsible`/`collapsed`/`onToggle` onto `Collapsible::open`/`onToggle` as
one shared public struct in this pass. That would be a real, breaking API change (renaming
`Panel::collapsed` to `Panel::open`, or folding `Panel` into a `Collapsible` with an added `framed`/
`padding` option) that also has to answer the chrome question above — does a "framed Collapsible"
exist as a real combination (title + card border + no separate collapsible flag, always-open-capable),
or does `Panel`'s `collapsible = false` case (a titled, non-collapsible card) have no `Collapsible`
equivalent and therefore can't just become one? Table above shows these are genuinely different
widgets with overlapping behavior, not one widget wearing two names — deciding to merge them as a
single public type is a real design call, not a mechanical rename, and should get its own issue/design
pass with its own review, not be smuggled in under "fix the duplication."

## Scope of this fix

- New shared QML component (name TBD at implementation time, e.g. `MorphToggleHeader` or inlined as a
  private `Behavior`-style helper) owning: the `open`/`collapsed`-equivalent bool, the `toggle` fire
  call, and nothing about layout or chrome.
- `MorphPanel.qml`: keeps its own card framing, padding, separate title+toggle layout, divider; only
  the toggle-and-fire logic delegates to the shared piece.
- `MorphCollapsible.qml`: keeps its own combined title/toggle button, no framing; same delegation.
- No change to `include/morph/ui/view.hpp`, `author.hpp`, `interpreter.hpp`, `document.hpp`, or any
  spec doc — the public vocabulary is untouched by this pass.
- No change to any existing test's assertions — this is an internal QML refactor, verified by the
  existing `Panel`/`Collapsible` toggle tests (`test_ui_mount.cpp:475`, the `test_ui_interpreter_
  commands.cpp` panel case added for #1019) continuing to pass unchanged, not by new tests specific to
  the refactor itself (there is no new public behavior to test).

## What's left open for a future pass (not this fix)

Whether `Panel` and `Collapsible` should become one public type is a real product/API question this
design deliberately does not answer: it depends on whether a "framed, always-open, titled-but-not-
collapsible" card (today's `Panel{collapsible: false}`) is a shape `Collapsible` should also be able to
express, which is a UI-vocabulary decision, not a refactor. Flag this as the deferred half of #1020 (or
a new issue) rather than resolving it here.

## Final verdict on the toggle-logic extraction (the part this design *did* scope in)

Reading both files side by side closer than the issue's own comparison did surfaces one more fact: the
two widgets are not even identical in the one place #1020 calls out as shared — `Panel` draws a divider
between its header row and its content (`MorphPanel.qml`'s `objectName: "divider"` `Rectangle`,
conditioned on `headerRowShown`); `Collapsible` has no divider at all. So "extract the shared toggle/
chevron/divider behaviour into one place both call" (the issue's own suggested fix) is not actually
extracting identical code — doing it as literally stated would add a divider to `Collapsible` that it
does not have today, a real visible change, not the no-op refactor this design's own scope section (1)
promised.

What is genuinely, byte-for-byte identical between the two files is three lines: flip the bool, fire
`"toggle"` with the new value. This kit has no existing convention for sharing that much logic across
two QML files below the level of a full shared visual component (no `.js` helper modules anywhere in
`src/qt_quick/qml/`, no non-visual `QtObject` behavior pattern) — introducing one for three duplicated
lines would add more surface area (a new file, a new import in both widgets, a new thing a future
reader has to go look up) than it removes.

**Verdict: stays independent, per the issue's own stated fallback** ("if a design review decides
`Panel` and `Collapsible` should stay fully independent... this closes as wrong-layer/invalid with that
reasoning recorded"). The duplication is real but small (3 lines) and already slightly diverged (the
divider), and no mechanism in this codebase shares logic at this granularity without adding more than it
saves. The actual, larger duplication #1020 points at — two parallel public vocabulary concepts for
"collapsible section" — is the real issue, and that is the part already correctly deferred above as a
separate design/product decision, not something this pass should try to paper over by forcing a
3-line extraction that doesn't actually address it.
