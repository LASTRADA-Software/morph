# The workspace shell, slice d2: start, navigator, pins, prefetch and host screens (part 8, #893)

**Goal.** The rest of spec 6 §7 and §15's host screens:
- `resume` reopens the person's recorded tabs, and an `app-start` screen or the fallback opens otherwise;
- the record is written as tabs open and close;
- a navigator lists the catalog by module and group, with disabled entries and a title search;
- pins are the person's ordered shortcuts, capped;
- hovering an entry or a pin starts the screen's `prefetch` queries;
- a catalog entry that names a `host` screen mounts from a `ScreenHostRegistry` into a tab like a document screen.

**Specs.** Spec 6 §7 (start, navigator, pins, prefetch), §15 (host screens, persistence), §20 (routes); spec 5 §5
(`prefetch`). In-tree: `docs/spec/ui/workspace.md`, `docs/spec/ui/host.md`, `docs/spec/ui/backend_contract.md`.

## What planning found

1. **Nothing starts a `prefetch` query.** The document validates `prefetch` (a body that reads only params) and
   `Session::prefetch` warms one cache entry, but no caller connects the two. `DocumentScreenHost::prefetch`
   evaluates each named query's body over the params and warms the entry the mounted screen will read.
   - A keyed alias's token is `shared:<key>`, and a stateless model's is `shared`, so either one's entry is the
     one the screen reads.
   - A private alias gets a fresh token at mount, so nothing could read what a prefetch warmed: it is skipped.
2. **The view backend has no hover event.** `Widget::setOnHover` is added with a default that does nothing, so
   a backend without hover still conforms. Qt Quick's `MorphWidget` already has a `HoverHandler` for tooltips.
3. **A record is routes.** A tab's route is `routeOf` over its document's declared params (§20). A host screen
   has no document, so its route is its catalog id. `resume` parses each route back against the documents, and
   skips one that does not parse or names an unavailable screen.
4. **Debouncing belongs to the caller's clock.** The shell calls `onRecord` once per change of the open tabs.
   With a `reactive::Scheduler`, it calls `onRecord` once after a quiet period instead. `flushRecord()` writes
   at once, as sign-out needs.
5. **Pins are a list the navigator shows and edits.** Storing them is the application's, through the
   preferences model under the `pins` point (§8). The navigator takes the stored list and reports each change.
6. **Host screens are backend-neutral.** Spec 6 §15 has the factory return a `QQuickItem`. Here a factory mounts
   into the tab's page container and returns a `ScreenHandle`, so the same registry serves every backend and the
   recording backend in tests.

## Design

- `Widget::setOnHover(std::function<void()>)`: default no-op. `RecordingBackend` records it and its test
  `hover(id)` fires it. Qt Quick's `HoverHandler` asks through the relay when one is set.
- `DocumentScreenHost::prefetch(screenId, params)` and `DocumentScreenHost::routeOf(screenId, params)`.
- `ScreenHostRegistry`: `add(name, factory)`, `find(name)`. A factory is
  `(ParamMap const&, ContainerWidget& page) -> std::expected<std::unique_ptr<ScreenHandle>, MountError>`.
- `WorkspaceScreen` gains `host` and `available`. `WorkspaceOptions` gains:
  - `hosts`, the registry;
  - `onRecord`, which receives the routes of the open tabs in order;
  - `scheduler` and `recordDelay`, for debouncing.
- `Workspace::start(WorkspaceStart)`: `{start, fallback, recorded}`. `resume` opens each recorded route, and the
  fallback when none opened; any other `start` opens that screen. `Workspace::flushRecord()` writes at once.
- `ui::Navigator(backend, parent, NavigatorOptions)`:
  - a search input, a pins row, and per module and group a header and one button per entry;
  - an unavailable entry is disabled, with its reason as the tooltip;
  - the search hides the entries whose titles do not contain the text, case-insensitively, in any title
    `titles(entry)` gives (every locale);
  - `onOpen(screenId)`, `onPrefetch(screenId)` on hover, `pin`, `unpin`, `pins()`, and `onPins` with the new list,
    capped at `maxPins`.

## Not in this slice

- The three-choice close guard dialog and the Ctrl+Tab switcher overlay are the host's drawing. The shell's
  hooks for both exist.
- A Qt Quick host-screen factory over a `QQuickItem` is the application's, through the generic factory.

## Tasks (one commit each)

1. This plan.
2. `Widget::setOnHover` in the contract, the recording backend and Qt Quick.
3. `DocumentScreenHost::prefetch` and `routeOf`.
4. `ScreenHostRegistry` and host screens in the workspace.
5. The workspace's start and record.
6. `ui::Navigator` with pins and prefetch.
7. Docs: `workspace.md`, `host.md`, `backend_contract.md`, CHANGELOG.

## Verification

Each test is shown to fail on a mutation built into a fresh binary:
- the prefetch skips keyed aliases (no request before the mount);
- resume opens unavailable routes;
- the record is not written on close;
- the search ignores the second locale's title;
- the pin cap is not enforced;
- a host screen is mounted as a document.

Then GCC full, Qt Quick, Doxygen, and clang-tidy on the changed lines with the file list split.
