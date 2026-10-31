I found two blockers: K4 misses the e-signature case that motivated it, and K5 leaves a stale input that the design says it fixes. There are also four should-fixes and some nits. Design line numbers below refer to `docs/superpowers/specs/2026-10-31-lims-gallery-batch-k-design.md` at 15cd3a55b.

## Blockers

**B1. K4 doesn't cover the screen it is for.** (Design :256–262, :284; read.)
- The e-signature 390 grab is the signature dialog drawn as a sheet. Its fields are the dialog's body children. `lowerDialog` lowers them with a freshly built `innerCtx` (`interpreter.hpp:8608`) and holds them in `holding(…, bodyGap)` (`:8629`).
- The design sets `listGap` only for the `column` kind, `lowerArranged` and `lowerAll`, and says every caller that builds a `LowerCtx` resets it to 0. So the switch in the dialog body still gets a gap-0 holder, and the "Verify" fails.
- The "step 2, so 16 px" arithmetic is the other path, the `called` case's `ui::column(…, {.gap = 2})` (`esignature.cpp:243`). The dialog body's gap is `kDialogGap` = 3 (`:6796`).
- **Change:** `lowerDialog`'s `innerCtx` sets `listGap = bodyGap`. Add a dialog-body case to the K4 tests, and correct the expected spacing in "Verify".

