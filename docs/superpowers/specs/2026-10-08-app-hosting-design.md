# Hosting an application: lifecycle, design package, workspace shell — design (spec 6)

An application with its own visual identity and its own native pieces — a sign-in against an
existing user table, a licence check, window chrome, a corporate design kit — runs on the generic
client of the program without writing a screen, a view-model or a QML layout of its own. What the
application keeps is small and is all *design* or *integration*: a theme, the few components a theme
cannot express, a `main`, and the seams that connect its own sign-in and preferences storage.
Everything else is served by its server as UI documents (spec 5).

This spec defines what an application and the client agree on beyond the document itself: the
lifecycle of a mounted screen, the contract between a host and a renderer, the app root scope, the
design package, the workspace shell, application-scope preferences, offline as a backend switch, and
the error and schema vocabulary a real application needs. Its worked example is a sample-entry
screen of the lims example (§11).

## Contents

[1 Intent](#1-intent) · [2 Roles](#2-roles) · [3 Screen lifecycle](#3-screen-lifecycle) ·
[4 Host contract](#4-host-contract) · [5 App root scope and session](#5-app-root-scope-and-session) ·
[6 Design package](#6-design-package) · [7 Workspace shell](#7-the-workspace-shell) ·
[8 Preferences and catalog](#8-preferences-and-catalog) · [9 Offline as a backend switch](#9-offline-as-a-backend-switch) ·
[10 Forms and errors](#10-forms-errors-and-calculation) · [11 Worked example](#11-worked-example-sample-entry) ·
[12 Tests](#12-tests) · [13 Risks](#13-risks) · [14 Host vocabulary](#14-host-vocabulary-host1) ·
[15 Native panes and the close guard](#15-native-panes-tabs-and-the-close-guard) ·
[16 Call and return](#16-call-and-return) · [17 Keyboard and focus](#17-keyboard-and-focus) ·
[18 Files and long jobs](#18-files-and-long-jobs) · [19 Settings and messages](#19-settings-and-messages)

## 1. Intent

**Goals:**

- **Minimal application GUI code.** What an application writes in a GUI toolkit is limited to what
  defines its look: tokens and the components tokens cannot express.
- **Reusable by construction.** A component written for one application is reusable by any: it is
  named, declares its props and events, and has a fallback (spec 5 §10).
- **Native integration stays native, through narrow seams.** Sign-in, licensing, window chrome and
  preference storage are the application's, behind interfaces the framework defines.

**Decisions:**

- **Tokens before components, components before screens.** A look is first data (§6 tokens); what
  data cannot express is a named component; a screen is never hand-written.
- **A framework workspace shell, not an application shell.** Tabs, per-tab dirty state, a close
  guard, most-recently-used switching, pins and a start page are not specific to any application, so
  the framework provides them (§7). Window chrome, sign-in and licensing are not, and stay with the
  application.
- **The host embeds or the client owns, with one contract.** An application with a native shell
  mounts documents through the renderer's `ScreenHost` (§4); the generic client mounts the same
  documents through the workspace shell. A document does not know which.

## 2. Roles

| Role | Owns | Does not own |
|---|---|---|
| **Server** | Models, actions, screen documents, the catalog, custom components, theme tokens, the app root document | Anything about pixels on a particular client |
| **Framework client** | Interpreter, renderers, workspace shell, `ScreenHost`, theme application, host-component registry | Application names, models, colours |
| **Application executable** | `main`, a theme (if not served), host components, `AppSource` and `IPreferencesStore` implementations, window chrome | Screens, view-models, layouts |

An application whose models run in-process (local mode, spec 5 §2) plays the server role in the same
executable.

## 3. Screen lifecycle

A screen document gains these optional top-level keys (spec 5 §3):

```json
{ "id": "lims.sample", "params": { "sampleId": "int?" },
  "identity": { "ref": "sampleId" },
  "title": { "fn": "coalesce", "args": [ { "ref": "sampleNumber" }, { "t": "lims.sample.new", "fallback": "New sample" } ] },
  "dirty": { "ref": "unsaved" },
  "onMount":  [ { "if": [ { "fn": "engaged", "args": [ { "ref": "sampleId" } ] }, [ { "run": "load" } ], [ { "run": "create" } ] ] } ],
  "onUnmount": [],
  "onClose": [],
  "onBackendChange": [ { "run": "load" } ] }
```

- **`title`** is an expression (a literal `{ "t": ... }` still works). The host shows it in the tab
  and window title and re-reads it when it changes.
- **`dirty`** is a bool expression. A `true` value shows the unsaved indicator and arms the close
  guard.
- **`identity`** is an expression. Two openings of the same screen id with equal `identity` are the
  same screen: opening the second focuses the first. A screen without `identity` is a singleton when
  the catalog entry says `singleton`, and one instance per opening otherwise. A catalog entry's
  `singleton` is the only place that flag lives.
- **`onMount`** runs once, after the screen's scope exists and before its queries start: the
  interpreter issues the commands of `onMount`'s batch, then lets the scope's queries run their first
  fetch. Commands that start a call are issued, not awaited, so a query that depends on a call made
  in `onMount` declares it: the call's mutation `invalidates` the query (spec 5 §5), or the query is
  gated with `when`. `watch` does not run on the first evaluation (spec 5 §6); `onMount` is how a
  screen starts an action on open.
- **`onUnmount`** runs before the scope is destroyed. Commands that need a reply are not awaited; a
  screen whose teardown must complete runs it in `onClose`.
- **`onClose`** runs when the host asks to close the screen. Without `onClose`, a screen whose
  `dirty` is `true` is guarded by the host's own confirmation, and one whose `dirty` is not `true`
  closes at once. With `onClose`, the document owns the decision: it opens its own confirm dialog and
  runs `{"cancelClose": true}` to keep the screen open. `cancelClose` is valid only inside `onClose`
  and is refused at load elsewhere.
  - An open dialog and a mutation in flight count as pending: the screen closes when `onClose`'s
    batch has ended and nothing it started is pending, and `cancelClose` anywhere in that time keeps
    it open.
  - The host's `done(closed)` is called exactly once per `requestClose`, always posted to the owner
    and never from inside the document's own handler. A second `requestClose` while one is pending
    returns the pending one.
- **`onSave`** is the command list that saves the screen. It is what the shell's guard runs for
  *save and close* (§15); a screen without it is not offered that choice.
- **`onBackendChange`** runs after the interpreter has rebound the screen to a new backend (§9).

**Private model instances.** A model alias's `instance` is `"private"`, `"shared"` or an expression
(spec 5 §5). A private alias is bound to an instance of its own: the model's state, such as an open
record, is the scope's alone, and the binding is released when the scope unmounts. The binding is a
`NoSharing` handler owned by the scope.

## 4. Host contract

```cpp
namespace morph::ui {                                  // include/morph/ui/host.hpp
using ParamMap = std::map<std::string, Value>;         // Value: the document's typed value variant
struct MountError { std::string screenId; std::string reason; };

class ScreenHandle {
public:
    virtual reactive::Computed<std::string> const& title() const = 0;
    virtual reactive::Computed<bool> const& dirty() const = 0;
    virtual void requestClose(std::function<void(bool closed)> done) = 0;   // the close protocol of §3
    virtual void forceClose() = 0;       // for application quit: skips onClose, runs onUnmount, never vetoed
    virtual ~ScreenHandle() = default;
};
class ScreenHost {                                    // implemented by a host that owns its own shell
public:
    virtual std::expected<std::unique_ptr<ScreenHandle>, MountError>
        mount(std::string_view screenId, ParamMap params) = 0;
    virtual std::optional<std::string> identityOf(std::string_view screenId, ParamMap const& params) = 0;
    virtual void onNavigate(std::string_view screenId, ParamMap const& params) = 0;   // implemented by the host
    virtual ~ScreenHost() = default;
};
}
```

- `qt_quick::Renderer` (spec 3 §1) implements `ScreenHost` over a connected source and exposes each
  mounted screen's root `QQuickItem` beside its handle. The host places the item wherever its shell
  wants it. `onNavigate` is called by the renderer and implemented by the host.
- A document's `{"navigate": "screen", "params": e}` that targets another screen is delivered to
  `onNavigate`; the host decides whether that is a new tab, a focus or a dialog. The workspace shell
  (§7) is the framework's implementation of that decision.
- The host calls `requestClose` and waits for `done(closed)`. It never destroys a handle whose close
  is pending, except through `forceClose`.

## 5. App root scope and session

The bundle may carry one **app document** (id `app`) with `state`, `let`, `queries`, `mutations` and
`watch`, mounted once per connection and destroyed on sign-out. Screens read its names as
`{"ref": "app.name"}` and may write its `state` with `{"set": "app.name", ...}`.

Its use is what no single screen owns: lookup lists loaded once per session (analysis
methods, clients, units), the current tenant, a connection banner.

Two expression forms read the client and the session:

| Form | Meaning |
|---|---|
| `{"session": "displayName"}` | `principal`, `userName`, `displayName`, `userKey`, `tenant`, `authenticated` |
| `{"env": "online"}`, `{"env": "backend"}` | Whether the active backend is reachable, and its name (`primary`, `local`, `remote`) |

**Session attributes.** `session::Context` carries a principal, a token, a request id, a locale and
metadata; `Principal` carries an id, roles and a claims bag. `userName`, `displayName`, `userKey` and
`tenant` are entries of the principal's claims, and `authenticated` is true once the server has
accepted the token. They are what the client shows; the server never trusts a value the client sets.

**Establishing a session.** Two ways, both through `AppSource`:

- **Native.** The application's `AppSource::open` signs the user in with its own code before the
  catalog is requested, on the owner, and installs the principal and claims through the bridge's
  session API. A licence check runs there too; a refusal is a `ConnectError` that the frontend shows
  before any screen exists.
- **Document.** A login screen runs a mutation whose reply carries a session token, then
  `{"signIn": {"from": "login"}}`: the client builds a new session context from the current one with
  the token replaced, so the locale, request id and metadata are kept, installs it, reloads the
  catalog and mounts the start screen. The login mutation is never queued offline, and its token
  field is `secret` (spec 5 §5).
- **Sign-out** unmounts every screen and the app document first, so their private instances are
  deregistered under the session that owns them, and then clears the session. A person who signs in
  as a different user on the same connection therefore cannot be left with instances the new session
  cannot release. `{"signOut": true}` shows the login screen.

`userKey` is a claim the application's sign-in sets for its own use. The preferences model does not
trust it (§8).

## 6. Design package

The look of an application is data first.

**Theme tokens** (vocabulary `ui-theme/1`) are part of the bundle, or of the client when the
application ships its own:

```json
"theme": { "vocab": "ui-theme/1",
  "light": { "color.surface": "#FFFFFF", "color.surfaceAlt": "#F1F5F9", "color.text": "#0F172A",
             "color.textMuted": "#64748B", "color.accent": "#2563EB", "color.accentText": "#FFFFFF",
             "color.ok": "#15803D", "color.warn": "#B45309", "color.err": "#B91C1C",
             "color.border": "#CBD5E1", "color.focus": "#2563EB" },
  "dark":  { "color.surface": "#0B1220" },
  "scale": { "space": [0, 4, 8, 12, 16, 24, 32], "radius": [0, 4, 8, 12], "font.size": [11, 12, 13, 15, 18, 24],
             "font.family": "Inter", "control.height": 32 },
  "density": { "comfortable": { "control.height": 32 }, "compact": { "control.height": 26 } } }
```

- The names are the closed set `ui-theme/1` defines; an unknown name is ignored, a missing one takes
  the `MorphUi` default. The `MorphUi` base components (spec 3 §3) read the tokens through one
  `Theme` singleton, so every generated screen and every host component sees the same values.
- `{"pref": "density"}` (spec 5 §12b) selects a `density` entry at runtime.
- The terminal renderer maps colour roles to its palette and ignores the scale.
- **Semantic sets.** Each of `ok`, `warn`, `err` and `info` has `color.<role>`, `color.<role>.bg`,
  `color.<role>.border` and `color.<role>.dot`; `elevation.1` to `elevation.3` are shadow tokens.
- **Surfaces.** A theme may declare `"surfaces": {"sidebar": {"color.surface": "#0F172A",
  "color.text": "#E2E8F0"}}`: a named token set that overrides the base for a subtree. A node with
  `"surface": "sidebar"` resolves every token under it, including a host component's, through that
  set. This is how a dark rail sits inside a light theme.
- **Extension tokens.** Names under `ext.` (`ext.rail.width`, `ext.row.dense`) are free-form, typed
  by their value, and readable by host components and restyled kinds; the built-in components ignore
  them.
- **Icons.** `"icons": {"alert": "asset:sha256:…"}` maps an icon name to an SVG in the bundle; a node's
  `icon` property names one. A small built-in set covers the names the built-in components use.

**Host components.** A component the client carries, named and versioned, registered by the
application executable:

```cpp
qt_quick::HostComponents::add({ .name = "ResultGrid", .version = 1,
    .url = QUrl("qrc:/kit/ResultGrid.qml"),
    .props  = { {"columns", "json"}, {"rows", "json"}, {"results", "json"} },
    .events = { {"edited", "json"} },
    .safeForUntrusted = true });
```

- A bundle declares one the same way as a delivered component (spec 5 §10) with `"host"` in place of
  `"qml"`:

  ```json
  "components": { "ResultGrid": { "host": "ResultGrid", "version": 1,
      "props": { "columns": "json", "rows": "json", "results": "json" }, "events": { "edited": "json" },
      "fallback": { "kind": "table", "...": "..." } } }
  ```

- The `ui-hello` request (spec 5 §12) lists the client's host components with their versions. A
  component the client lacks, or has at a lower version, renders its fallback, chosen when the screen
  mounts, and is reported once; the fallback's props must be type-compatible with the component's, so
  load-time validation covers both.
- A host component needs no hash and no verified TLS connection: it is part of the client binary.
  That makes its props and event payloads untrusted input from a document, which the component
  validates. A component with a side effect (it opens files, reaches the network, touches the host)
  sets `safeForUntrusted: false` and is mounted only from a verified connection (spec 5 §13).
- It sees its props and event sinks and, in addition to a delivered component, may import other host
  components and the `Theme` singleton.
- **Restyling a built-in kind** (spec 5 §10) maps to a host component with `"restyle": {"button":
  "BrandButton"}`. The mapping is checked when the server registers the bundle and when the client
  loads it, against the base kind's props and events.

**Accessibility and test identity.** Every node takes `"a11y": {"name": e, "role": "..."}` and
`"testId": "..."`. The Qt Quick generator emits `Accessible.name`, `Accessible.role` and
`objectName`; the terminal renderer uses the name as the widget's label.

- Roles are `group`, `heading`, `text`, `button`, `edit`, `checkbox`, `list`, `table`, `chart` and
  `status`. A node with a click handler has the role `button` and the press action unless the
  document gives it another.
- A field bound to a form takes its accessible name from the field's label. A read-only value is
  named `"<caption>: <value>"`, so a test reads it without a separate query.
- `testId` is a stable identifier for the node, and is how a test finds a node whose name is
  generated. The generator emits it as `objectName` and, on a Qt version that has the property, as
  `Accessible.id`. Qt's documentation does not say how either maps to the platform's automation id,
  so the framework does not assume it: an automated-UI conformance test on each platform that has
  one reads the properties a driver sees (name, role, automation id) from a generated screen in a
  real window, and the supported way to locate a node is the one that test passes. Lookup by
  accessible name and role is the primary route and works wherever the platform exposes them.

## 7. The workspace shell

The app shell vocabulary of spec 2 §8 (`app-menu`, `app-screens`) gains a second kind, **workspace**:
a set of tabs over screens, as most desktop applications of this kind need. Its keys use the same
`app-` prefix.

```json
{ "app-shell": "workspace",
  "app-start": "resume", "app-start-fallback": "overview", "app-max-pins": 12,
  "app-screens": [ "lims.overview", "lims.sample", "lims.history" ] }
```

The screens are catalog ids; whether a screen is a singleton, and its title, group and availability,
are the catalog entry's (§8).

- **Tabs.** A tab is a screen instance. Opening a screen whose `identity` equals an open tab's
  focuses that tab. A `singleton` screen has at most one tab. The tab shows the screen's `title`
  and, when its `dirty` is `true`, the unsaved indicator. A hidden tab keeps its scope and its state.
- **Close.** Closing runs the close protocol of §3 and the guard of §15. Close-others and close-all
  run it per tab, one tab at a time, and stop at the first veto.
- **Recent order.** Ctrl+Tab walks tabs most recently used first; the shell shows a switcher while
  the modifier is held.
- **Start.** `resume` reopens the screens recorded for the person, as `screenId` or
  `screenId:identity`; `overview` or any screen id opens that screen; `app-start-fallback` applies
  when nothing is recorded or a recorded screen is no longer available. The record is written as
  tabs open and close, debounced, and flushed before the session is cleared on sign-out.
- **Navigator.** The catalog (§8) is listed with its module and group; an entry that is
  `available: false` is listed disabled with its reason. A search over titles (in every locale, §19)
  and a keyboard shortcut open it; its keys are §17's.
- **Pins.** The user's shortcuts are an application-scope customization point of kind `pins` (§8),
  ordered, optionally grouped, limited to `app-max-pins`.
- **Window chrome** (title bar, window controls, user menu) is the host's: the shell is an item the
  host places in its own window. The generic client supplies a plain default.

The shell is a document-free framework component, in `include/morph/forms/engine/workspace.hpp`
beside `app_shell.hpp`: its state is reactive nodes (spec 1), its view is a view tree (spec 1 §5),
and it mounts screens through the same `ScreenHost` an embedding host uses.

## 8. Preferences and catalog

**Application-scope customization.** Spec 5 §12b's `customize` block may appear on the app document
as well as on a screen. Two point kinds are added:

| Kind | Holds |
|---|---|
| `pins` | An ordered list of catalog ids with optional group labels, limited by `app-max-pins` |
| `arrange` | An ordered list of ids of declared children of a container node, each enabled or hidden |

`arrange` lets a user reorder and hide the widgets of an overview screen; the node's children carry
the ids. It orders and hides what the document declares and never positions or sizes anything, so
free layout editing stays out of scope (spec 5 §15).

**Storage seam.**

```cpp
struct PrefKey { std::string person, app, scope /* screen id or "app" */, point; };
class IPreferencesStore {
public:
    virtual async::Completion<std::optional<std::string>> get(PrefKey const&) = 0;   // JSON text
    virtual async::Completion<void> set(PrefKey const&, std::string json) = 0;
    virtual async::Completion<void> remove(PrefKey const&) = 0;                      // reset to the defaults
    virtual ~IPreferencesStore() = default;
};
```

- `person` is derived on the server from the verified principal. The request carries no person: the
  authoriser sees only the model and the action, never the body, so the preferences model itself
  takes the key from the verified session, and a client that names another person's key reads and
  writes nothing of theirs. The framework's store therefore never accepts a client-supplied key.
- An application with its own storage supplies its own store, which maps the principal to the person
  on the server: an application with an existing per-person settings table keys it by the person the
  login resolves to. Choices then follow the person across logins, tenants and workstations.
- A stored value has a size bound, applied before it is written.
- An absent entry is the document's default, never an error. A stored entry that no longer fits its
  point is ignored (spec 5 §12b).

**Catalog entries** (spec 5 §12) carry what a navigator needs:

```json
{ "id": "lims.sample", "title": { "t": "lims.sample.title", "fallback": "Sample entry" },
  "module": "Samples", "group": "Testing", "icon": "flask",
  "singleton": false, "available": true, "reason": null, "whenDenied": "disable", "public": false,
  "host": null, "reuseEmpty": false }
```

- `available: false` lists the entry disabled, with `reason` shown. A catalog that lists what an
  application has not built yet lets a navigator's counts stay true.
- `whenDenied` says what a permission refusal does to the entry: `hide` (the default, spec 5 §12b)
  or `disable`, with the reason `denied`.
- `public: true` marks an entry the client may fetch before sign-in, such as a login screen
  (spec 5 §12).
- `host` names a screen the application registered with the client instead of a document (§15);
  `reuseEmpty` makes opening the entry replace an empty, unmodified tab of it (§15).

## 9. Offline as a backend switch

An application whose offline mode is a second database with its own models (a local database that
later synchronises) implements it as a backend switch, not as a queue of mutations (spec 5 §5
`"offline": "queue"`, which an application without a local database keeps using).

The bridge already switches backends under live handlers. The interpreter relies on that and adds the
document side:

- `AppSource::switchBackend(Backend)` posts to the bridge's owner and calls `Bridge::switchBackend`,
  with the offline reconnect coordinator's sequencing where the application has one. `Backend` is the
  application's opaque backend handle; its name (`primary`, `local`, `remote`) is what
  `{"env": "backend"}` reads.
- The bridge re-issues a bind for every live binding: a private instance comes back as a fresh,
  empty instance from the model factory, and a shared one re-attaches by its key. A switch is
  all-or-nothing: if it fails, the old backend stays, nothing is rebound, and the interpreter reports
  `backendSwitchFailed` to the `onError` path of the command that asked for it.
- A call in flight on the old backend is rejected with `BackendChangedError`, which has the error
  kind `backendChanged` (§10).
- Order in the interpreter: the switch commits; the queries on private aliases are held idle; each
  mounted screen's `onBackendChange` runs; then the held queries run their fetch and the others
  refetch, in one batch; `{"env": "backend"}` and `{"env": "online"}` publish with it. A query on a
  private alias therefore never fetches against the empty new instance.
- A private model instance does not survive a switch. A screen holding an open record reloads it in
  `onBackendChange`, from state it keeps, and decides what the user is told about edits the old
  instance held: `dirty` is whatever the screen sets, never silently carried over.
- The model's own `onBackendChanged` hook and the document's `onBackendChange` are two things: the
  first lets a model re-prepare its resources, the second lets a screen recover its view.
- What a switch implies for data — transferring an unsaved draft to the other database, reserving a
  key range for rows created offline, remembering which records to synchronise on reconnect — is the
  model's: it happens in the model's own actions and in the application's `AppSource`, so every
  client of the model behaves the same and no screen carries it.

## 10. Forms, errors and calculation

**A form's field overlay.** A `form` node takes `fields`, applied to the parsed form model before the
session is built (spec 2 §7):

```json
{ "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration",
  "fields": { "rowIndex": { "omit": true },
              "analystId": { "hidden": true },
              "mass": { "label": { "t": "lims.capture.mass", "fallback": "Mass" }, "unit": "g", "decimals": 1 },
              "receivedOn": { "widget": "epochDays" },
              "remarks": { "blankAs": "empty" } } }
```

`omit` removes the member from the form and its body; `hidden` keeps it in the body and out of the
view; `label`, `unit`, `decimals`, `widget` and `blankAs` replace the schema's. `blankAs` takes the
lower-case spelling of the schema's `x-blankAs` values (`omit`, `empty`). An application that needs
the same overlay for several screens declares it once on the model's schema emitter (the server's
`x-` keywords), and the document's overlay refines it.

**Integer-backed dates.** `x-widget: epochDays` and `epochSeconds` select the field kinds `EpochDate`
and `EpochDateTime` (spec 2 §2), whose wire value is an integer count of days or seconds since the
epoch (UTC) and whose display is the locale's date or date-time. `x-widget` is an advisory hint
today; these two values are new vocabulary items of `forms/1`, so a client that does not know them
shows the integer, and the handshake says which a client speaks.

**Error kinds.** `{"errorKind": "m"}`, for a query or mutation `m`, is one of `notFound`,
`validation`, `connectionLost`, `denied`, `conflict`, `backendChanged`, `other`; `{"error": "m"}`
stays the display text. The server's typed errors map onto the kinds through one table the
application registers; `BackendChangedError` maps to `backendChanged`. A reply enum such as a save's
validation outcome selects a message with the display functions:
`t(concat("lims.save.", result.error), "Not saved")`, with one catalog entry per enum value.

**Calculation stays on the server.** A value derived from several rows, such as a share of a total
with its limits and deviations, is a field of an action's reply (spec 5 §7). The
client does not recompute it, so the displayed value and the value the server judges against are one
and the same.

## 11. Worked example: sample entry

The reference is the lims example's sample-entry screen, `lims.sample` (spec 4 §5). A record-editing
dialog of the kind an application hand-writes as a view-model, a set of read-only data types, a
schema annotator and a layout per section is one document.

**Model additions.** The lims `SampleModel` already has `RegisterSample`, `OpenSample`, `GetSample`,
`ListResults`, `ListDilutionModes` and `CaptureConcentration`. The example adds what a
record-editing dialog needs and the lifecycle model lacks:

| Addition | Why |
|---|---|
| `NewSample`, `LoadSample`, `SaveSample`, `DeleteSample` over a working copy held by the model instance | The screen edits a private working copy and Save commits it (§3) |
| `HeaderSection`, `SamplingSection`, `TurbiditySection`, `NitrateSection` | One action per editable section, each with a `fields` overlay |
| `ClearOptionalMeasurement` | Removes an optional measurement's stored values |
| `GetHeader`, `GetMeasurements`, `GetDilutionSeries` | Snapshots of the working copy; `GetMeasurements` reports which optional measurements hold data |
| A dilution-series row with `measured`, `recoveryPct`, `limitMin`, `limitMax`, `outsideLimits` | Computed by the model, never by the client (§10) |
| `ListProjects` with `TableQuery` and `Page<Row>`, and `SelectProject` | The lookup table (spec 7 §2) and its choice |

**The screen**, authored in C++ with the builders of spec 5 §11. `page`, `pageHeader`, `card` and
`pill` are compositions of the palette's `column`, `row`, `panel`, `scroll` and `text`:

```cpp
auto limsSample = ui::screen<"lims.sample">([](ui::Scope& s) {
    auto sampleId = s.param<std::optional<std::int64_t>>("sampleId");
    auto sample   = s.model<SampleModel>("sample", {.instance = ui::Instance::Private});   // a working copy
    auto unsaved  = s.state("unsaved", false);
    auto optional = s.state("optional", ui::List<std::string>{});   // optional measurements shown
    auto project  = s.dialog("project");

    auto header  = s.query<GetHeader>(sample, {}, {.refreshOn = ui::actions<HeaderSection, SelectProject>()});
    auto series  = s.query<GetDilutionSeries>(sample, {}, {.refreshOn = ui::actions<SamplingSection>()});
    auto measure = s.query<GetMeasurements>(sample, {}, {.refreshOn = ui::actions<TurbiditySection, NitrateSection,
                                                                                  ClearOptionalMeasurement>()});

    s.identity(sampleId);                                           // one tab per sample
    s.title(ui::coalesce(header.at(&Header::sampleNumber), ui::t("lims.sample.new", "New sample")));
    s.dirty(unsaved);

    // load and create fill the working copy after the queries started, so they invalidate them
    auto load  = s.mutation<LoadSample>(sample, {.invalidates = {header, series, measure},
                                                 .onSuccess   = {ui::set(unsaved, false)}});
    auto make  = s.mutation<NewSample>(sample, {.invalidates = {header, series, measure},
                                                .onSuccess   = {ui::set(unsaved, false)}});
    auto clear = s.mutation<ClearOptionalMeasurement>(sample, {.invalidates = {measure, series},
                                                               .onSuccess  = {ui::set(unsaved, true)}});
    auto pick  = s.mutation<SelectProject>(sample, {.invalidates = {header},
                                                    .onSuccess   = {ui::set(unsaved, true), ui::close(project)}});
    auto save  = s.mutation<SaveSample>(sample, {.onSuccess = {ui::set(unsaved, false)},
                                                 .onError   = {ui::notify(ui::errorText(ui::self), ui::Role::Error)}});
    s.onMount(ui::if_(ui::engaged(sampleId), ui::run(load, {.id = sampleId}), ui::run(make)));
    s.onBackendChange(ui::seq(ui::if_(unsaved, ui::notify(ui::t("lims.lost", "Unsaved edits were lost"), ui::Role::Warn)),
                              ui::run(load, {.id = sampleId})));

    return ui::page(
        ui::pageHeader(ui::title(s.titleExpr()),
            ui::button({.label = ui::t("lims.project", "Project"), .onClick = ui::open(project)}),
            ui::button({.label = ui::t("lims.save", "Save"), .busy = ui::pending(save), .onClick = ui::run(save)})),
        ui::scroll(
            ui::card(ui::t("lims.header", "Header"),
                ui::form<HeaderSection>(sample, {.prefill = header.value(), .onSuccess = ui::set(unsaved, true)})),
            ui::card(ui::t("lims.measurements", "Measurements"),
                ui::column(
                    ui::form<SamplingSection>(sample, {.onSuccess = ui::set(unsaved, true)}),
                    ui::form<TurbiditySection>(sample, {.visible = ui::includes(optional, "turbidity")}),
                    ui::form<NitrateSection>(sample, {.visible = ui::includes(optional, "nitrate")}),
                    ui::optionalMenu(optional, {"turbidity", "nitrate"}, {.onRemove = ui::run(clear)}))),
            ui::card(ui::t("lims.series", "Dilution series"),
                ui::column(
                    ui::custom<"SeriesGrid">({.props = {.rows = series.at(&Series::rows)}}),
                    ui::custom<"RecoveryCurve">({.props = {.rows = series.at(&Series::rows)}})),
                ui::pill({.text = ui::format(ui::t("lims.outside", "{0} outside limits"),
                                             ui::count(ui::filter(series.at(&Series::rows), &Row::outsideLimits)))}))),
        ui::dialog(project, {.title = ui::t("lims.chooseProject", "Choose project")},
            ui::dataTable<ListProjects>(sample, {.mode = ui::TableMode::Server, .selection = ui::Selection::Single,
                                                 .onActivate = {ui::run(pick, {.projectId = ui::row("id")})}})));
});
MORPH_REGISTER_SCREEN(limsSample);
```

The "choose project" dialog is declared in the sample screen's scope, so its mutation reaches the
sample's private working copy. A picker reusable across screens borrows the owner's instance with
`"instance": {"from": "sample"}` (§16).

**What the document replaces in a hand-written application:**

- the view-model and its read-only data types, since queries and `refreshOn` replace the
  `subscribe` plus `refresh*` pairs;
- the schema annotator, since the `fields` overlay replaces it;
- a layout per section, since each is a `form` node;
- the shell wiring, since `identity`, `title` and `dirty` replace tab bookkeeping.

This screen is built, as `lims.sample`, in the lims example and unit tested there (spec 4 §5): the
framework's claims in this spec are tested against it, not against a description of it.

**What remains in the application executable:**

| Hand-written today | After |
|---|---|
| View-models, section layouts, the shell, the workspace and dialog registries | Documents on the server; the workspace shell of §7 |
| A component kit, tokens, a theme script | `ui-theme/1` tokens; host components only where a token cannot express the control; the rest restyle built-in kinds or go |
| `main`, config, bootstrap | `main` that selects the frontend and builds the `AppSource` |
| Sign-in, licence check, window chrome | The same code, behind `AppSource` and the host's window |
| Offline queue, key-range reservation, draft transfer | The models' actions (§9) |

## 12. Tests

- **Lifecycle:** `title` and `dirty` follow their expressions; `identity` focuses instead of
  duplicating; `onMount` runs once, before the scope's queries start, and a query that depends on a
  call made there is refreshed by `invalidates`; `cancelClose` keeps a screen open and is refused
  outside `onClose`; a dirty screen without `onClose` is guarded by the host; `done` is called once
  and posted; a second `requestClose` returns the pending one; `forceClose` skips `onClose`; a
  private alias is a new instance per mount and is released on unmount; a row scope's alias without
  `instance` is refused at load.
- **Host contract:** a fake `ScreenHost` mounts, reads `title` and `dirty`, closes with and without a
  veto, and receives `navigate`; the Qt Quick renderer exposes the root item.
- **App root and session:** an app-document query is shared by two screens and runs once; `signIn`
  installs the token and keeps locale, request id and metadata; `signOut` deregisters every private
  instance before the session is cleared; native sign-in runs on the owner before the catalog; a
  `secret` token is absent from `result`, diagnostics and the queue.
- **Design package:** a token set reaches a generated screen and a host component; a missing token
  takes the default; a missing or old host component renders its fallback, chosen at mount, and is
  reported once; a host component that is not safe for untrusted documents renders its fallback on an
  unverified connection; a restyle mapping with a missing prop is refused; `a11y` and `testId`
  appear as `Accessible.name` and `objectName`.
- **Workspace shell:** open, focus-instead-of-duplicate, singleton, hidden tabs keep state, close
  protocol per tab, close-all stops at the first veto, MRU order, `resume` with an unavailable
  screen, disabled entries with their reason, pins up to the limit, `arrange` reorder and hide,
  reset.
- **Preferences:** a custom `IPreferencesStore` receives the person the server derived; a request
  naming another person's key reads and writes nothing of theirs; an oversized value is refused; an
  absent entry is the default; an outdated entry is ignored.
- **Offline switch:** aliases rebind, a failed switch leaves the old backend, in-flight calls settle as
  `backendChanged`, queries on private aliases are held until `onBackendChange` has run, queries
  refetch in one batch, `env` changes.
- **Forms:** the overlay applies each key; `omit` removes a member from the body; epoch dates round
  trip exactly; a client that does not know the epoch widgets shows the integer.
- **Errors:** every kind from the registered table; an enum reply selects its message.
- **Host vocabulary:** a registered command runs with validated arguments and a mismatch is a
  reported error; an unsafe command is skipped on an unverified connection; a missing command is
  skipped and `can` is false; a host value is reactive and `null` when absent; `switchBackend` is
  refused for an unlisted backend.
- **Native panes and guard:** a host screen shares tabs, MRU order, dirty state and the session record
  with document screens; the guard offers save and close only with `onSave`, waits for pending work,
  and keeps the screen open if `dirty` stays `true`; discard does not run `onSave`; close-all stops
  at the first veto; a re-keyed tab is focused by a later opening of its identity; `reuseEmpty`
  replaces an empty tab.
- **Call and return:** `onResult` receives `return`'s value; closing by the platform runs `onCancel`;
  `window` falls back to a dialog; a borrowed instance is the opener's and is not released by the
  child; `from` outside a dialog, drawer or window is refused at load.
- **Keyboard:** chords fire inside their node and the innermost wins; a reserved chord is refused at
  load; `autofocus` and `focus`; `default` and `cancel`; `onSubmit`; a menu shows the chord of its
  item.
- **Files and jobs:** `accept` and `multiple`; an operating-system drop reaches `onDrop`; a transfer
  reports `progress`, `pending` and `error`, and a large one is chunked; preview binds a local
  reference and falls back without the viewer; a job poll stops when `state` is terminal.
- **Settings and messages:** each `customize` editor edits its point and `resetPrefs` restores the
  default; `setLocale` re-resolves every `t`; a missing catalogue shows the fallback; search matches
  titles in every manifest locale.
- **Automation conformance:** on each platform with an automation interface, a generated screen
  exposes the names, roles and ids §6 promises.
- **Port check:** an application's existing UI-automation suite, restricted to lookups by accessible
  name and role (and `testId` where the platform maps it), runs against the generated screen. A lookup
  that depends on component names or file names of the old QML does not carry over and is rewritten
  to a name or a `testId`.

## 13. Risks

- **Tokens may not reproduce a designed look.** `ui-theme/1` is a closed set; an application whose
  design needs more restyles kinds with host components. The size of that residue is measured when
  the first application is ported, not assumed.
- **Private instances cost resources.** Each private alias is a server-side model instance and,
  for a database-backed model, a connection. Dialogs and row scopes are where they multiply; the
  load-time rule for row scopes and the server's `maxLiveModels` are the brakes (spec 5 §5).
- **The workspace shell is the framework's largest new component.** It is justified by two users in
  the program (the lims example and any application with tabbed documents) and is built after the
  document lifecycle it depends on.
- **New bridge seams.** Model-free dispatch needs a binding by type id, a raw execute and a
  string-keyed attach (spec 5 §2). They are the first change of the program and block the interpreter.
- **Host component drift.** A host component's props are a contract between a client and a server
  that release independently; the version in the handshake and the fallback are what keep that
  safe.
- **Scope.** §14–§19 are the largest part of this spec, and the application they come from is the
  only one that has exercised them. Each is specified at the level an implementation needs; the
  reference screens of spec 4 §5 exercise the ones the lims example can, and the others are first
  exercised by the first ported application.
- **The builder API is a sketch.** Spec 5 §11 shows one short example; the screen in §11 needs, at
  least, `identity`, `title`, `dirty`, `onMount`, `onBackendChange`, `actions<...>()` for `refreshOn`,
  `form`, `dataTable`, `custom`, `optionalMenu`, `includes`, `filter` and `format`. The first
  implementation of the reference screen fixes the API; this spec's names are not final.
- **Optional state comes from the model.** The reference screen shows an optional measurement when
  the working copy holds data for it, so `GetMeasurements` reports which ones; a model that cannot
  report it keeps that state on the client and loses it on a backend switch.

## 14. Host vocabulary (`host/1`)

An application has commands and values no document can define: restart the process, open a path in
the operating system, a count the application's own sync engine keeps. They are registered by the
application executable, named and typed, and declared to the server in the handshake exactly as host
components are (§6):

```cpp
qt_quick::HostCommands::add({ .name = "restartApp", .version = 1, .args = {}, .safeForUntrusted = false });
qt_quick::HostCommands::add({ .name = "openPath", .version = 1, .args = { {"path", "string"} },
                              .safeForUntrusted = false });
qt_quick::HostValues::add({ .name = "sync.needsYouCount", .version = 1, .type = "int", .signal = &syncNeedsYou });
```

- **Use in a document.** The command `{"host": "openPath", "args": {"path": e}}` runs the registered
  command; the expression `{"host": "sync.needsYouCount"}` reads the registered value, which is a
  `reactive::Signal`, so a binding on it updates. `{"can": "host:openPath"}` is true when the client
  has the command, so a document shows a control only where it works.
- **Arguments are validated** against the declared types before the command runs; a mismatch is a
  reported error, not a call.
- **Absent is safe.** A command the client lacks is skipped and reported once; a value it lacks reads
  as `null`. A document guards with `can` or with `coalesce`.
- **Trust.** A command marked `safeForUntrusted: false` runs only from a document that came over a
  verified connection (spec 5 §13); otherwise it is skipped and reported. A value is read-only and
  needs no gate.
- **`switchBackend`** is not a host command. `{"switchBackend": "local"}` is a `ui/1` command that
  calls `AppSource::switchBackend` (§9) for a backend name the application's `AppSource` lists in
  `backends()`; a name it does not list is refused.
- **The handshake** carries the registered names and versions in `ui-hello` (spec 5 §12), so a
  server knows what a client can do before it serves a document that needs it.

## 15. Native panes, tabs and the close guard

- **Host screens.** A catalog entry may name `"host": "DatabaseSync"` instead of a document. The
  application registers the screen with the renderer (`ScreenHostRegistry::add(name, factory)`); the
  factory returns a `ScreenHandle` (§4) and the screen's `QQuickItem`. The workspace shell mounts it
  like a document screen, in a tab, with the handle's `title`, `dirty` and close protocol. A native
  and a document screen therefore share tabs, most-recently-used order, the unsaved indicator and the
  session record. A host screen has no `identity` unless its factory gives one.
- **Close guard.** A dirty screen without `onClose` is guarded by the shell with three choices: save
  and close, discard and close, cancel. *Save and close* is offered only when the screen declares
  `onSave` (a command list; a host screen's handle offers `save()`); the shell runs it, waits until
  nothing it started is pending, and closes the screen if `dirty` is then `false`; otherwise it keeps
  the screen open and shows the failure. *Discard and close* does not run `onSave`.
- **Tab menu.** Close, close others, close to the right, close all saved (the tabs whose `dirty` is
  not `true`), and Ctrl+W for the active tab. Each runs the close protocol per tab, one at a time,
  and stops at the first veto.
- **Re-keying.** When a screen's `identity` changes (a new record receives its key on the first
  save), the shell re-keys the tab. A later opening of that identity focuses the first tab that holds
  it; two tabs never merge.
- **Placeholders.** A catalog entry with `"reuseEmpty": true` is replaced rather than stacked: opening
  it with an `identity` when a tab of the same screen exists with no `identity` and `dirty` not
  `true` reuses that tab.
- **Persistence.** The session record (§7) lists host screens by their catalog id, like document
  screens.

## 16. Call and return

A screen can open another and receive an answer:

```json
{ "navigate": "lims.chooseProject", "params": { "obj": { "clientId": { "ref": "clientId" } } },
  "as": "dialog", "onResult": [ { "set": "projectId", "value": { "ref": "event" } } ] }
```

- `as` is `tab` (the default; the shell decides, §7), `dialog`, `drawer` or `window`. A `window` is a
  top-level window the user may move to another monitor; a renderer or host that cannot show one shows
  a dialog.
- A screen opened as a dialog, drawer or window ends with `{"return": e}`, which closes it and runs
  the opener's `onResult` with the value as `{"ref": "event"}`, or with `{"close": "self"}`, which
  closes it and runs the opener's `onCancel`. Closing it by the platform (Esc, the window's close)
  is `onCancel`.
- **Borrowing.** A model alias of the opened screen may declare `"instance": {"from": "sample"}`
  naming an alias of the opener. The child then uses the opener's binding: the same instance, not
  released when the child closes. It is allowed only for a screen opened as a dialog, drawer or
  window, and a document that declares it elsewhere is refused at load. A picker is therefore one
  document used by every screen that needs it.
- Params are typed by the callee's `params` and are validated when the screen opens.

## 17. Keyboard and focus

- **`keys`** on a screen, dialog or node maps a chord to a command list: `"keys": {"Ctrl+S": [...],
  "Ctrl+Shift+K": [...]}`. Chords are modifiers (`Ctrl`, `Shift`, `Alt`, `Meta`) joined to a key
  name (a letter, a digit, `F1`–`F24`, `Enter`, `Esc`, `Tab`, `Up`, `Down`, `Left`, `Right`,
  `Space`). A chord is active while focus is inside the node; a screen's chords are active while
  focus is inside its tab. The innermost node wins.
- **Reserved chords** are the workspace shell's (Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+W, Ctrl+K for the
  navigator) and the host's; a document that declares one is refused at load.
- **`autofocus: true`** focuses a node when its scope mounts; the first in document order wins.
  **`{"focus": "id"}`** moves focus to a node. Focus order is document order.
- **Dialogs** take `default` (Enter, when focus is not in a multi-line input) and `cancel` (Esc,
  which closes the dialog if unset) command lists; a single-line text input takes `onSubmit`.
- **Menus** show a chord for an item whose `keys` is declared on the same screen, so the label and
  the binding cannot disagree.
- **The navigator** (§7) moves with Up and Down, opens with Enter, closes with Esc and pins or unpins
  the highlighted entry with Ctrl+P. It searches the title in every locale the client holds (§19),
  and the module and group.

## 18. Files and long jobs

**Files (`files/1`).**

- A `filePicker` takes `accept` (extensions and media types) and `multiple`. A `dropZone` is a panel
  that accepts files dropped from the operating system; both deliver a list of file references to
  `onPick` or `onDrop` as `{"ref": "event"}`, usable by `upload`.
- `{"upload": e, "to": "m", "id": "t1"}` names the transfer; `{"ref": "t1.progress"}` (0 to 1, or
  `null` when unknown), `t1.pending` and `t1.error` are element values, and a transfer too large for
  one envelope is chunked.
- `{"download": e, "save": "dialog", "name": e}` offers a suggested file name.
  `{"download": e, "to": "preview", "as": "url"}` fetches to a local temporary reference bound to the
  state `url`, which a viewer takes as a prop. The viewer (a PDF viewer, an image viewer) is a host
  component with a fallback of a "Save as…" button, so a client built without it still works.
- Opening a file in the operating system's own application is a host command (§14).

**Long jobs.** A job is an action that starts work and a query that reports it:

- The action returns a job id; `GetJob` returns `{state, phase, phases, done, total, message}`. The
  document polls it with `refreshEvery` and `when: state is running`, and shows `progress` and
  `steps`. A `watch` on `state` runs the follow-up when it becomes `done` or `failed`.
- Pushing progress needs a server-initiated message the wire does not have; it is not part of
  `ui/1`. A later vocabulary version may add a `ui-event` kind negotiated by `ui-hello`.

## 19. Settings and messages

**Settings.** A settings screen is an ordinary document. Its customization editors are `customize`
nodes (spec 5 §9): `{"kind": "customize", "point": "pins"}` renders the editor for a point of kind
`pins`, `arrange`, `columns`, `choice`, `value` or `saved`, and `{"resetPrefs": "pins"}` or
`{"resetPrefs": "all"}` restores the defaults. The start page is a `choice` point of the app
document, with the options `resume`, `overview` and `screen` (any catalog id); `app-start` (§7) is its
default. An application section (a logging panel) is a host component, or host values and commands
(§14), placed in the same document.

**Messages.**

- The bundle manifest lists a catalogue per locale, `i18n/<locale>.json`, a flat map of key to text,
  fetched by hash (spec 5 §12) and cached. The client's `TranslationProvider` resolves
  `(key, locale)` from them; `{"env": "locale"}` is the active locale and `{"setLocale": "de"}`
  changes it, which re-resolves every `t`, and is stored as a preference.
- Keys are authored, `<screen>.<name>`; there are no keys derived from text. Porting a screen keeps
  its source text as the `fallback` and gives it an authored key, so a missing catalogue shows the
  source text.
- Search across locales (§17) asks the provider for each locale the manifest lists.
