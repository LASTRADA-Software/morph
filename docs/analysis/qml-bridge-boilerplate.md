# QML bridge boilerplate: B vs C, re-measured at six rungs

Analysis for [#86](https://github.com/LASTRADA-Software/morph/issues/86)
("Per-model hand-written QObject bridges don't scale"). The issue's numbers
were taken when four examples existed. This re-runs them against the tree as
it stands, evaluates options **B** (`QQmlPropertyMap`-backed generic
projection) and **C** (build-time codegen from glaze reflection), and
recommends.

Measured against `analysis-qml-bridges` (branched from `16dfc1b`, which
carries rungs 1–5). Rung 6 (`lims`) is read from `ladder-lims-rung6`
(`00e426b`), since it is not yet on this branch point. Qt spikes run against
the locally installed **Qt 6.11.1**; CI pins 6.8.1 (`ci.yml`) and 6.8.3
(`wasm-ladder.yml`).

---

## Recommendation

**Do not build B. Do not build C. Build neither a projection engine nor a
generator.**

Instead do the small, boring thing the measurements actually point at:

1. **Lift the model-*independent* helpers that are currently copy-pasted**
   into a shared header — `exception_ptr` → `QString` message rendering
   (duplicated in **18 files**) and the strong-id ↔ QML `qlonglong`/`QString`
   conversions `idNumber` / `idFromText` (duplicated in **9 files**, four of
   them byte-for-byte identical inside the `ledger` rung alone). This is
   ~130 lines removed, needs no macro, no moc trickery, no reflection, and
   carries none of the silent-failure risk that sank the previous attempt.
2. **Generalise the metaobject-surface assertions** that `bookmarks` already
   has (`examples/bookmarks/tests/test_bookmark_qml_bridges.cpp:278-380`)
   into a small shared test helper, and apply it to every bridge. This is
   the drift guard, and it is worth having *whether or not* anything is ever
   generated — it is what caught the `Q_MOC_RUN` hazard in PR #155.
3. **Close #86** with the measurements below, and record in
   `docs/todo.md`'s "Considered and refused" table that the projection half
   is per-model by construction.

The reasoning is that the premise the issue rests on does not survive
measurement. Three findings, each independently sufficient:

- **The boilerplate is not O(models).** It is O(QML surface), at a very
  stable **12.4 lines per exposed property/invokable/signal** (range
  9.7–20.0 across six codebases, n=218 elements, 2705 lines). `bank` has 11
  models and 54 surface elements; `polls` has 1 model and 16. Bridge size
  tracks how many things the *screen* does, not how many models exist. A
  QML surface is a UI design decision; nothing can derive it from a DTO.
- **The mechanical share is ~25–33%, not 60–70%.** Hand-classified, the
  reference file `examples/pastebin/gui_lib/paste_qml_bridges.cpp` is
  **52 per-model / 25 mechanical** code lines, and
  `examples/bank/gui/controllers/AccountController.cpp` is **37 / 11**. The
  issue's 60–70% figure is correct *for the forms wrapper alone* (which is
  ~100% mechanical) and was generalised to the whole file.
- **The forms wrapper — option A's entire target — is a dying pattern.**
  Rungs 4 (`kanban`) and 5 (`ledger`) contain **zero** references to
  `FormsControllerCore`, `schemasJson` or `schemaJson<>`. Rung 3 (`polls`)
  folded its forms surface into its domain bridge rather than keeping a
  separate class. Two forms wrappers remain in the whole tree.

And the direction of travel is the opposite of what the issue assumed: it
treats `bank`'s list/detail controllers as an outlier ("architectural scope
gap"). Measured now, **`ledger` and `kanban` have converged on exactly
bank's shape** — `Q_PROPERTY(QVariantList …)` + `busy` + `lastError`, rows as
`QVariantMap`. Bank is the majority pattern, not the exception. Twelve of
the nineteen QML-facing classes are list/detail shaped.

---

## 1. Inventory: what actually exists