**B2. K5's fix leaves the one-line elision stale.** (Design :309; reproduced on Qt 6.12, `$S/lg-kr/fontdep.qml`.)
- The cause is confirmed. Setting the text and then changing the font, `ma.advanceWidth(text)+1` stays at 100.97, while the same expression gated on a read of `ma.font` follows to 111.375.
- But `fitted` (`MorphText.qml:32`) is a separate `metrics.elidedText(…)` binding, not "the same binding". In the same probe it stayed unelided after the font change (`elided System suitability` against `elidedDep System suitabi…`).
- So a mono or `singleLine` text whose role is set after its text, in a column of fixed width (whose `controlWidth` doesn't move), draws un-elided past its width.
- **Change:** `fitted` reads the font too. Add a test: a `singleLine` text in a fixed-width parent, role set after the text, that elides.

## Should-fix

**S1. The K5 dependency read must not be gated on `pixelSize`.** (Design :306; read.)
- The obvious model is `Theme.qml:646`, `font.pixelSize > 0 ? … : 0`. But for an app without `font.family`, `strongFont` is `boldOf(platformMetrics.font)` (`Theme.qml:69`). That keeps a point size, so its `pixelSize` is -1, and every Section, Overline or caps text would measure 0.
- **Change:** read a member that always holds, e.g. `metrics.height >= 0 ? … : …`.
- Binding loops: none from this read. `label.font` depends on the role and the Theme only, never on width (read). `wrapSlack` 1 holds: my repro shows label 111.375 against advance 110.375. I could not run the Qt 6.8.1 qmllint here; only 6.12 is installed.

**S2. K1: a token set on the base never reaches the built-in surfaces.** (Design :118–122; read.)
- `theme.cpp` never writes `themeScheme`. It writes `Theme` properties, and `themeScheme`, `page`, `card`, `toolbar` and `bar` bind to those (`Theme.qml:285–528`).
- With "every built-in scheme leaves `hasSelectionText` false", a base-level `color.selectionText` reaches only surfaces declared in the theme index. That breaks the precedent `theme.md:201` states for `list.selection` ("a surface that does not set it keeps the theme's, the built-in sidebar and bar included").
- **Change:**
  - add `Theme.selectionText` and `Theme.hasSelectionText`, written in `setPlainTokens`, with `false` when the token is absent;
  - bind `themeScheme`, `page`, `card`, `toolbar` and `bar` to them;
  - leave the built-in `sidebarScheme` false, since its text and selection are its own, and have the spec say so;
  - add a test: base-only token gives `cardScheme.selectedText`.

**S3. K1: a table's selected row doesn't follow the token.** (Read.)
- `MorphTableRowBackground.qml:60` fills a selected row with `scheme.selection`, and its cells keep `scheme.text`.
- No current theme breaks: the existing rule keeps `color.text` at 4.5:1 on `color.selection`. But one token would mean "selected text" for list items and not for table rows.
- **Change:** either selected rows read `selectedText` (K-C adds `MorphTableRow*.qml`), or `theme.md` scopes the token to item-style buttons explicitly.
- I checked the other places a selected item draws content:
  - item-style buttons (label, icon, content via `contentScheme`) are the design's scope;
  - tabs (`MorphTabs.qml:190`), the select's highlight (hover) and the push-button selection ring don't use `selection` as a text background;
  - the terminal uses `listItemSelected` (`leaf_widgets.cpp:308`).

**S4. K2: the principal can stay hidden after a later `setPrincipal`, and an empty trail slot takes a gap.** (Design :188, :195.)
- **Stale visibility (read).** `_topPrincipal`'s visibility is computed only in `showChrome()` (`shell_frame.hpp:398`). `setPrincipal` (`:316–321`) never recomputes it.
  - The client's sign-in path calls `showSignedIn(true)` → `showChrome()` with the old empty name, then `setPrincipal(name)` (`shell.cpp:468–470`, via `:729`). So the head principal stays hidden after a sign-in that follows a sign-out.
  - "Ends in `showTrail()`" fixes this only if `showTrail()` also recomputes `_topPrincipal`.
  - **Change:** move that line into `showTrail()`, and add a RecordingBackend test: `setPrincipalInHead(true)`, then `setPrincipal("x")`, visible.
- **Phantom 12 px gap (reproduced on 6.12, `$S/lg-kr/emptygap.qml`).** A visible empty item in a non-wrapping layout takes the gap: width 78 against 66.
  - `setHeadShown(true)` shows `_headSlot` whenever the head has an eyebrow or a lead, even with no trail (`workspace.hpp:827`). So on stability, review and e-signature the new `_trail` is [empty slot][12 px][principal]. The 12 px counts against the wrap budget, and the terminal gets the same gap.
  - "Holds the principal alone" (:195) is therefore false.
  - **Change:** either make `_trail` a `wrappingRow` (a basis-0 child takes no gap: `MorphStack.qml:149`, `:233`), or have the client tell the frame whether the trail has content.
- **Other checks on K2/K3, all fine.**
  - `principalPlacement: sidebar` apps are unchanged: `_trail` shows exactly when `_headSlot` did, and is a basis-0 child when empty, as before.
  - The J5 title line and `_leadBlock`'s minimum are untouched, and the width arithmetic (761, 597, 418.5) checks out.
  - Compact still hides the principal.

## Nits

- **K4 wording and leaks** (design :257–261).
  - The `column` kind lowers its children through the `children` lambda (`interpreter.hpp:7840`), which the design says resets the gap. So that lambda needs a gap argument.
  - Grid cells pass `ctx` straight through (`:7873`), so a switch in a grid cell would inherit an outer column's gap. The same goes for the horizontal branch of `lowerArranged` (`:9813`) unless it resets explicitly.
  - Simpler and leak-proof: `lower()` zeroes `listGap` for every kind except `switch`, and only the holders set it.
  - A `forEach` row's multi-node holder (`:8866`) has the same gap-0 shape. Mention it under "Not in this batch".
- **K1 test count.** `tests/test_ui_theme.cpp:204–206` asserts that the value maps sum to `kThemeTokens.size()`. With the empty-default colour left out, it must become `size() - 1`. Name this in K-C's test list.
- **K8.**
  - A third-party theme that set `elevation.card` to restyle its dialogs silently loses that. No example app sets it (only the gallery sets `elevation.popup`). Say so in `theme.md`, or default `elevation.dialog` to follow `elevation.card` the way `selectionText` follows `text`.
  - The shadow `Rectangle` (`MorphDialog.qml:104`, `MorphDrawer.qml:105`) has no `objectName` for the test to find; give it one.
- **K1 naming.** `markedSelection` reads oddly next to "a look names only the mark". `itemSelected` would be plainer.
- **K1 re-apply test.** `setSurfaces` only writes surfaces present in the new index, so "a re-apply without the token" must keep the same surface declared, or the test checks nothing.
- **Lanes.**
  - K-A's conditional removal of K-S's e-signature wrapper edits K-S's file. Leave it to K-S.
  - B1's fix stays in `interpreter.hpp` (K-A).
  - S3, if taken, adds `MorphTableRow*.qml` to K-C.
  - S4 stays in K-B: `shell_frame.hpp`, plus `shell.cpp`/`workspace.hpp` if the client route is chosen.
  - Otherwise the split is disjoint. The order K-A → K-B → K-C with K-C owning the shared widgets test is workable, and no framework commit waits on K-S.
- **P6 independently confirmed.** `qml6` offscreen printed nothing without `QT_FORCE_STDERR_LOGGING=1`, and printed with it.

## Items with no finding

- **K6:** the cause and both rule sites are confirmed (`MorphDialog.qml:217`/`:257`, `MorphDrawer.qml:152`/`:191`). The control fails as described.
- **K7:** the cause is consistent with P4. The content's `leftPadding` is `indicator.width + spacing` (`MorphCheckbox.qml:32`), so zero side padding plus step 2 gives the mockup's 0 and 8 px. Qt 6.8 is fine.
- **K9:** keeping the calendar button is correct. It is the only way to open the calendar.
- **Terminal:** handled correctly by each item's "Per backend" paragraph, apart from S4's phantom gap.

Everything was done read-only. Probes are in `/tmp/claude-1000/-home-yaraslau-repo-morph/8742d70b-a059-4ed6-9415-5f45a5067fd9/scratchpad/lg-kr/`. Nothing was built.