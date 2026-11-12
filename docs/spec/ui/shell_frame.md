# The shell frame (`ui::ShellFrame`)

`include/morph/ui/shell_frame.hpp`. The frame a client shows its screens in: a sidebar with the navigator, a top bar,
and the page the workspace's tabs sit on. It is built from backend widgets, so every frontend shows it.

## Contents

- [Shape](#shape)
- [Signed in and signed out](#signed-in-and-signed-out)
- [The compact form](#the-compact-form)
- [Declared parts](#declared-parts)
- [Module groups](#module-groups)
- [Toast](#toast)
- [Design decisions](#design-decisions)

## Shape

`ShellFrame(backend, parent, options)` appends, to `parent` (a backend root when it is null):

| Widget | Test id | What it is |
|---|---|---|
| a row, stretch × stretch | `shell` | the whole frame |
| a row, surface `sidebar`, `fixed(kShellSidebarWidth)` (28) × stretch, gap 3, its content padded by spacers | `shell_sidebar` | the application's name (`shell_application`, a heading), a `ui::Navigator` over `ShellFrameOptions::navigator` that takes the sidebar's spare height (its group headings are overlines, which look as sections do unless the theme's `font.overline.case` is `uppercase`, and its entries body text, so the scale runs name, heading, entry), and at its foot the principal (`shell_principal`, muted) above the sign-out button (`shell_signOut`, ghost), both hidden without `onSignOut` |
| a column, stretch × stretch | — | the main column |
| a column, surface `toolbar` (see [The top bar's surface](#the-top-bars-surface)), gap 2 (compact: 1, or 3 for a tall or framed head), padded above and below by zero-height spacers | `shell_topbar` | the top bar: one line, below, a gap of step 2, or of the theme's `head.paddingY` where a backend reads one and it is above 0 (the frame tells the bar it is the expanded head bar with `setHeadBar(true)`, and false while compact; the joined bar's gap changes alike, so the page that continues it meets it without a seam) (compact: step 1, or step 3 while the head is taller than one line or ends in a framed button) from the bar's top and bottom edge |
| a row, gap 4 (compact: 1), `align: center`, padded by a zero-width spacer at both ends | `shell_topbar_line` | the top line: the two pad spacers around the wrapping row, so its content starts a gap of step 4, the page's gutter (compact: step 1), from the bar's left edge and ends as far from the right, and sits centred across the bar's line |
| a row that wraps, gap 3 (compact: 2), `align: start` (`center` while the head is one line), `justify: end`, stretch | `shell_topbar_main` | the navigation menu (`shell_nav_menu`, shown only in [the compact form](#the-compact-form)); a lead block, a column that stretches from `kShellHeadLeadMinimum` (32) units, holding the eyebrow slot (`shell_head_eyebrow`), the title line (`shell_title_line`, a row that wraps, gap 2, centred) and the lead slot (`shell_head_lead`), in that order; the title line holds the shown screen's title (`shell_title`, a heading, or a headline where `setTitleRole` says) and the inline lead slot (`shell_head_inline`, hidden unless the lead is inline); then the trail (`shell_head_trail`). Every line of it is padded at both ends by the pad spacers |
| a row that wraps, gap 3, `align: center`, content-sized, hidden unless the head or the principal shows | `shell_head_trail` | the trail slot (`shell_head`), then the principal: the two wrap as one block, and a wrapping row gives no gap to an item with no width, so a shown slot with nothing mounted leaves no space before the principal |
| a text, muted, hidden unless the principal sits in the head | `shell_head_principal` | the principal, the trail's last item (`setPrincipalInHead`), centred on the actions on whichever line the trail is |
| a row, gap 2, of two zero-width spacers, shown only compact and only while the trail ends framed | `shell_trail_end_inset` | the top line's item after the wrapping row: with the line's gap on either side it adds step 3 after the trail (`setTrailEndInset`) |
| a column, surface `page` (or the one `setPageSurface` names), stretch × stretch | `shell_page` | the page: zero-sized spacers above and either side of the content, each a gap of step 4 away, so the content is padded by step 4; the spacers hide while `setPageInset(false)`, and a hidden spacer takes no gap, so the content then fills the page |
| a row, surface `sidebar`, `fixed(kShellChatWidth)` (32) × stretch, gap 3, its content padded by spacers, after the main column | `shell_chat` | the chat's column: `chatSlot()` is where a client mounts a panel. Hidden unless `setChatShown(true)`, and always while signed out or compact, since a compact window has no width to spare beside the page |

- `page()` is the content container, where the client places the workspace.
- `navigator()` is the navigator, for its pins and search.
- `chatSlot()` is the container a client mounts a chat panel into.
- `headEyebrowSlot()`, `headLeadSlot()` and `headSlot()` are the containers a client mounts the active head's three
  parts into — `Workspace::headPartsForActive()` builds them: the eyebrow (a muted line above the title, hidden while
  the head has none), the lead (the subtitle and the badges on one wrapping line) under the
  title, and the trail (tools, actions and menu) at the line's trailing edge. Where the trail does not fit beside the
  lead block's 32 units (or its natural width where that is less), it drops to a line of its own at the trailing edge.
- `headInlineSlot()` is the container a client mounts the head's inline lead into (`HeadParts::inlineLead`), on the
  title line right after the title. `setLeadInline(true)` shows the slot and changes no sizing: the slot is
  content-sized and the lead block always stretches from its 32-unit minimum. The inline lead the workspace builds
  stretches to fill what the title line gives it, so its subtitle wraps inside that width when it is still too narrow. The lead follows the title while both fit
  the block and wraps to a line under the title when they do not, so the trail wraps only when the block's minimum and
  the trail do not fit together; the spare width lies after the lead, before the trail. `setLeadInline(false)` hides
  the slot.
- `setPageSurface(name)` draws the page area on the named surface; empty is `page`, and a name no surface answers to
  draws as `page` does. The generic client sets it from `Workspace::activePageSurface()`.
- `setHeadOneLine(oneLine)` aligns the wrapping line's items across it (`Center`) while the head is a single line, so a
  title alone is level with the trail's buttons, and by their tops (`Start`) while it has an eyebrow or a lead under the
  title, so the trail's top is the eyebrow's. A wrapping row aligns per line, so a trail that wrapped to a line of its
  own is centred on that line.
- `setPrincipalInHead(inHead)` moves the principal from the sidebar's foot to the end of the head's trail
  (`shell_head_principal`, the last item of `shell_head_trail`), so it is level with the actions on whichever line the
  trail is. Compact, the head draws none, and the navigation menu's sign-out entry stays described by the principal.
  The sign-out button stays at the sidebar's foot. One function decides what the trail shows: the principal needs the
  head placement, the expanded form and a name, and the trail shows while the head or the principal does, so a name set
  after the chrome (a sign-in) shows it.
- `setHeadShown(shown)` shows or hides the head's slots. The frame builds widgets and never sees what is mounted in
  them, so it cannot tell when the head is empty: the client says, and a new frame starts with them hidden. An
  application that mounts something else into the slots calls it itself.
- `setPageInset(inset)` insets the page's content by step 4, or lets it fill the page. The generic client sets it from
  `Workspace::activePageInset()`. With the head in the page (`HeadPlacement::InPage`) there is no shell inset and the
  flag changes nothing.
- `setTitleRole` sets the role the top bar's title is drawn in, `Heading` or `Headline`; a tab's caption, the window's
  title and an overlay's title keep theirs.
- `setTitle` sets the top bar's title and `setTitleSpans` its runs (an identifier in the mono face beside plain words,
  each run in the title's role's size and weight), which draw in place of the plain title until they are cleared with an
  empty list; `setPrincipal` the sidebar's principal text; compact, the principal is the
  sign-out entry's description.
- The sign-out button calls `ShellFrameOptions::onSignOut`; with none, it and the compact sign-out entry are hidden.

## Signed in and signed out

`showSignedIn(false)` hides the sidebar and the top bar, so the page is all there is: what a client shows before
sign-in. `showSignedIn(true)` shows them again. The frame does not decide which: a client passes whether its own
session is signed in, and the principal to show with it (none while signed out). The generic client's rule is in
[`client/client.md`](../client/client.md#the-shell).

## The top bar's surface

The top bar is drawn on one surface, chosen by `showTopBarSurface()`: `toolbarJoined` while the shown page continues the
head's block (`setHeadJoined(true)`, on both forms); otherwise the surface the shown screen asks for
(`setHeadSurface(name)`, the screen's `headSurface`) while it names one; otherwise `toolbar`. A new frame is on
`toolbar`. The generic client sets both from one effect over the active tab (`Workspace::activeHeadJoined()` and
`activeHeadSurface()`), so a tab switch or a width change moves the bar between surfaces.

## The compact form

`setCompact(true)` gives the page nearly the whole window: the sidebar, with the principal and the sign-out button at its foot, is hidden, and the top
bar starts with the navigation menu, `shell_nav_menu`, a button-style menu whose trigger is the
framework icon `nav.menu` (drawn `☰` when the theme's set does not name it), named and titled "Navigation". It lists
`Navigator::menuEntries()` (the pins, then the groups as submenus, each count after its label and the current
screen's entry checked) and opens the entry chosen through `Navigator::choose`, so it opens what the sidebar's entry
would; with a sign-out handler the menu ends with a separator and a "Sign out" entry described by the principal, which calls `onSignOut` and opens no screen; the frame sets it again whenever the navigator says its entries changed or the principal does. The compact top bar is tighter: it pads its line by step 1 above and below, which makes it one touch target and
step 1 each side (52 px with 44 px targets), the line's ends are padded by step 1 and its items are step 2 apart, so a
touch-sized menu square beside the title leaves the title room. On the terminal steps 1 and 2 are both no rows, so the
bar's rows do not change. A head that is not one line (`setHeadOneLine(false)`: an eyebrow, or a lead under the title) or
that ends in a framed button (`setTrailEnd(HeadTrailEnd::Framed)`) is content a card's padding would frame, so the bar
pads it by step 3 above and below (12 px; one row each on the terminal). What ends the line decides the trailing end:
`HeadTrailEnd` is `None` (nothing in the trail: the lead's text or badges end the line), `Unframed` (a ghost or
icon-only action, or the head menu's trigger, each of which carries its own padding) or `Framed` (a framed action, or a
head tool such as a select); a new frame is `None`. Compact, unless the end is `Unframed`, `setTrailEnd` adds step 3
after the trail, so the line ends step 4 from the edge, the page's gutter; an `Unframed` end keeps step 1, since the
button's own padding makes up the rest. The inset follows the end alone, not the head's height, and
`setHeadOneLine`, `setTrailEnd` and `setCompact` each re-evaluate both the bar's vertical padding and the inset. The client says
the end from the last shown element of the trail in its order (`Workspace::activeHeadTrailEnd`). On the terminal the
same widget tree puts such heads 3 more cells from the edge. The expanded bar needs no inset. `setCompact(false)` shows the sidebar
again, restores the bar's step 2 and the line's steps 3 and 3, and hides the menu; `compact()` answers the form, and a new frame is not compact. The form
and the sign-in state combine: signed out, the sidebar and the top bar stay hidden in either form. The frame does not
decide when to be compact; the generic client's rule is in [`client/client.md`](../client/client.md#the-shell). A
compact frame has no navigator search: the menu lists every entry.

## Declared parts

An application's [`ShellParts`](shell_parts.md) are drawn by the frame from state the client sets; the frame asks no
action itself. `setParts(ShellPartsView)` installs them. `ShellPartsView` is the frame's whole input:

| Member | Holds |
|---|---|
| `context` | the switcher: `options` (each `label`, `detail`, `group`, a `key` the handler gets back), `chosen` (an index, or none), `enabled` |
| `search` | `hint`, `text`, `phase` (`Idle`, `Busy`, `Done`), `sources` (each `title`, `failed`, `hits` of `kind`, `title`, `detail`), `highlight` (an index over the drawn hits, or none), `open` |
| `inbox` | `title`, `count` (none while unanswered), `failed`, `shown` |
| `principal` | `name`, `subtitle`, `detail`, `roles` (each `label`, `detail`), `locales`, `chosenLocale`, `formats`, `chosenFormat` (each with a display `label`), `shown` |
| `labels` | the texts the frame draws that are not data: the catalogue-resolved strings of `morph.shell.*` |

`ShellPartsHandlers` is what the frame reports: `onContext(index)`, `onSearchText(text)`, `onSearchMove(delta)`,
`onSearchOpen()`, `onSearchOpenAll()`, `onSearchClose()`, `onInbox()`, `onLocale(index)` and `onFormat(index)`. A frame
with no parts installed has none of these widgets visible.

The widgets, by test id: `shell_context` (the sidebar's top, under `shell_application`); in the top bar's line, in this
order, `shell_search` (a text input; Ctrl K focuses it) with `shell_search_popover` under it holding
`shell_search_results`, `shell_inbox` with its sibling badge `shell_inbox_count` and its error marker, and
`shell_principal_menu`. `shell_search_popover` is a `PopoverWidget` and closes on `setOnDismiss`. With parts, the top bar's
title and head slots are not used: the client mounts the workspace `HeadPlacement::InPage`, and `shell_head_trail` and
`shell_head_principal` stay hidden. The sidebar's principal text and sign-out button are hidden while a principal menu is
shown; the navigator's search field is hidden while a search part is shown (`Navigator::setSearchShown`).

Compact, the switcher, the module groups and the principal's menu are entries of `shell_nav_menu` (below), and the search
and the inbox are icon buttons of the bar (`shell_search_open`, `shell_inbox`); `shell_search_open` shows
`shell_search_overlay`, a full-window overlay with the same field and results.

The navigation menu's entries are, in order: the switcher submenu (when the context is shown), a separator, the
navigator's `menuEntries()`, a separator and the principal submenu (when shown), then, for a frame with no principal
menu and a sign-out handler, the sign-out entry. The frame keeps the list it built and maps a chosen path through it, not
by position.

## Module groups

`Navigator::setModuleGroups(true)` makes the navigator one group per module: each module is one row
(`nav_module_<module>`) holding the module's label, a chevron, and, from `setModuleBadge(module, badge)`, its count
while the module is closed; only the open module (`openModule()`, `setOpenModule(module)`; opening one closes the other)
lists its entries, under the headings of the module's own groups as sections. `setCurrent` opens the module of the
current screen and `setOpenModule` keeps the person's choice until the next `setCurrent` to another module. A module
with one entry is that entry's row. A row's accessible name carries its count and whether it is open. Pins stay above the
modules. `setSearchShown(false)` hides the navigator's own search.

`menuEntries()` changes with module groups: one submenu per module whose label carries the count after it, holding the
module's groups as heading entries and its screens, and no submenu for a one-entry module (that entry is a plain item).
Without module groups it is what it was.

## Toast

`showToast(text, tone)` shows a passive, transient message over the frame's main content area (under `_main`, the
column holding the top bar and the page — not the sidebar, which is a chrome column beside it, and not the page
itself, since the toast must stay reachable regardless of which screen is mounted there). The toast widget is made
the first time `showToast` is called, not at construction, and lives as long as the frame.

A later call always replaces the shown text and tone and restarts the dismiss interval, whether or not the toast is
currently on-screen — it is one call, `ToastWidget::show(text, tone)`, not separate setters plus a visibility toggle,
since a `setVisible(true)` while already visible would silently no-op and leave a stale dismiss timer running. The
toast is non-modal, dims nothing, and never takes keyboard focus.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Built from widgets | Backend widgets, not a document | The frame is the client's, not an application's, and every frontend has the widgets. |
| The padding | Zero-sized spacers a gap away, hidden by `setPageInset(false)` | A stack has a gap and no padding; spacers say it in the vocabulary that exists, and step 4 is 16 px on Qt Quick and a row and two columns on the terminal. A screen whose root insets itself (a `pinned` page, which pads its body and its bars' sides so its bars reach the page's edges) asks for the frame's inset to go, so the inset is drawn once, where the bars can span the page. |
| A head wider than its share of the bar | One wrapping line: the title and the lead stretch from 32 units, the trail wraps under them only when it does not fit; the principal wraps with the trail as its last item, and sign-out sits in the sidebar or the navigation menu | Wrapping to a further line is what every backend can draw with the widgets it has. Eliding loses the badges and actions, which are why the head exists; an overflow menu needs a menu node and a measuring pass the vocabulary does not have. A head costs no line of its own where there is room for it, and the sign-out button does not compete with the screen's actions for the trailing edge. |
| The width of the sidebar | 28 units, fixed | Wide enough for a navigator entry's title, and a unit on the terminal is a column. |
| A window too narrow for the sidebar | A compact form: the navigator's entries as one menu in the top bar | On a 60-column terminal the sidebar took half the screen and left a `compact` document about 30 units. A menu holds every entry in the space of one button, and is the navigator's own list, so the two never differ. Collapsing the sidebar to icons would need an icon per catalog entry, which the catalog does not carry. |
| The toast's one `show(text, tone)` call | Not separate setters plus `setVisible` | A widget's `setVisible` skips a write when the value already matches, so a second `setVisible(true)` while the toast is still showing would be silently dropped and its dismiss timer would not restart. |
| The toast's parent | `_main`, not `_root` or the page | `_root` is a row holding the sidebar and `_main` side by side; parenting there would make a floating widget compete for horizontal space as a third column. The page is replaced per screen, so parenting there would lose the toast on every navigation. |
