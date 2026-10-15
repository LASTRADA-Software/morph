# Preferences — design

A user's choices at a screen's customization points (spec 5 §12b) are stored on the server, keyed by the person
the server derives from the verified session, and come back when the screen mounts, on any device. This page
covers the storage seam, the framework's store, the server's preferences model and the client glue. It implements
spec 6 §8's storage seam (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`) and the customization half of
spec 5 §13.

| Header | What it holds |
|---|---|
| `include/morph/ui/preferences.hpp` | `PrefKey`, `IPreferencesStore`, `MemoryPreferencesStore`, the wire shapes, `PreferencesModel`, `registerPreferences` |
| `include/morph/ui/preference_sync.hpp` | The client: `PreferenceSync`, `PreferencesLoaded` |

The interpreter's half is in [`interpreter.md`](interpreter.md): a screen holds one choice per point, starting at
the declared default; `Screen::setPreference` refuses a choice that does not fit its point; a `customize` editor
writes the choice and calls `InterpreterHost::preferenceChanged`; `resetPrefs` restores the defaults and calls
`InterpreterHost::resetPrefs`.

## Contents

- [The storage seam](#the-storage-seam)
- [The preferences model](#the-preferences-model)
- [Who the person is](#who-the-person-is)
- [Bounds](#bounds)
- [The client](#the-client)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The storage seam

```cpp
struct PrefKey { std::string person, app, scope, point; };   // scope: a screen id, or "app"
class IPreferencesStore {
public:
    virtual async::Completion<std::optional<std::string>> get(PrefKey const&) = 0;   // JSON text, or nothing stored
    virtual async::Completion<bool> set(PrefKey const&, std::string json) = 0;       // stored; false is a refusal
    virtual async::Completion<bool> remove(PrefKey const&) = 0;                      // an entry was there
};
```

- Every call is asynchronous, so a store may sit on a database or a remote service. A store owns the executor its
  completions are delivered on; a completion needs a serial owner, and the store is the one component that knows
  where its answers come from.
- The model calls a store only with a key whose person the server derived and with a value it has already bounded
  and parsed. A store applies no bound and no key check of its own.
- An application with its own per-person settings table supplies its own store over it, keyed by the person the
  login resolves to, so choices follow the person across logins and workstations.

`MemoryPreferencesStore(base)` is the framework's store: a map kept in tasks on an `OwnerStrand` over `base`, with
no lock. Each call is one task, and its completion is delivered on the strand. Its entries last as long as it does.

## The preferences model

`PreferencesModel` is registered under `morph.preferences` (`kPreferencesModel`) by
`registerPreferences(store, dispatcher, registry, options)`, with a custom factory that gives every instance the
store. Nothing registers it at static initialisation; a server that offers preferences calls it once before it
serves. Its three actions are Task handlers that await the store:

| Action | Body | `ok` reply |
|---|---|---|
| `load` | `PrefLoad`: `{app, scope, points}` | `PrefValues`: `{values: {point: json}}`; a point with no entry is absent |
| `set` | `PrefSet`: `{app, scope, point, value}` | `PrefStored`: `{stored}` |
| `remove` | `PrefRemove`: `{app, scope, point}` | `PrefRemoved`: `{removed}` |

A value travels as its JSON text in a string, which is what the store keeps. The actions are not journalled
(`Loggable::No`): a customization choice is not application data, and its history has no use.

## Who the person is

The person is `session::current()->principal` read inside the action: `RemoteServer` overwrites the envelope's
principal with the authoriser's `authenticate` result before dispatch, and clears it when the authoriser verifies
nobody. No body has a person. A body that names one anyway has the key ignored by the action decoder, which drops
unknown keys, so a request naming another person's key reads and writes only its own caller's entries.

The authoriser sees the model and the action, never the body, so it cannot keep people apart; the model can, and
does. A deployment's authoriser admits `morph.preferences` and its three actions for every signed-in principal.

With no verified principal there is no key that belongs to the caller alone: `load` answers no values, so every
point is at its default, and `set` and `remove` are refused with `preferences are stored for a signed-in person`.
The store is not called.

## Bounds

Checked by the model before the store is called; a request outside them is an `err` reply.

| What | Bound |
|---|---|
| A value | `PreferencesOptions::maxValueBytes` bytes of JSON text, 16 KiB by default, and parseable JSON within `kMaxJsonDepth` |
| `app`, `scope`, `point` | 1 to 256 bytes (`kMaxPrefNameBytes`) |
| Points in one `load` | 256 (`kMaxPrefPoints`) |

The value bound is the model's, not the store's, so an application's store needs none and every store gets the
same one.

## The client

`PreferenceSync(dispatcher, application, onError)` is the part of an application's `InterpreterHost` that talks to
the model. It binds `morph.preferences` privately through the interpreter's own `Dispatcher` at its first call, so
it travels the same bridge, session and backend as the screen's models. All its calls are made on the runtime's
owner, where their replies arrive.

- **`load(screen, scope, done)`** after a mount: one `load` call for every point of `screen.document()`. Each
  stored value is parsed (as the point's type for `choice` and `value`, structurally for the other kinds) and given
  to `Screen::setPreference`. A value that does not parse, or that the point refuses, is ignored: the point keeps
  the document's default, so a new document version never breaks on an old choice. `done` gets the counts
  (`PreferencesLoaded{applied, ignored}`) or the call's failure. `scope` gates the delivery: a screen destroyed
  before the reply is never touched.
- **`changed(screen, point, value)`** from `preferenceChanged`: a `set` of the value's JSON.
- **`reset(screen, point)`** from `resetPrefs`: a `remove` of the point, or of every point of a screen the sync has
  loaded for `all`.
- A failed write is told to `onError` with its reason; the screen keeps the choice the user made.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Where the person comes from | The verified session, inside the model | The authoriser never sees the body; a key from the client is a key the client chose. |
| No verified person | Defaults, and writes refused | Every unauthenticated caller would share one key. |
| Store completions | `Completion<bool>` for writes, delivered on the store's executor | `Completion<void>` does not exist; the bool carries the one fact a caller can use. |
| Where the size bound lives | The model | One bound for every store, applied before the write. |
| The client's transport | The interpreter's `Dispatcher` | Preferences go wherever the screen's models go, with the same session. |
| One `load` per screen | All points in one call | A screen's choices arrive together, in one round trip. |

## Not yet

- **App-scope points.** `pins` and the `app` scope wait for the app root scope (spec 6 §5), which nothing mounts
  yet, and for a settings screen that can name an app point (spec 6 §19).
- **Offline.** Spec 5 §12b keeps choices locally while offline and writes them on reconnect; that is the backend
  switch's (spec 6 §9).
- **The fit of `columns`, `layout` and `saved`.** Their stored shapes are their editors', and the interpreter
  lowers only `choice` and string `value` editors, so `setPreference` accepts any value for them.