36 files under `examples/` contain `Q_OBJECT`. They hold **33 QObject
classes in GUI code**, plus four non-bridge `QObject`s (two `App` shells,
`testkit/fault_proxy.hpp`'s `FaultProxy`, `kanban`'s `AttachmentServer`).

| kind | classes | Q_PROPERTY | Q_INVOKABLE | signals |
|---|---:|---:|---:|---:|
| presenter (`Presenter` subclass, **not QML-facing**) | 11 | 0 | 0 | 69 |
| domain bridge (ladder, QML-facing) | 11 | 23 | 60 | 66 |
| list/detail controller (`bank`) | 6 | 16 | 23 | 14 |
| forms wrapper | 2 | 2 | 2 | 3 |
| base class (`Presenter`, `BankController`) | 2 | 0 | 0 | 3 |
| reference (`examples/forms`, the only `QML_ELEMENT` user) | 1 | 4 | 3 | 2 |
| **total** | **33** | **45** | **88** | **157** |

The issue counted "20 hand-written QObject classes". That number conflated
two layers that the ladder deliberately separates:

- **Presenters** (`examples/common/gui/presenter.hpp`) are `QObject`s for
  testability — they expose `busy()`/`idle()`/`bound()` so tests can wait
  on quiescence instead of sleeping. They have **no properties and no
  invokables**; QML never sees them. Neither B nor C touches them, and they
  are not boilerplate in the issue's sense. They are 11 of the 33.
- **Bridges/controllers** are the QML-facing layer. There are 19 of them
  (+1 reference).

Per rung:

| rung | models | QML-facing classes | shape |
|---|---:|---:|---|
| 1 `pastebin` | 1 | 2 | `FormsBridge` + domain bridge |
| 2 `bookmarks` | 4 | 4 | `FormsBridge` + 3 domain bridges |
| 3 `polls` | 1 | 1 | forms surface folded **into** the domain bridge |
| 4 `kanban` | 2 | 2 | list/detail, property-heavy, **no forms** |
| 5 `ledger` | 3 | 4 | list/detail, property-heavy, **no forms** |
| 6 `lims` | 2 | **0** | **no GUI at all** |
| `bank` | 11 | 6 | list/detail, property-heavy |

### 1.1 Rung 6 changes the shape of the problem by not having one

`lims` on `ladder-lims-rung6` ships 21 files: core, db, dto, models, tests.
There is no `gui_lib/`, no `.qml`, no `Q_OBJECT`, and its README's
"What to implement" task list mentions no GUI, shell, bridge or presenter.
The rung the issue would most want to count as evidence of unbounded growth
contributes **zero** QObject classes.

This matters for the extrapolation: the issue's model is "every new rung
adds 2–4 more". The observed series is 2, 4, 1, 2, 4, 0.

---

## 2. The growth law: O(QML surface), not O(models)

| rung | models | props | invokables | signals | surface | bridge LOC | LOC/element |
|---|---:|---:|---:|---:|---:|---:|---:|
| `pastebin` | 1 | 1 | 5 | 6 | 12 | 132 | 11.0 |
| `bookmarks` | 4 | 1 | 11 | 16 | 28 | 289 | 10.3 |
| `polls` | 1 | 1 | 8 | 7 | 16 | 320 | 20.0 |
| `kanban` | 2 | 10 | 24 | 23 | 57 | 684 | 12.0 |
| `ledger` | 3 | 12 | 22 | 17 | 51 | 494 | 9.7 |
| `bank` | 11 | 16 | 23 | 15 | 54 | 786 | 14.6 |
| `lims` | 2 | 0 | 0 | 0 | 0 | 0 | — |
| **total** | 22 | | | | **218** | **2705** | **12.4** |

"Bridge LOC" is non-blank, non-comment lines in the `*_qml_bridge*.{hpp,cpp}`
of each rung (and `bank/gui/controllers/*`).

Against model count the relationship is noise: `polls` spends 320 lines on
one model, `bookmarks` 289 on four, `bank` 786 on eleven. Against surface
element count it is tight: **12.4 ± ~3 lines per element**, over a 4.75×
range of rung sizes.

