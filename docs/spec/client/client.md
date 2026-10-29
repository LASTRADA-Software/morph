# The generic client (`morph::client`)

`include/morph/client/{args,source,shell,transport}.hpp`, `src/client/`. The generic client opens an application's
screens from a server or from the application linked in-process, mounts them in a shell of tabs, and answers the
commands a document cannot carry out alone. An application with a shell of its own uses the same source and
application host.

## Contents

- [Packaging](#packaging)
- [Arguments](#arguments)
- [Sources](#sources)
- [Transports](#transports)
- [The shell](#the-shell)
- [Sign-in mapping](#sign-in-mapping)
- [The application host](#the-application-host)
- [Tests](#tests)

## Packaging

- `morph::client` (`morph_client`) is a compiled STATIC library behind `MORPH_BUILD_CLIENT` (default `ON`: it needs
  only morph). Its headers name no toolkit. It is built after every optional component, so it links the transports
  that were built, PRIVATE: the Qt WebSocket transport when `morph_qt_impl` exists, the raw-socket one when
  `morph::net` does (`MORPH_CLIENT_TRANSPORT_QT`, `MORPH_CLIENT_TRANSPORT_NET`).
- It installs and exports as component `client` (`EXPORT_NAME client`, listed in `MORPH_INSTALLED_COMPONENTS`): its
  headers and its archive. `scripts/check_install_export.sh` requires it in the install, links a consumer against
  `morph::client` and calls into it; an install built without it fails the check.
- `MORPH_BUILD_EXAMPLES=ON` with `MORPH_BUILD_CLIENT=OFF` fails at configure: the examples' client and their screen
  tests run on it. The examples' remote-only client executable is `morph_remote_client`, so the library keeps the
  name `morph_client`.

## Arguments

`readArgs(args)` reads `client::Args` from the command line and removes what it read: `--server URL` or `--app NAME`
(exactly one), `--db`, `--user`, `--screen`, `--exit-after-mount` and `--dev`. A value option is `--name value` or
`--name=value`, the last one given wins, and nothing after `--` is read. A refusal leaves the arguments as they were.

The examples' client takes one more option before `readArgs` sees the rest: `--grab FILE.png` sizes the Qt Quick
window 1280×900 (from `FrontendConfig::onEngine`), and once the first screen mounted lets the window lay out and draw
for a second, saves `QQuickWindow::grabWindow()` to the file and quits as `--exit-after-mount` does, with 1 when the
image is empty, cannot be written, or the frontend is not Qt Quick. A grab in the mount's own turn or the next shows
the items where they were made, before the scene is polished. `scripts/shoot_screens.sh OUT_DIR [CLIENT]` grabs the
sign-ins of bank, lims and bookmarks, `bank.move` and `bookmarks.main` on the offscreen platform, each over a fresh
database.

## Sources

`client::Source` is a `ui::AppSource` that also hands the shell what it mounts with once it delivered a bundle: the
dispatcher over its bridge (behind the offline queue), the catalog facts, the screens the principal may open, the
file side channel, the watcher, and the schemas of every model the catalog names.

- `RemoteSource(url, timeout)` fetches the bundle from a server over the transport the frontend calls for; it sends
  `ui-hello` again on each reconnect and calls its `onConnected` listeners.
- `LocalSource(LocalApp, LocalSetup)` runs a linked application in-process: its models over a `LocalBackend`, its
  bundle answered by a `UiService` in the same process and verified by the same client as a remote one.
- `makeSource(args, apps)` answers the one the arguments name; an `--app` that names no linked application is
  refused, naming the linked ones.
- `signIn(principal, token, done)` and `signOut(done)` install a session and reload the catalog for it; `poll`
  is one step of development reload.
- `documents(onRefused)` answers the lookup `DocumentScreenHostOptions::documents` takes, over the loaded bundle:
  each screen the source lists, loaded against the current catalog facts the first time it is asked for after a
  bundle or catalog load, and kept until the next. A document that does not load is answered null and handed to
  `onRefused`. A shell of the application's own mounts with it and the same source.

## Transports

`makeTransport(context, url)` answers a `RemoteTransport` (the bridge's backend and its request verb, one object):
a `SocketBackend` on the context's I/O loop when it has one (the terminal frontend), a `QtWebSocketBackend`
otherwise (Qt Quick). A transport the frontend needs that is not built is refused, naming it.

## The shell

`mountShell(context, backend, opened, source, options)` mounts the library's `ShellFrame` with a `Workspace` of tabs
in its page for what the source delivered, or shows why it did not open. `ShellOptions` holds the start screen and
params, `onProblem`, `onMounted`, the downloads directory, the history, development mode, `onReloaded` and
`signIn`. A sign-in or sign-out closes every tab and rebuilds the frame for the new catalog; a navigator choice
opens the screen as a tab from a task posted to the owner. An empty `signIn` is `std::invalid_argument`.

A document's `navigate` is handled from a task posted to the owner too, since the screen that asked is still
running its command:

- `as: dialog` and `as: drawer` show a document screen over the tabs (`Workspace::openOverlay`,
  [workspace.md](../ui/workspace.md#overlays)); a refused one is reported and shows nothing. Anything else opens it
  as a tab, or focuses the tab that shows it.
- A host screen asked for as a dialog or drawer opens as a tab, and the substitution is reported once per screen.
  `as: window` opens a tab, since the client has one window, and is reported once per session.
- A screen that closes itself by `return` or `close: self` loses its tab or overlay: the shell's host forwards
  `onScreenClosed` to `Workspace::screenClosed`.
- A navigation with a call that mounted no document screen (it focused an open tab, its mount was refused, or the
  screen is a host screen) settles the call (`DocumentScreenHost::settleCall`): the opener's `onCancel` runs, once,
  and no later mount answers it. A navigation that waits for a sign-in's catalog and is replaced by another, or
  dropped when the next session change starts, is settled the same way; one that waited while the frame was rebuilt
  had its call cancelled when the old workspace's tabs went (`WorkspaceModel`'s destructor).
- The width-class listener also tells the workspace (`Workspace::setWidthClass`), so an open dialog overlay is a
  sheet while the window is `compact`. Problems the workspace reports, such as a callee head's menu an overlay leaves
  out, go to `onProblem`.

The shell mounts the bundle's app document (`DocumentScreenHost::mountApp`), so `{"ref": "app.name"}` in any screen
reads the session's app state, lets and queries. The root lives as long as the frame it was mounted for, and is
destroyed after the screens and before the session changes hands, as the interpreter orders:

- building the frame destroys the old root after the old workspace's tabs, and mounts the current app document after
  the new frame and workspace are built, before the first tab;
- signing out destroys it after every tab closed, before the source signs out;
- the shell's destructor destroys it after the workspace and before the document host.

Once the root is mounted and the frame built, the shell copies the root's `navigation` counts
(`AppRoot::navigationBadges()`) onto the frame's navigator (`Navigator::setBadge`) with an effect, made after both and
destroyed before either goes, so it never writes into a navigator that is gone or reads a root that is. A screen's
first visible badge is its count; a count no longer given is taken away. A count for a screen the navigator does not
list is reported once per screen, while the navigator lists anything (a signed-out frame lists nothing).

An app document that does not load, or that the interpreter refuses, is reported through `onProblem`, and the
screens mount without it. A development reload that changes the app document loads and reports it, and the new
document takes effect at the next frame build (a sign-in or sign-out); the root is not remounted in place.

The shell sets `{"env": "widthClass"}` from its view backend: before the first mount it writes
`backend.widthClass()` into the session (`Session::setEnvironmentValue`), then registers the backend's
`setOnWidthClass` listener, which writes each change, so every open tab re-evaluates it; the shell clears the
listener when it goes. The session belongs to the shell, and every screen of the client shares the value.

The frame takes its compact form (`ShellFrame::setCompact`, [shell_frame.md](../ui/shell_frame.md#the-compact-form))
below `expanded`: on `compact` and `medium`, when it is built and on each change the listener hears. The class stays
the window's. Below `expanded` the sidebar is gone, so the page is nearly the window, and a document's class describes
the space it gets; with the sidebar, an 80-unit `medium` window, an 80-column terminal among them, would leave the
page 52 units, narrower than any `compact` window. On `expanded` the page is the window less the sidebar's 28 units,
at least 132.

The frame shows its sidebar (with the principal and the sign-out button at its foot, or, compact, the navigation menu's sign-out entry) and top bar while the shell is signed in, and the page alone
otherwise (`ShellFrame::showSignedIn`). The shell is signed in after a sign-in, until a sign-out, and from the start
when its start screen is not public: an application without a public screen, or a `--screen` that is not public, is
used by the principal the source opened with. While it is signed out, opening a listed screen that is not public (a
route the history holds at start, a move back or forward) signs it in the same way. Whether a sign-in happened is the
shell's own fact, not read from the source: a local source's signed-out principal is its `--user` name and its
signed-out catalog lists the screens, so neither tells signed in from signed out. The navigator lists the catalog's
screens either way. The top bar's title and the navigator's current entry follow the active tab: each change of the
workspace sets the title to the tab's caption and `Navigator::setCurrent` to its screen. The shell builds its
`Workspace` with `HeadPlacement::External`, so a page holds only its screen and the head is shown once, in the top
bar's two head slots (`headPartsForActive()`: the lead under the title, the trail at the line's trailing edge), which the
shell shows through an effect beside those mounts (`ShellFrame::setHeadShown(Workspace::activeHeadShown())`); the slots
follow the active tab the same way, but only
re-mounts on a genuine active-tab change
(compared by the model's own tab id, not the bar's index), since the mounted row is itself reactive to its own
content.

## Sign-in mapping

- `SignedIn{principal, token}` is what a sign-in installs; `SignInMapper` maps a sign-in mutation's reply to one,
  or `std::nullopt` to refuse it. `defaultSignIn` reads `{ok, principal, token}`: refused when `ok` is `false` or
  there is no principal, and `token` may be absent.
- `ShellOptions::signIn` is the mapper, `defaultSignIn` by default; an application whose sign-in answers in a
  shape of its own (`{user, jwt}`) gives its own.
- A mapper that answers a `SignedIn` with an empty principal refuses the sign-in, which is reported through
  `onProblem` (`sign-in mapper answered no principal`): installing it would sign in an anonymous session.

## The application host

`ApplicationHost(source, signIn, downloads, signedIn, signedOut, onProblem)` is the `ui::InterpreterHost` the shell
passes as `DocumentScreenHostOptions::application`; a shell of the application's own constructs one and passes it
there itself. An empty mapper is `std::invalid_argument`.

- `signIn(reply)` maps the reply and hands the principal and token to `signedIn`; `signOut()` calls `signedOut`.
- `uploadFor` reads the picked file from disk and sends it through the source's file store; `download` fetches a
  file by reference and saves it in the downloads directory under the suggested name, else its own, keeping only
  the name's last component so a file never lands outside the directory.
- `readFile` reads a picked file a document moves inline: its size first, refusing one over the cap without
  reading it. `saveFile` saves inline bytes in the downloads directory, as `download` saves a fetched file.
- A preview, by reference or inline, is not supported: the shell has no viewer.
- `setNotifySink(sink)` sets what `notify(text, role)` calls; empty falls through to `InterpreterHost::notify`'s
  base, safe no-op. The generic shell wires the sink to its `ShellFrame::showToast` (`shell_frame.md`#toast) inside
  `buildFrame()`, re-wired on every rebuild (sign-in, sign-out, a catalog reload all replace the frame), mapping
  `notify`'s role (`"info"`, `"success"`, `"warning"`, `"error"`; empty or unknown default to `Tone::Info`) to a
  `Tone`.

## Tests

`tests/client/` (`morph_client_tests`, label `client`), headless over a `FakeAppContext` and `RecordingBackend`:
the arguments, both sources and the shell over a two-screen probe application (and the same behind a public login,
for when the frame shows its chrome), a navigation as a dialog and a callee that returns from a tab, a call to a
screen whose tab is open, the frame's compact form below `expanded`, the app document's navigation counts, the waits
the tests use, and the
sign-in mapping: a mapper reading `{user, jwt}` signs in, an empty principal is reported, and an `ApplicationHost`
passed to a hand-built `DocumentScreenHost` runs a document's `signIn` and inline `upload`, while the same host
without it reports both as not supported.
