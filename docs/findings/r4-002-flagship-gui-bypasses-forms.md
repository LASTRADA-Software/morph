---
id: r4-002
title: The ladder's designated showcase GUI is hand-built end to end with zero morph::forms usage and no rule-2 justification
subsystem: forms
severity: minor
source: kanban rung 4, README truth pass
disposition: open
test: spec-cited
---

`examples/LADDER.md` designates kanban **the single polished showcase** — the
one rung the audience is pointed at, and the only one permitted to spend
effort on presentation. Its GUI uses none of `morph::forms`.

`grep -rn "morph::forms\|MorphForms\|FormsController\|schemaJson"` over the
rung's `gui/`, `gui_lib/`, `src/`, `include/` and `tests/` trees matches three
comment lines and no code (`gui/qml/LoginView.qml:6`,
`gui_lib/board_qml_bridge.hpp:449`, `gui_lib/board_presenter.hpp:161`, the
last two naming *polls*' controller as a routing precedent). Every input in
the rung's seven QML files is a hand-written control:
`gui/qml/LoginView.qml:63`, `gui/qml/RulesView.qml:89`, `:97`, `:103`,
`gui/qml/MembersView.qml:56`, `:81`, `:87`, `gui/qml/ProjectListView.qml:125`,
plus four in `gui/qml/BoardView.qml` and one in `gui/qml/TaskDetailPopup.qml`
(cited without line numbers: both files are being edited on another branch).
Sibling rungs render the same *kinds* of form through the shipped renderer
(`examples/bookmarks/gui/qml/LoginView.qml`,
`examples/polls/gui/qml/VoteView.qml`, `examples/lims/gui/qml/SampleView.qml`,
`examples/pastebin/gui/qml/Main.qml` all reference `MorphForms`).

`examples/IMPLEMENTATION.md`'s rule 2 makes hand-built input widgets
"forbidden by default" and allows exactly two justifications, each of which
has to be **written in the rung README**: (a) the generated UI cannot express
the interaction — "which is precisely a forms-subsystem finding, so file it on
the gap ledger" — or (b) pure glue with no domain logic. Neither is written.
The nearest thing to a rationale is a code comment
(`gui/qml/LoginView.qml:3-7`) attributing the decision to
`docs/superpowers/specs/2026-08-17-kanban-gui-design.md` §4; that section
specifies the two-bridge split and JSON property-bag binding and never
mentions forms, schemas, or `DynamicForm` — the whole GUI design spec contains
exactly one occurrence of the string "Form" (`:182`), inside the name of
bookmarks' `FormsBridge`, cited there only as the auth-flow precedent.

The consequence for the program, not just for the rung: the ladder's largest
and most-shown GUI contributes **zero** stress to the forms subsystem, and
whichever of rule 2's two justifications actually applies, the artifact that
was supposed to fall out of the decision — a forms-subsystem finding, or a
written justification — does not exist. If (a) applies, the gap ledger is
missing entries for whatever `morph::forms` could not express here (a
drag-and-drop board is a plausible (a); a login field, a "new column"
name + WIP-limit pair, a role dropdown and a rule-definition row are not
obviously so). If (b) applies, the README owes the sentence.

**Verification status.** Read at master `9371c1a0`: the greps above over
`examples/kanban/` (input controls enumerated from a
`TextField|ComboBox|SpinBox|Dialog` grep across all seven QML files, with
`gui/qml/LoginView.qml` read in full), `examples/IMPLEMENTATION.md` rule 2,
and `docs/superpowers/specs/2026-08-17-kanban-gui-design.md` — its §4 read in
full plus a whole-file grep for `forms`/`schema`/`Form`, which is what the
"one occurrence" count above rests on. **Nothing was built or run, and no
attempt was made to render any of these forms through `morph::forms`** — so
this finding asserts that the justification is missing, *not* that the forms
subsystem is incapable. Which of the two is true is exactly what triaging it
has to decide.

**What would change the verdict.** Either the rung README gains rule 2's
written justification per custom element (closing this as a documentation
defect), or the attempt to render kanban's forms through
`morph::forms::schemaJson<A>()` produces concrete gaps, which then become
their own findings and this one becomes their index. Related, but not the
same: morph#304 §B1 records that four *other* rungs pair `controller: null`
with hand-written submit buttons while still using forms — kanban is absent
from that list because it uses no forms at all.