That constant is the thing to reason about. Each surface element costs about
a dozen lines because each one is a decision: *what is this property called,
what does the delegate bind to, what arguments does this action take from the
view, what does the view need told when it completes.* A generator cannot
invent those; it can only transcribe them from a description that a human
wrote — and a description precise enough to emit them is the same size as
the code.

---

## 3. Mechanical vs per-model: the hand count

Two files classified line by line. Ranges are given so this can be re-derived.

### `examples/pastebin/gui_lib/paste_qml_bridges.cpp` (the issue's own reference)

| per-model | lines | | mechanical | lines |
|---|---:|---|---|---:|
| `isoOrEmpty` (L17–19) | 3 | | `FormsBridge` ctor (L71–72) | 2 |
| `readsText` (L29–31) | 3 | | `schemasJson()` (L74–76) | 3 |
| `idText` (L34–36) | 3 | | `submitIfValid()` (L78–91) | 14 |
| `toVariantMap(PasteView)` (L39–53) | 15 | | `PasteBridge` ctor frame (L93–94) | 2 |
| `toVariantMap(PasteSummary)` (L59–67) | 9 | | `connect` → `bound` (L97) | 1 |
| `connect` listed + row loop (L98–105) | 8 | | `connect` → `removed`/`failed` (L112–114) | 3 |
| `connect` → `loaded` (L106–107) | 2 | | | |
| `refresh` / `open` / `remove` (L116–126) | 9 | | | |
| **52 (68%)** | | | **25 (32%)** | |

Split by class rather than by file, the picture is sharper still:

- `FormsBridge`'s 19 `.cpp` lines are **100% mechanical** — this is what
  option A codified, and the issue's 60–70% figure describes exactly this.
- `PasteBridge`'s 58 lines are **90% per-model**.

### `examples/bank/gui/controllers/AccountController.cpp`

| per-model | lines | | mechanical | lines |
|---|---:|---|---|---:|
| `toMap` (L16–30) | 15 | | ctor (L34–35) | 2 |
| `refresh` aggregation body (L40–57) | 18 | | `refresh` frame (L37–39) | 3 |
| `openAccount` body (L63–66) | 4 | | `refresh` `.onError` (L58–60) | 3 |
| | | | `openAccount` frame + `.onError` (L62, L67–68) | 3 |
| **37 (77%)** | | | **11 (23%)** | |

The further a class is from the forms pattern, the higher its per-model
share. Since rungs 4–6 contain no forms classes at all, the tree is moving
*away* from the part that automates well.

---

## 4. The two moc constraints — verified, and one of them is wrong

Both were reproduced from scratch with a standalone CMake + AUTOMOC spike at
**Qt 6.11.1**, compiled, linked and run. Full matrix:

| variant | `indexOfSignal` | `indexOfMethod` | `invokeMethod` | verdict |
|---|---:|---:|---|---|
| **V1** whole-class macro using `Q_SIGNALS:` | 4 | 5 | OK | **works completely** |
| **V2** whole-class macro using lowercase `signals:` | **−1** | 4 | OK | signal silently absent |
| **V3** member macro whose header sits inside `#ifndef Q_MOC_RUN` | 4 | **−1** | **FAILS** | invokables silently absent |

### Constraint 2 — confirmed exactly as stated

A macro header included inside `#ifndef Q_MOC_RUN` is undefined under moc,
the invocation expands to nothing, and moc emits a metaobject with no
invokables. It compiles, it links, and `QMetaObject::invokeMethod` returns
`false` at runtime — QML's calls go nowhere with no diagnostic anywhere.

This is not a theoretical hazard in this codebase: **every ladder bridge
header already guards its morph includes this way**, because moc mis-parses
Lightweight's `DataMapper` headers (see the comment at
`examples/pastebin/gui_lib/paste_qml_bridges.hpp:9-22`, repeated near-verbatim
in `ledger_qml_bridge.hpp:9-14`, `board_qml_bridge.hpp:15` and
`project_admin_qml_bridge.hpp:12`). Anyone following local convention would
put a new macro header inside that guard. PR #155 hit exactly this.

