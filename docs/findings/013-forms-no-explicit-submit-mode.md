---
id: 013
title: Shipped forms renderer auto-fires on validity, no explicit submit
subsystem: forms
severity: blocker
source: examples/LADDER.md, forms-subsystem gaps
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/52
---

The shipped forms renderer (QML/Qt `MorphForms`) auto-fires (auto-dispatches) an action the moment all required fields are engaged and all rules are satisfied, with no explicit submit button. This is safe for read-only queries (rung 0's pastebin `GetPaste` call) but catastrophic for any side-effectful form (rung 1's `CreatePaste` action must not fire on every keystroke in a field).

**What blocks this:** rung 1 needs explicit-submit mode before any side-effectful form can ship. The renderer must support an opt-in "submit button required" mode, and the schema must carry a signal for the renderer to engage it. Without this, rung 1's forms cannot safely model `CreatePaste`, the first side-effect operation in the ladder.
