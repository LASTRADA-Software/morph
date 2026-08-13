---
id: 025
title: MORPH_CLIENT_ONLY removes a client's link dependency on its models, but nothing removes the header dependency — a browser client still has to #include the ORM
subsystem: core
severity: minor
source: rung 1 (pastebin) task 13 — the WASM client
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/61
---

`MORPH_CLIENT_ONLY` exists for exactly one scenario, and
`docs/spec/core/registry.md` names it outright:

> even a build that never constructs a model locally still forces the linker to
> resolve the model's constructor and `execute()` bodies, pulling in whatever
> those depend on (a database driver, a native UI framework, an OS-specific
> API) — dependencies a client target may have no link path for at all (**a
> browser/WASM build in particular**), and will never call regardless.

That is the *link* half, and it works: the spec's own empirical note
(`tests/compile_checks/client_only_no_model_link.cpp`) confirms a model whose
constructor and `execute()` are **declared but never defined** links fine
under the macro.

The residue is the word *declared*. A client's whole dispatch surface is
`BridgeHandler<Model>` — a template over the model type — so the client must
still see `Model`'s complete definition, hence its header, hence everything
that header includes. For any ladder rung that follows
`examples/IMPLEMENTATION.md` rule 4 (all of them: persistence is
`Lightweight::DataMapper` behind a `WithMapper` mixin base), that is the ORM
and, transitively, ODBC:

```
paste_presenter.hpp
  └── pastebin/models/paste_model.hpp        // class PasteModel : private db::WithMapper
        └── pastebin/db/db_model.hpp
              └── <Lightweight/DataMapper/DataMapper.hpp>   // ODBC, absent in a browser
```

So `MORPH_CLIENT_ONLY` gets the client to the link step and the include graph
never lets it get there: rung 1's WASM client cannot compile a single
translation unit of shared presenter code without an ODBC-capable include path,
even though it will never open a database.

## What should happen

A pure client should be able to name a model's *action set* — the thing it
actually needs, since `ActionTraits<A>` already carries the type-ids and JSON
codecs — without the model's implementation surface. Some seam that makes
`BridgeHandler` parameterisable on a declaration-only facade, or a documented
"client-side model declaration" macro pairing with `MORPH_CLIENT_ONLY`, would
close it. Grepping `include/` finds nothing of the sort today: every
`BridgeHandler` instantiation in the repository is over a complete model type.

## What happens instead

Each rung works around it in its own persistence layer. Rung 1's answer
(`examples/pastebin/include/pastebin/db/db_model.hpp`) is a two-branch
`WithMapper`: the real DataMapper-owning mixin natively, an empty base under
`__EMSCRIPTEN__`, with no `mapper()` at all in the browser branch so any
attempt to reach a database from a WASM build is a compile error rather than a
link error. It is small, it is confined to the file that owns the ODBC
dependency, and no model, DTO, presenter or QML file gets a WASM variant — but
it is still a per-rung `#ifdef` that the framework, not the app, should be
making unnecessary. Every future rung will need the same three lines for the
same reason.

## Note on severity

`minor`, deliberately: it is a real gap with a real cost, but the workaround is
tiny, local, and does not change any behaviour — unlike `020`/`021`, which
force an app to give up a design outright. It becomes worse if a rung's model
header ever needs something heavier than a mixin base (a `Field<>`-typed member
in the model itself, say), because there is no `#ifdef` shape that keeps such a
model's declaration honest in both worlds.