### Constraint 1 — **real, but not what it was thought to be**

The stated constraint is "moc does not see a `signals:` access specifier that
arrives through a macro expansion". Isolated:

| case | spelling | via macro? | signal seen by moc |
|---|---|---|---|
| A | `signals:` | no | **yes** |
| C | `signals:` | yes | **no** |
| D | `Q_SIGNALS:` | yes | **yes** |

So the trigger is not macro expansion of a signals specifier in general — it
is specifically the **lowercase `signals` spelling** passing through a macro.
`Q_SIGNALS:` survives expansion intact, and **a macro that emits the entire
class — `Q_OBJECT`, `Q_PROPERTY`, `Q_INVOKABLE` and `Q_SIGNALS:` together —
works end to end**: V1 above registers the signal as a real signal
(`methodType() == Signal`), registers the property, dispatches through
`invokeMethod`, and delivers the emission to a connected lambda.

The relevant machinery is in `qtmetamacros.h`: under `Q_MOC_RUN` both
`signals` and `Q_SIGNALS` are defined **self-referentially**
(`#define signals signals`, lines 182–186), while outside it `signals` is
defined as `Q_SIGNALS` (line 44). The two spellings are therefore not
interchangeable inside a macro body as far as moc's parser is concerned.
I did not pin down which part of moc's lexer/parser makes the distinction;
the behaviour is reproducible and stable, but the *mechanism* is
uncharacterised, so treat the workaround as empirical rather than
guaranteed.

The failure is silent in the way that matters: V2 compiles and links fine as
long as nothing emits the missing signal. If something does emit it, it is a
link error (`Undefined symbols: V2::replied(QString const&)`) — which is at
least loud. The dangerous shape is a signal that is only *connected* to and
emitted from elsewhere, where nothing ever fails.

**Every existing bridge in the tree uses lowercase `signals:`** (33 of 33
classes). So this hazard is live for any future macro or generator, and the
mitigation — "emit `Q_SIGNALS:`, never `signals:`" — is a one-token rule
that no compiler enforces.

### Does this change the analysis?

It removes a *technical* objection to generative approaches: PR #155
concluded a whole-class macro was impossible, and that is not true. It does
**not** change the recommendation, because the recommendation does not turn
on whether generation is possible — it turns on how little there is worth
generating (§2, §3) and on what generation cannot see (§5). But #86 and the
`MORPH_QML_FORMS_BRIDGE_MEMBERS` doc comment both record the stronger claim,
and both should be corrected if this file is acted on.

### Qt has not moved on the load-bearing constraint

Re-checked at 6.11.1, five minor versions past the 6.5–6.8 range the issue
examined:

- `Q_OBJECT` in a class template: still a hard error —
  `error: Template classes not supported by Q_OBJECT`. Unchanged.
- `Q_GADGET`: the issue says it "doesn't help — no signals/invokables".
  Half right. A `Q_GADGET` **does** carry `Q_INVOKABLE` methods and
  `Q_PROPERTY` (verified: moc emits `// Method 'twice'`). What it lacks is
  signals — so it still cannot be a bridge, but the stated reason was wrong.
- `QQmlPropertyMap` behaviour is unchanged and is measured in §5.

Nothing in 6.9–6.11 changes the picture. Since CI pins 6.8.1, note that all
spike results above are from 6.11.1 and were **not** re-run on 6.8 — see
§9.

---

## 5. Option B — `QQmlPropertyMap`-backed generic projection

Spiked directly against `bankgui::AccountController`, the class the issue
names as the case B must prove itself on. The spike builds two projections of
the same `bank::dto::AccountInfo` — one generic (every reflected field, raw)
and one reproducing the shipped `AccountController::toMap` — and diffs them.

