# The shell frame (`ui::ShellFrame`)

`include/morph/ui/shell_frame.hpp`. The frame a client shows its screens in: a sidebar with the navigator, a top bar,
and the page the workspace's tabs sit on. It is built from backend widgets, so every frontend shows it.

## Contents

- [Shape](#shape)
- [Signed in and signed out](#signed-in-and-signed-out)
- [The compact form](#the-compact-form)
- [Toast](#toast)
- [Design decisions](#design-decisions)

## Shape

`ShellFrame(backend, parent, options)` appends, to `parent` (a backend root when it is null):

| Widget | Test id | What it is |
|---|---|---|
| a row, stretch × stretch | `shell` | the whole frame |
| a row, surface `sidebar`, `fixed(kShellSidebarWidth)` (28) × stretch, gap 3, its content padded by spacers | `shell_sidebar` | the application's name (`shell_application`, a heading), a `ui::Navigator` over `ShellFrameOptions::navigator` that takes the sidebar's spare height (its group headings are overlines, which look as sections do unless the theme's `font.overline.case` is `uppercase`, and its entries body text, so the scale runs name, heading, entry), and at its foot the principal (`shell_principal`, muted) above the sign-out button (`shell_signOut`, ghost), both hidden without `onSignOut` |
| a column, stretch × stretch | — | the main column |
| a column, surface `toolbar` (see [The top bar's surface](#the-top-bars-surface)), gap 2, padded above and below by zero-height spacers | `shell_topbar` | the top bar: one line, below, a gap of step 2 from the bar's top and bottom edge |
| a row, gap 3 (compact: 1), `align: center`, padded by a zero-width spacer at both ends | `shell_topbar_line` | the top line: the two pad spacers around the wrapping row, so its content starts a gap of step 3 (compact: step 1) from the bar's left edge and ends as far from the right, and sits centred across the bar's line |
| a row that wraps, gap 3 (compact: 2), `align: start` (`center` while the head is one line), `justify: end`, stretch | `shell_topbar_main` | the navigation menu (`shell_nav_menu`, shown only in [the compact form](#the-compact-form)); a lead block, a column that stretches from `kShellHeadLeadMinimum` (32) units, holding the eyebrow slot (`shell_head_eyebrow`), the title line (`shell_title_line`, a row that wraps, gap 2, centred) and the lead slot (`shell_head_lead`), in that order; the title line holds the shown screen's title (`shell_title`, a heading) and the inline lead slot (`shell_head_inline`, hidden unless the lead is inline); then the trail slot (`shell_head`). Every line of it is padded at both ends by the pad spacers |
| a text, muted, hidden unless the principal sits in the head | `shell_head_principal` | the principal in the top line, after the wrapping row (`setPrincipalInHead`) |
| a column, surface `page` (or the one `setPageSurface` names), stretch × stretch | `shell_page` | the page: zero-sized spacers above and either side of the content, each a gap of step 4 away, so the content is padded by step 4; the spacers hide while `setPageInset(false)`, and a hidden spacer takes no gap, so the content then fills the page |

- `page()` is the content container, where the client places the workspace.
- `navigator()` is the navigator, for its pins and search.
- `headEyebrowSlot()`, `headLeadSlot()` and `headSlot()` are the containers a client mounts the active head's three
  parts into — `Workspace::headPartsForActive()` builds them: the eyebrow (a muted line above the title, hidden while
  the head has none), the lead (the subtitle and the badges on one wrapping line) under the
  title, and the trail (tools, actions and menu) at the line's trailing edge. Where the trail does not fit beside the
  lead block's 32 units (or its natural width where that is less), it drops to a line of its own at the trailing edge.
- `headInlineSlot()` is the container a client mounts the head's inline lead into (`HeadParts::inlineLead`), on the
  title line right after the title. `setLeadInline(true)` shows the slot and changes no sizing: the slot is
  content-sized and the lead block always stretches from its 32-unit minimum. The lead follows the title while both fit
  the block and wraps to a line under the title when they do not, so the trail wraps only when the block's minimum and
  the trail do not fit together; the spare width lies after the lead, before the trail. `setLeadInline(false)` hides
  the slot.
- `setPageSurface(name)` draws the page area on the named surface; empty is `page`, and a name no surface answers to
  draws as `page` does. The generic client sets it from `Workspace::activePageSurface()`.
- `setHeadOneLine(oneLine)` aligns the wrapping line's items across it (`Center`) while the head is a single line, so a
  title alone is level with the trail's buttons, and by their tops (`Start`) while it has an eyebrow or a lead under the
  title, so the trail's top is the eyebrow's. A wrapping row aligns per line, so a trail that wrapped to a line of its
  own is centred on that line.
- `setPrincipalInHead(inHead)` moves the principal from the sidebar's foot to the top line's trailing end
  (`shell_head_principal`, after the wrapping row). Compact, the head draws none, and the navigation menu's sign-out
  entry stays described by the principal. The sign-out button stays at the sidebar's foot.
- `setHeadShown(shown)` shows or hides the head's slots. The frame builds widgets and never sees what is mounted in
  them, so it cannot tell when the head is empty: the client says, and a new frame starts with them hidden. An
  application that mounts something else into the slots calls it itself.
- `setPageInset(inset)` insets the page's content by step 4, or lets it fill the page. The generic client sets it from
  `Workspace::activePageInset()`. With the head in the page (`HeadPlacement::InPage`) there is no shell inset and the
  flag changes nothing.
- `setTitle` sets the top bar's title and `setTitleSpans` its runs (an identifier in the mono face beside plain words,
  each run in the heading role's size and weight), which draw in place of the plain title until they are cleared with an
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
would; with a sign-out handler the menu ends with a separator and a "Sign out" entry described by the principal, which calls `onSignOut` and opens no screen; the frame sets it again whenever the navigator says its entries changed or the principal does. The compact top line is tighter: its ends are padded by step 1 and its items are step 2 apart, so a touch-sized menu
square beside the title leaves the title room. `setCompact(false)` shows the sidebar
again, restores steps 3 and 3, and hides the menu; `compact()` answers the form, and a new frame is not compact. The form
and the sign-in state combine: signed out, the sidebar and the top bar stay hidden in either form. The frame does not
decide when to be compact; the generic client's rule is in [`client/client.md`](../client/client.md#the-shell). A
compact frame has no navigator search: the menu lists every entry.

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
| A head wider than its share of the bar | One wrapping line: the title and the lead stretch from 32 units, the trail wraps under them only when it does not fit; the principal and sign-out sit in the sidebar or the navigation menu, so the line has one owner's controls at its end | Wrapping to a further line is what every backend can draw with the widgets it has. Eliding loses the badges and actions, which are why the head exists; an overflow menu needs a menu node and a measuring pass the vocabulary does not have. A head costs no line of its own where there is room for it, and the sign-out button no longer competes with the screen's actions for the trailing edge. |
| The width of the sidebar | 28 units, fixed | Wide enough for a navigator entry's title, and a unit on the terminal is a column. |
| A window too narrow for the sidebar | A compact form: the navigator's entries as one menu in the top bar | On a 60-column terminal the sidebar took half the screen and left a `compact` document about 30 units. A menu holds every entry in the space of one button, and is the navigator's own list, so the two never differ. Collapsing the sidebar to icons would need an icon per catalog entry, which the catalog does not carry. |
| The toast's one `show(text, tone)` call | Not separate setters plus `setVisible` | A widget's `setVisible` skips a write when the value already matches, so a second `setVisible(true)` while the toast is still showing would be silently dropped and its dismiss timer would not restart. |
| The toast's parent | `_main`, not `_root` or the page | `_root` is a row holding the sidebar and `_main` side by side; parenting there would make a floating widget compete for horizontal space as a third column. The page is replaced per screen, so parenting there would lose the toast on every navigation. |
