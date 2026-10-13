# Preferences: implementation plan (part 8, slice b1, #893)

**Goal.** A user's choices at a screen's customization points are stored on the server, keyed by the person the
server derives from the verified session, and come back when the screen mounts. A client cannot name another
person's key. Part of #893.

**Specs.** Spec 6 (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`) §8 "Storage seam" and the §12
"Preferences" tests; spec 5 (`2026-10-07-ui-document-design.md`) §12b "Customization points" and §13. In-tree:
`include/morph/ui/interpreter.hpp` (`Screen::setPreference`, `InterpreterHost::resetPrefs` and
`preferenceChanged`), `include/morph/session/session.hpp` (`session::current()`), `docs/spec/core/registry.md`
(custom factories), `docs/spec/core/coroutines.md` (Task handlers).

## What planning found

1. **The interpreter half exists.** A document's `customize` points are parsed (all seven kinds, `pins` and
   `arrange` only in the app document), a screen holds one choice per point with the declared default,
   `setPreference` refuses a `choice` outside its options and a `value` of the wrong type, `customize` editors
   write the choice and call `preferenceChanged`, and `resetPrefs` restores the defaults and calls the host. What is
   missing is everything behind the host hooks: the store, the server model, and the glue that loads choices on
   mount.
2. **Spec 6 §8's store shape does not compile against the framework.** `async::Completion<void>` does not exist
   (`Completion<T>` stores an `std::optional<T>`), and a completion needs an owner executor to deliver on, which the
   sketch does not say who supplies. The seam becomes `Completion<bool>` for `set` (stored) and `remove` (an entry
   was there), and the store owns the executor its completions are delivered on. The spec is corrected.
3. **`arrange` cannot work where the spec puts it.** Spec 6 §8 says `arrange` orders "the declared children of a
   container node" so a user can rearrange "the widgets of an overview screen", and the parser accepts it only in
   the app document. The app document has no view (`Document::view` is null for `app`), so its `arrange` point's
   `of` names a node of no document; the parser checks `of` only for `columns`. The container belongs to the
   overview screen, so `arrange` is a screen-scope kind there. This is raised on #893, and not built here.
4. **Applying app-scope points needs the app root scope.** The app document is not mounted by anything yet: the
   app root scope (spec 6 §5) is the rest of slice (b). `pins` has nothing to apply to until then, and a settings
   screen cannot name an app point either (`checkElement` refuses a `customize` node naming a point of another
   document; spec 6 §19).

So the slice is split:

- **(b1) this PR**: the storage seam, the in-memory store, the server preferences model and its registration, and
  the client glue for a screen's points. `Part of #893`.
- **Left for (b)**: app-scope points (`pins`, the `scope: "app"` key, a settings screen naming app points) with
  the app root scope (§5) and settings (§19); `arrange` after the decision on #893; the offline copy of choices
  (spec 5 §12b, "kept locally and written on reconnect") with the backend switch (§9); and the fit rules of
  `columns`, `layout` and `saved`, whose stored shapes are their editors' (the interpreter lowers only `choice`
  and string `value` editors today, so no other shape is written yet).

## Design (b1)

### The seam (`ui/preferences.hpp`)

```cpp
struct PrefKey { std::string person, app, scope, point; };
class IPreferencesStore {
public:
    virtual async::Completion<std::optional<std::string>> get(PrefKey const&) = 0;   // JSON text
    virtual async::Completion<bool> set(PrefKey const&, std::string json) = 0;       // stored
    virtual async::Completion<bool> remove(PrefKey const&) = 0;                      // an entry was there
};
```

- `MemoryPreferencesStore(exec::IExecutor& base)`: a map on an `OwnerStrand` over @p base, the house pattern for
  state a component owns; each call is a task on the strand, and its completion is delivered there.
- A store is written only with values the model has already bounded and parsed, so an application's store needs no
  bound of its own.

### The server model

`PreferencesModel` (type id `morph.preferences`), with three Task handlers:

| Action | Body | Result |
|---|---|---|
| `load` | `{app, scope, points}` | `{values: {point: json}}`, absent entries omitted |
| `set` | `{app, scope, point, value}` | `{stored}` |
| `remove` | `{app, scope, point}` | `{removed}` |

- **The person is `session::current()->principal`**, which `RemoteServer` stamps from the authoriser's
  `authenticate` before dispatch, and clears when it authenticates nothing. No body has a person: a `person` key in
  a body is ignored by the lenient decoder, so a request naming another person's key reaches only the caller's.
- An empty principal stores nothing: `load` answers no values (every point at its default), `set` and `remove` are
  refused. Without a verified person there is no key that belongs to the caller alone.
- Bounds, before the store is called: `value` at most `maxValueBytes` (16 KiB by default) and parseable JSON within
  `kMaxJsonDepth`; `app`, `scope` and `point` non-empty and at most 256 bytes; at most 256 points per `load`.
- `registerPreferences(store, dispatcher, registry, options)` registers the model with a custom factory that injects
  the store, and its three actions. Nothing registers at static initialisation.

### The client glue (`ui/preference_sync.hpp`)

`PreferenceSync(Dispatcher&, std::string application)`: the part of an application's `InterpreterHost` that talks
to the model, over the interpreter's own `Dispatcher` (one private binding of `morph.preferences`).

- `load(screen, scope, done)`: one `load` call for every point of the screen's document; each stored value is
  parsed (as the point's type for `choice` and `value`) and given to `Screen::setPreference`. A value that does not
  parse, or that the point refuses, is ignored and counted; the screen keeps the default. `scope` gates the
  delivery, so a screen destroyed first is never touched.
- `changed(screen, point, value)` writes the value's JSON (`preferenceChanged`); `reset(screen, point)` removes the
  point, or every point of the screen for `all` (`resetPrefs`). Failures are reported to an optional callback.
- `Screen::document()` is added so the glue can read a mounted screen's points; it is the only change to
  `interpreter.hpp`, in the customization region.

## Tasks (one commit each)

1. This plan.
2. `ui/preferences.hpp`: the seam, `MemoryPreferencesStore`, `PreferencesModel`, `registerPreferences`; tests through
   a `RemoteServer` for the five spec 6 §12 "Preferences" cases on the server side.
3. `ui/preference_sync.hpp` and `Screen::document()`: tests that a mounted screen takes its stored choices, an
   absent entry is the default, an outdated entry is ignored, a change is written and a reset removes, end to end
   through `BridgeDispatcher` and a `RemoteServer`.
4. `docs/spec/ui/preferences.md` (new), `docs/spec/ui/interpreter.md`, spec 6 §8 (the seam's shape, `arrange`),
   spec 5 §12b, CHANGELOG, CMake FILE_SET.

## Verification

- Mutations, each restored after and each checked against a freshly linked binary: take the person from the body
  instead of the session; drop the size bound; apply a stored value without the point's fit check. Each must fail a
  test.
- GCC Debug `morph_tests` in full; clang-tidy on the changed lines; Doxygen with `WARN_AS_ERROR`; clang-format on
  every touched file.