```
DTO fields:                 9
generic projection keys:    9  -> balanceMinor, currency, id, interestBps, kind,
                                  number, overdraftMinor, owner, status
shipped projection keys:    9  -> balanceText, closed, hasOverdraft, id, kind,
                                  number, overdraftText, statusKind, statusText

keys QML binds that reflection CANNOT produce (6):
    balanceText, closed, hasOverdraft, overdraftText, statusKind, statusText
keys reflection exposes that the UI never asked for (6):
    balanceMinor, currency, interestBps, overdraftMinor, owner, status
same key name, DIFFERENT value (silent semantic drift): kind, number
    e.g. number: generic=1234567890123456   shipped=•••• 3456
```

### What B cannot cover

**Six of the nine keys `AccountsPage.qml` binds cannot come from
reflection.** They are derived, not projected:

- `balanceText` combines **two** DTO fields (`balanceMinor` + `currency`)
  through `fmt::money`.
- `statusText`, `statusKind`, `closed` are three different renderings of one
  `int status` — one for display, one as a *styling token* the `Pill`
  component consumes, one as a boolean.
- `hasOverdraft` is a predicate; `overdraftText` is a literal-prefixed
  formatting of a second field.

Beyond the row projection, the controller computes **cross-row aggregates**
that no per-row reflection reaches: `totalBalance` sums only *open* accounts,
checks all of them share a currency, and falls back to a plain count when
they do not (`AccountController.cpp:40-57`). `LedgerQmlBridge::storeTransaction`
mints a fresh `QUuid` idempotency key per user gesture
(`ledger_qml_bridge.cpp:176`). `kanban` and `ledger` bridges own polling
lifecycles. None of that is a projection of anything.

### What B would actively break

Two of the nine keys **collide by name with a different meaning**. The worst
is `number`: the shipped projection is `fmt::last4()`, which masks all but
the last four digits of the account number. A reflection-driven projection
publishes the **full 16-digit account number** under the same key, and
`AccountsPage.qml`'s `Text { text: modelData.number }` renders it. The DTO's
shape is unchanged, so no shape-based drift guard would catch it. `owner`
would likewise be newly exposed.

This is not a bank-specific accident. It is what "project the DTO
generically" means: the projection function is currently where the decision
about *what the client is allowed to see* is made, and B moves that decision
to "whatever the DTO happens to contain".

It also collides with `examples/TESTING.md` presenter rule 6 — **"QML is
bindings-only; every conditional, format, and validation lives in the
presenter"** — and with `IMPLEMENTATION.md:66`. Handing QML raw ints and
minor units means the formatting has to happen *somewhere*, and the only
remaining somewhere is QML.

### What B costs — much less than the issue assumes, because it is already spent

The issue's stated cost for B is "losing compile-time-checked `Q_PROPERTY`
bindings and static QML tooling support (`qmllint` etc.)". **Measured, that
is not currently being obtained.**

Only `examples/forms/gui_qml/FormsController.hpp` uses `QML_ELEMENT`. Every
ladder rung and `bank` inject their bridges as untyped objects — `bank` via
`setContextProperty` (`bank/gui/main.cpp:77-83`), the rungs via
`setInitialProperties` into `property var` declarations
(`pastebin/gui/qml/Main.qml:32-33`). Running the shipped `qmllint` 6.11.1
over `bank/gui/qml/AccountsPage.qml`:

```
Warning: AccountsPage.qml:20:23: Unqualified access [unqualified]
                text: accounts.totalBalance
                      ^^^^^^^^
Warning: AccountsPage.qml:67:24: Unqualified access [unqualified]
                model: accounts.accounts
                       ^^^^^^^^
Info: Did you mean "count"?
```

`qmllint` has no type information for `accounts` at all — it is guessing at
an unrelated identifier. And the row data is *already* dynamic: the delegate
reads `modelData.kind`, `modelData.statusKind`, `modelData.balanceText` off a
`QVariantMap` inside a `QVariantList`, which is untyped lookup with no
compiled binding today.

