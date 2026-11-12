# The shell's declared parts (`ui::ShellParts`)

`include/morph/ui/shell_parts.hpp`. An application declares, as data, the five parts of a shell that no screen can
draw: a scope switcher, a count per module, one search over every module, an inbox button and a menu for the signed-in
person. Each part names a model and a read action by id and the fields of its reply that it reads. The client asks the
action, and the library's [`ShellFrame`](shell_frame.md) draws the answer. The library knows no company, no role and no
record kind: what a scope, a count or a hit means is the application's, and a CRM declares the same five parts for its
own models.

## Contents

- [Why parts and not screens](#why-parts-and-not-screens)
- [The declaration](#the-declaration)
- [Delivery](#delivery)
- [What the client asks, and when](#what-the-client-asks-and-when)
- [The context](#the-context)
- [Where each part is drawn](#where-each-part-is-drawn)
- [The compact form](#the-compact-form)
- [The terminal frontend](#the-terminal-frontend)
- [Keyboard](#keyboard)
- [Accessibility names](#accessibility-names)
- [Failure and refusal](#failure-and-refusal)
- [Alternatives considered](#alternatives-considered)
- [Design decisions](#design-decisions)

## Why parts and not screens

The frame (sidebar, top bar, navigator) is the client's, built from widgets, and it is on screen whatever tab is
shown. A screen is a document mounted on the page, so a screen that drew a switcher or a search would draw it once per
tab and in the page, not in the chrome. The five parts are therefore declared beside the screens, in the bundle, and
read by the shell.

## The declaration

```cpp
struct ShellParts {
    std::optional<ContextSwitch> context;     // the scope switcher
    std::vector<ModuleCount>     modules;     // a count per module
    std::vector<SearchSource>    search;      // one source per module searched
    std::optional<InboxEntry>    inbox;       // the inbox button
    std::optional<PrincipalMenu> principal;   // the signed-in person's menu
};
```

Every part is optional and a default `ShellParts` changes nothing: a frame without parts is the frame
[`shell_frame.md`](shell_frame.md) describes. Each member below names a model by its registered type id and an action
by its registered id, as a document's `models` and `run` do, and members of the action's reply by name. A reply member
is read from the reply's JSON object by name; a name that is absent from a reply reads as nothing and is reported
(see [Failure and refusal](#failure-and-refusal)).

| Type | Members | The action answers |
|---|---|---|
| `ContextSwitch` | `model`, `action`, `optionsField`, `labelField`, `valueField` | a reply whose `optionsField` is a list of objects; each has a label (`labelField`, a string) and a value (`valueField`, a string or an integer). The options are the scopes the person may choose |
| `ModuleCount` | `module`, `model`, `action`, `countField` | a reply whose `countField` is a non-negative integer: the module's open work. `module` is a `CatalogEntry::module`; one entry per module, because an action type belongs to one model |
| `SearchSource` | `module`, `model`, `action`, `queryField`, `resultsField` | a request that has a string member `queryField` (the typed text), and a reply whose `resultsField` is a list of hits (below) |
| `InboxEntry` | `screen`, `sources` | `screen` is the screen id the button opens; `sources` is a list of `ModuleCount`, and the button's count is the sum of their counts. `module` on a source is informational: it is the module whose work the source counts |
| `PrincipalMenu` | `model`, `action`, `nameField`, `rolesField`, `locales` | a reply with the person's display name (`nameField`, a string) and roles (`rolesField`, a list of strings, each shown as written). `locales` is the list of locale tags the menu offers, such as `en`, `de`; it is data of the declaration, not of the reply |

A search hit is an object with fixed members, because the library cannot guess which of an application's fields opens
which screen: `screen` (a screen id), `params` (an object, the screen's params, absent for none), `title` (a string),
`detail` (a string, optional, muted, after the title), and `kind` (a string, optional, a message key or text: the hit's
type, drawn as a muted label before the title). A hit with no `screen` or `title` is dropped and reported.

## Delivery

The parts travel in the bundle as one manifest file of kind `shell` and id `parts`, its body the JSON of `ShellParts`.
`UiRegistry::setShellParts(ShellParts)` sets them before the bundle is built; a bundle built without calling it has no
such file. The file is content-addressed and listed in the manifest like a screen, and gated like one that is not
public: it is fetched under the `fetch` gate, so a signed-out client never reads it, and a client that does not know
the kind ignores it, as the manifest's kind rule already says. Whether the principal may *run* each declared action is
the server's `authorize` as for any action; a part whose action the principal may not run is not drawn (the same rule as
a catalog entry whose queries the principal may not run, [`service.md`](service.md)), and nothing is reported, since
that is the authorisation working.

The registry checks the declaration when the bundle is built and refuses it, naming the part and the member, for: a
`module` that no catalog entry carries; a module listed twice in `modules`; an empty field name; a locale tag listed
twice; an inbox with no sources, or whose `screen` is not a catalog entry. It does not check that an action is
registered or that a reply has the named fields: those are facts of types the registry does not hold; they are checked
by the client (below).

## What the client asks, and when

The client reads each action as it reads a screen's query: a request on the session's dispatcher, whose reply is held
and refreshed by the same effects mechanism ([`KEEPING-SCREENS-CURRENT`](../../KEEPING-SCREENS-CURRENT.md)). Nothing in
a part says what to refresh.

- **On mount.** Once the shell is signed in and the frame built, the switcher's options, every module count, every
  inbox source and the principal's menu are asked. Signing out discards every reply and the context.
- **On a change.** A count, an inbox source and the principal's reply are refreshed when an action declares that it
  changes a record the reply holds, on this client or any other, exactly as a screen's query is. So a count action's
  reply type should be a registered record; if it is not, the reply is read once, on mount and when the context
  changes, and the client reports it once ([Failure and refusal](#failure-and-refusal)) rather than showing a
  count that never moves. The switcher's options are refreshed the same way.
- **When the context changes.** Every count, inbox source and the principal's reply is asked again, since the body
  carries the context (below).
- **Search.** On each edit of the field, after a pause of `kSearchPause` (200 ms) with no further edit; an edit
  before the pause ends cancels the pending request, and one made while requests are in flight discards their
  replies, so a slow reply to an old text never replaces the hits of the current one. All sources are asked at once.
  An empty text asks nothing and clears the hits. A source's hits are shown as they arrive, grouped in `search` order.
  At most `kSearchHitsPerSource` (8) hits of a source are drawn.

**The request body.** The shell sends the body `{}` with these members added: `queryField` (search only) set to the
typed text, and, when the context is chosen and the action's schema declares a member named `context`, `context` set to
the chosen value. The schema is read from the catalog facts the client already holds for the document loader. This is
the one place the shell names a request member of its own: the context has to reach a query somehow, and the rule is
the same one every action follows. A screen's queries are different and are never edited ([The context](#the-context)).

## The context

The switcher's chosen option value is the **context**. It is held in the session's environment as
`{"env": "context"}` and read by every screen like `widthClass` and `locale`: a screen's query passes it into its body
itself (`{"env": "context"}` inside the body expression), and the shell never edits a screen's body. It is `null`
until the first reply of the switcher arrives, and `null` for an application with no switcher.

- **Choosing** an option writes its value to the environment and runs no action: every open tab re-evaluates what
  reads it, so each list refetches with the new body, and a new record's form sends the new value. The choice is
  held for the session; it is not stored between runs and not shared between clients.
- **The first option** is the context at start. When the chosen option is no longer in the options (they were refreshed,
  or another sign-in changed them), the first option becomes the context. With no options the context is `null` and the
  switcher draws as disabled.
- **A screen that changes the context.** The client registers a host command `shell.setContext` (version 1, safe for
  untrusted connections) with one argument `value` of type `any`, when the bundle has a switcher.
  `{"host": "shell.setContext", "args": {"obj": {"value": e}}}` sets the context if `value` is an option's value, and
  refuses otherwise (reported as a host command refusal, nothing changes). A banner that tells the person a refusal
  came from the scope offers a button whose action runs it, so the person need not find the switcher.
  `{"can": "host:shell.setContext"}` is true exactly while a switcher is declared. The value read back is the same
  `{"env": "context"}`.

## Where each part is drawn

| Part | Expanded `ShellFrame` |
|---|---|
| Switcher | at the top of the sidebar, under the application's name: a button-style menu (`shell_context`) whose trigger shows the chosen option's label and whose entries are the options as radio-checked items |
| Module counts | in the navigator ([`shell_frame.md`](shell_frame.md#module-groups)): a module is one row holding the module's heading and its count as a badge; only the open module lists its screens; a module with one entry is that entry's row |
| Search | the top bar's line, in the group before the principal: a text input (`shell_search`) with the hint "Search" and the shortcut "Ctrl K"; its results open in a floating column under it (`shell_search_results`) |
| Inbox | beside the search: a button (`shell_inbox`) with the screen's catalog title as its caption, an icon and the sum as a badge in the `critical` tone while it is above zero |
| Principal | replaces the principal text: a button-style menu (`shell_principal_menu`) whose trigger is the name, and whose entries are the name, the roles, a separator, the locales, and sign-out when a handler exists |

The sidebar's principal text and the head's principal text (`shell_principal`, `shell_head_principal`) are replaced by
the menu wherever the document's `principalPlacement` puts the principal; with no `principal` part they draw as today.

**Module groups.** Opening a module closes the one open before it; the open module at start, and after every
navigation, is the one of the shown screen, and the user's toggling of another is kept until the next navigation. A
count of zero draws no badge. A module the shell has no count for draws no badge and is still collapsible. Counts reach
the navigator as `Navigator::setModuleBadge(module, badge)`; the per-entry badges of the app document's `navigation`
(`setBadge`) are unchanged and show on the entries of the open module.

**Search results.** Hits are grouped under a heading per source (the module's catalog group name, through the
catalogue), each hit a row of its `kind`, `title` and `detail`. The first hit is highlighted when results arrive. A hit
opens its screen with its params as a tab (`navigate`, `as: tab`, the same path as a navigator choice), and the field
is cleared and the results closed.

## The compact form

Below `expanded`, the sidebar is gone and the parts move:

- the switcher and the module groups are inside the navigation menu: the switcher is the menu's first entry, a submenu
  titled with the chosen label, holding the options as radio-checked items; the modules are the submenus
  `Navigator::menuEntries()` already builds, each with its count after its label;
- the search and the inbox stay in the bar, as icon-only buttons (`shell_search_open`, `shell_inbox`) with their names
  as accessible names; the inbox keeps its count as a badge;
- the search opens full-screen: the same field and results in an overlay over the whole window (`shell_search_overlay`),
  closed by Escape or by a hit;
- the principal's menu is the navigation menu's last submenu: the name, the roles and the locales, then sign-out.

## The terminal frontend

The same widget tree is drawn by the terminal renderer, so each part is usable and unstyled: the switcher and the
principal's menu are the renderer's menus; the module group is a heading row with its count in brackets; the search is
a one-row input with its results in the rows under it; the inbox is a button whose caption ends with the count. The
terminal's width classes decide expanded and compact as for every shell.

## Keyboard

- **Ctrl K** focuses the search from anywhere in the frame (compact: opens the search overlay). The chord is bound
  through `Widget::setKeys` on the frame; a document cannot declare it.
- **Down** and **Up** in the field move the highlight over the hits, wrapping at the ends, while focus stays in the
  field. **Enter** opens the highlighted hit's screen. **Shift Enter** opens every drawn hit, each as a tab, in order
  (at most `kSearchOpenAllLimit`, 10, tabs). **Escape** clears the highlight, closes the results and leaves the text.
  A second Escape on an empty field returns focus to the page.
- The switcher and the principal's menu are menus: their keys are the renderer's.

## Accessibility names

| Widget | Accessible name |
|---|---|
| `shell_context` | "Scope: <chosen label>" (the catalogue key `morph.shell.context`) |
| `shell_search` | "Search" (`morph.shell.search`); the results column is a list named "Search results" |
| a hit | the kind, the title and the detail, in that order |
| `shell_inbox` | the screen's title and the count: "<title>, <n> waiting" (`morph.shell.inbox`), without the count while it is zero |
| `shell_principal_menu` | "Signed in as <name>" (`morph.shell.principal`) |
| a module row | the module's label and its count, with the expanded state |

The `morph.shell.*` keys have a fallback in the client; an application's catalogues may override them.

## Failure and refusal

A part is a convenience, so a failure of one never blocks the rest of the frame, and every one is reported, never
silent: a reply that does not hold a named field, a count that is not a non-negative integer, a hit that is dropped, an
action the dispatcher refuses (other than for authorisation), and a count action whose reply type is no record are each
reported once per part through `ShellOptions::onProblem`. A part whose reply failed draws without its value: a count
as none, the switcher as disabled, the principal menu without roles.

## Alternatives considered

- **A screen per part.** Each screen would redraw the shell inside the page, once per tab, and could not take the
  sidebar or the top bar. Rejected.
- **An application-specific shell.** Per-application client code, which the design forbids: one client opens every
  application.
- **The app document's `navigation` for module counts.** It counts per screen entry from app state; a module count is
  a read of the server's records which must follow other users' changes, and the open work is not app state.
- **Putting the context in a request header.** The shell would have to edit every body or envelope. A value every
  screen reads and passes is visible in the screen's document and testable.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| The parts' form | Data naming a model, an action and the fields read | The domain stays in the application; the library draws and schedules |
| One `ModuleCount` per module | The model is part of the entry | An action type belongs to one model, so one action cannot count several modules |
| Hit members fixed | `screen`, `params`, `title`, `detail`, `kind` | A search has to open a screen; making the application name every member would only repeat the same five |
| The context in the environment | `{"env": "context"}`, written by the shell | Every screen already reads the environment, and a read is tracked, so a change refetches |
| A host command to change it | `shell.setContext` | A document cannot write the environment, and a new command in the document vocabulary would change `ui/1` |
| Counts refresh through effects | The screens' mechanism | Nothing new for an application to learn, and other clients' changes arrive the same way |
| A pause before search | 200 ms, the last edit wins | Keystrokes then cost one request, and replies to an old text are discarded |
