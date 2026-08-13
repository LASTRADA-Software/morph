---
id: 021
title: FormsControllerCore hardcodes its own LocalBackend, cannot compose over an existing Bridge/executor
subsystem: forms
severity: major
source: rung 1 (pastebin) GUI design investigation
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/57
---

`morph::qt::forms::FormsControllerCore<Model>`
(`include/morph/qt/forms/forms_controller_core.hpp:32-90`) is the shipped,
schema-driven QML forms controller `examples/IMPLEMENTATION.md` rule 2
mandates every rung's GUI render through. Its private members:

```cpp
morph::exec::ThreadPoolExecutor _pool{2};
::morph::qt::QtExecutor _gui;
morph::bridge::Bridge _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)};
morph::bridge::BridgeHandler<Model> _handler{_bridge, &_gui};
```

It owns and constructs its own `Bridge` over a hardcoded `LocalBackend`,
built from its own private pool and executor. There is no constructor
overload taking an existing `Bridge&`/`IExecutor*`, and no way to point it
at `Remote` mode.

This directly conflicts with `examples/TESTING.md`'s "Presenter
architecture" rule 2 binding requirement: presenters "take `(Bridge&,
IExecutor*)` and **never construct executors or backends themselves**" —
the whole point of `examples/common/gui::AppContext` is to be the *one*
place a rung's deployment mode (`Local`/`Remote`) is decided, with every
other piece of GUI code composing over the `Bridge&`/`IExecutor*` it
hands out. `FormsControllerCore` cannot do this: any rung using it as
shipped is silently pinned to an independent, always-local backend,
invisible to `AppContext`'s mode selection and untestable in `Socket`
mode via `BackendRig`'s matrix.

**What happens instead:** rung 1 (pastebin) does not use
`FormsControllerCore` as shipped. Its GUI still renders from
`morph::forms::schemaJson<A>()` through the real `MorphForms` QML module
(the schema-driven-first rule is honored in full) — only the *backend
wiring* is rung-owned: a thin controller exposing the same
`schemaJson()`/`submitIfValid()`/`fetchOptions()` surface, constructed
over the `BridgeHandler<PasteModel>` `AppContext::onReady()` already
hands it, instead of `FormsControllerCore`'s own hardcoded one. This is
"pure glue with no domain logic" under `IMPLEMENTATION.md` rule 2's
justification (b) for a rung-owned GUI piece, not a hand-rolled input
widget — the schema/validation/rendering machinery itself is untouched.

The framework-level fix `FormsControllerCore` needs: a constructor (or
factory) overload taking `Bridge&`/`IExecutor*` (or a pre-built
`BridgeHandler<Model>`) instead of building its own, so a QML-consuming
app can compose it the same way every other presenter in this codebase
already does.