So B's headline cost is largely illusory. But so is its benefit: the only
thing it would replace is the top-level container properties — **39 of them
across the whole tree** (23 ladder + 16 bank) — while leaving all 83
invokables, all 80 signals, and every projection function hand-written. The
spike confirms `QQmlPropertyMap` gives per-key `NOTIFY` (`hasNotifySignal=1`,
so bindings do update) but types everything as bare `QVariant`, and covers
zero invokables.

**Verdict on B: it addresses 39 property declarations, cannot produce 6 of 9
keys the UI needs, would silently unmask a PII field, and pushes formatting
into a layer the project's own rules forbid it in.** The cost side is
overstated in the issue and the benefit side is smaller still.

Note also that `docs/todo.md`'s "Considered and refused" table already
carries this judgment: *"Result-type presentation metadata (money, enum
labels, badge severity) — every GUI controller hand-writes a `toMap()`
projection. Genuine duplication, but it is a forms-layer concern."*

---

## 6. Option C — build-time codegen from glaze reflection

C is technically feasible — more so than PR #155 concluded, since §4 shows
generated code can emit a complete class including signals (and generated
`.hpp`/`.cpp` files need no macros at all, so **neither** moc constraint
applies to C). The objection is not feasibility. It is that the declarative
input would have to be as large as the output, and would have to be a
programming language.

Working from §3 and §5, here is what a generator would have to be told to
reproduce the six existing bridge sets:

| per-model content | can it be reflected? | what the description would need |
|---|---|---|
| field names + types | **yes** — `glz::reflect<T>` already does this | nothing |
| which fields are exposed at all | no | an allowlist (`owner`, `interestBps` are omitted) |
| derived keys (`closed`, `statusKind`, `hasOverdraft`) | no | an expression per key |
| multi-field formatting (`balanceText` = money(minor, currency)) | no | an expression per key |
| masking (`number` → `last4`) | no | an expression per key |
| enum ↔ display token, **both directions** | no | a table per enum, per direction |
| cross-row aggregates (`totalBalance`, `openCount`) | no | a fold with a filter and a fallback |
| invokable names + argument shapes | no | a signature per invokable (83 of them) |
| the domain call each invokable makes | no | a body per invokable |
| idempotency-key minting (`QUuid` per gesture) | no | a body |
| polling lifecycle (`kanban`, `ledger`) | no | a body |
| signal vocabulary | no | a signature per signal (157 of them) |

One row is reflectable. Everything else has to be written down. And the
rows marked "a body" are not data at all —
`AccountController::refresh`'s aggregation (sum open accounts, verify shared
currency, fall back to a count) is control flow. A description language
expressive enough to emit it is a programming language, which is the classic
codegen failure mode.

The honest tractable subset of C is: **generate the header declarations,
hand-write the `.cpp`.** But headers are only 512 of the 1919 code lines in
the ladder bridge files (27%), and they are the cheapest part to write —
they are declarations. That subset buys back roughly a quarter of the least
expensive quarter, in exchange for a generator, a CMake custom-command step,
a new input format, and a drift guard.

**Verdict on C: the only part of a bridge that reflection can supply is the
one part that was never expensive.**

---

## 7. What the measurements *do* point at

The duplication that is real, mechanical and model-independent — and that
neither B nor C targets:

### 7.1 `exception_ptr` → `QString`, duplicated 18 times

```cpp
try {
    std::rethrow_exception(err);
} catch (const std::exception& ex) {
    emit failed(QString::fromStdString(ex.what()));
}
```

Appears in 18 files: every rung's presenter and bridge, `bank`'s
`BankController::errorText`, and `examples/forms`'s `FormsController`. `bank`
already lifted it to a static member; nobody else did. ~5 lines × 18.

### 7.2 Strong-id ↔ QML scalar, duplicated 9 times

```cpp
template <typename IdT>
[[nodiscard]] IdT idFromText(const QString& text) {
    bool ok = false;
    const auto value = text.toLongLong(&ok);
    return ok ? IdT{value} : IdT{};
}
```

Byte-for-byte identical in `ledger`'s `budget_qml_bridge.cpp:29`,
`rule_qml_bridge.cpp:17`, `report_qml_bridge.cpp:19` and
`ledger_qml_bridge.cpp:32` — four copies **inside one rung**. `idNumber` is
duplicated across `kanban`, `ledger`, `bookmarks` and `polls`, with four
source comments explicitly saying *"same convention as
`kanban::gui::idNumber`"*. Three copies are already `template <typename IdT>`
— they were made generic within a file and never lifted out of it.

Together: ~130 lines across 18 files, removable by one header of free
function templates in `morph::qt` or `examples/common/gui`. No `Q_OBJECT`
involvement, so **neither moc constraint applies**, and nothing can fail
silently.

For scale: this is comparable to what PR #155 achieved (net −142/+144 for one
rung) at a fraction of the risk, and it applies to all six rungs plus bank at
once.

---

## 8. The drift guard

Whatever is decided, the drift-guard question has a good answer already in
the tree, and it is *not* `tests/test_forms_conformance_corpus.cpp`.

That corpus (227 lines) guards **schema emission** — it builds fixture action
types and asserts `morph::forms::schemaJson<A>()` still emits the documented
keys. It is the right prior art for "generated JSON must not drift from the
DTO", and it would be the right model for a generator's *schema* output.

But the failure mode that actually bites bridges is different, and
`bookmarks` already guards against it directly
(`examples/bookmarks/tests/test_bookmark_qml_bridges.cpp:278-380`):

```cpp
REQUIRE(meta->indexOfProperty("schemasJson") >= 0);
REQUIRE(meta->indexOfMethod("submitIfValid(QString,QString)") >= 0);
REQUIRE(meta->indexOfSignal("replyReceived(QString,bool,QString)") >= 0);
REQUIRE(meta->indexOfMethod("bulkArchive(QVariantList,bool)") >= 0);
```

This asserts the **metaobject surface** — exactly the thing that both moc
constraints destroy silently, and exactly what caught PR #155's `Q_MOC_RUN`
bug (`-1 >= 0`). It is the only mechanism in the codebase that can catch
either hazard.

A drift guard for a generative approach would need three layers, and only
the first exists today:

1. **Metaobject surface** — every declared property/invokable/signal is
   present with the exact signature. Generalise `bookmarks`' assertions into
   a shared helper taking a list of expected signatures. *Cheap, useful now,
   independent of B/C.*
2. **Projection key set** — the `QVariantMap` a bridge publishes contains
   exactly the documented keys. Nothing checks this today; a typo in a key
   name is a silent blank in the UI.
3. **Projection semantics** — the hard one, and the one §5 shows is
   load-bearing. A guard that only compares *shapes* would have passed the
   generic-vs-shipped `number` divergence, because both are a `QString` under
   the key `number`. Only a golden-value test per DTO per projection catches
   it. That is a test per model per screen — which is, again, O(QML surface).

Layer 3 being irreducible is itself an argument: a generator whose output
needs a hand-written golden test per surface element has not removed the
per-element work, only moved it from `.cpp` to test fixtures.

---

## 9. What would have to be true for this to be wrong

Stated as falsifiable claims, roughly in order of how much they would change
the conclusion.

1. **If the QML surface were derivable rather than designed.** The whole
   argument rests on 12.4 lines/element with the element count being a UI
   decision. If a convention were adopted — "every model gets exactly
   `refresh()`, `open(id)`, `remove(id)`, `listed`, `loaded`, `failed`" —
   then the surface *would* be derivable from the model, the 12.4 constant
   would apply to generated lines, and C becomes attractive. Note this is a
   *product* decision (uniform UIs), not a technical one, and the six
   existing rungs went the other way deliberately.
2. **If `lims` and rungs 7+ ship large GUIs.** The series 2, 4, 1, 2, 4, 0
   is what makes "unbounded growth" unpersuasive. If `lims` lands with 6
   bridges and rung 7 with 6 more, the absolute volume changes even though
   the growth law does not. Re-run §2's table at rung 8.
3. **If the ladder adopted `QML_ELEMENT` and typed bindings.** §5's "the cost
   of B is already spent" depends on every bridge being injected untyped.
   If the rungs moved to `qmlRegisterType`/`QML_ELEMENT` and started getting
   real `qmllint` coverage, B's cost would become genuine — which argues
   *harder* against B, but would also make C's generated headers more
   valuable, since generated code could carry the type annotations.
4. **If a second projection consumer appeared.** Today each `toMap` has
   exactly one consumer (one screen). If the same DTO had to be projected
   for a web client, a CLI table and a QML delegate, the projection would
   become a genuine cross-cutting artifact and reflection-driven defaults
   would earn their keep.
5. **If my mechanical/per-model classification is wrong.** It is a hand
   count of two files (§3), and the boundary between "mechanical `.onError`
   frame" and "per-model error handling" is a judgment call. I put the
   `try`/`rethrow`/`catch` frame on the mechanical side and the `emit`
   target on the per-model side. Classifying more aggressively toward
   mechanical would move `paste_qml_bridges.cpp` from 32% mechanical to
   maybe 45% — still not the 60–70% the issue claims, but the gap narrows.
   I did not hand-classify the other 14 bridge files; §2's LOC/element
   constant is the more robust of the two measurements.
6. **If the moc workaround is version-fragile.** §4's `Q_SIGNALS:`-through-a-
   macro result is empirical, at one Qt version, with the mechanism
   uncharacterised. If it breaks at 6.8 or in some future 6.x, whole-class
   generation via macro is back off the table — though generated *files*
   (option C) are unaffected either way.

---

## 10. What I could not determine

- **Whether §4's results hold at Qt 6.8.1**, which is what CI pins. Only
  6.11.1 was available locally. The `Q_OBJECT`-on-template error and the
  `Q_MOC_RUN` behaviour are longstanding and near-certainly identical; the
  `signals:` vs `Q_SIGNALS:` asymmetry is the one I would want re-run on 6.8
  before anyone relies on it, since I could not identify the moc code path
  responsible and therefore cannot reason about when it changed.
- **Why moc distinguishes the two spellings.** Both are self-referentially
  defined under `Q_MOC_RUN` in `qtmetamacros.h`, and moc's own `-E` output
  shows the `signals:` token surviving preprocessing intact — so the
  divergence is in the parser, not the preprocessor. I did not read moc's
  source.
- **Whether `lims` is intended to get a GUI.** Its README's task list does
  not mention one, but the branch is active in another worktree and may
  simply not be there yet. §1.1's "rung 6 contributes zero" could be a
  snapshot artifact. This is the single measurement most likely to be stale.
- **Whether the `polls` rung's 20.0 LOC/element outlier means anything.** It
  is the only rung meaningfully off the ~11 line, and I did not investigate;
  it may be the folded-in forms surface, or the event-polling machinery.
- **The true `bank` mechanical share across all six controllers.** I hand-
  counted `AccountController` only. `CardController` and
  `TransactionController` build their maps inline inside `.then` lambdas
  rather than in named helpers, which defeated automated classification, and
  I did not hand-count them.

---

## 11. Reproducing the measurements

Spikes live under `.spike/` in this worktree (untracked; they are throwaway).

```sh
# moc constraint matrix (V1/V2/V3) -- builds, links, runs
cd .spike/matrix && cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  && cmake --build build && ./build/matrix

# option B spike against bank::dto::AccountInfo
cd .spike/optionb && cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  && cmake --build build && QT_QPA_PLATFORM=offscreen ./build/optionb

# class/surface inventory (§1) and LOC/element table (§2)
python3 .spike/surface2.py

# qmllint, showing bridges are untyped today (§5)
/opt/homebrew/opt/qt/bin/qmllint examples/bank/gui/qml/AccountsPage.qml

# forms-pattern adoption per rung (§1)
for r in pastebin bookmarks polls kanban ledger bank; do
  echo -n "$r: "; grep -rl "FormsControllerCore\|schemasJson" examples/$r | wc -l
done
```
