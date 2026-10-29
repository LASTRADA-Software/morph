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
| a row, surface `sidebar`, `fixed(kShellSidebarWidth)` (28) × stretch, gap 3, its content padded by spacers | `shell_sidebar` | the application's name (`shell_application`, a heading) and a `ui::Navigator` over `ShellFrameOptions::navigator`, whose group headings are sections and whose entries are body text, so the scale runs name, section, entry |
| a column, stretch × stretch | — | the main column |
| a column, surface `toolbar`, gap 1 | `shell_topbar` | the top bar's two lines, below |
| a row, gap 3, `align: center`, padded by spacers at both ends | `shell_topbar_main` | line 1: the navigation menu (`shell_nav_menu`, shown only in [the compact form](#the-compact-form)), the shown screen's title (`shell_title`, a heading that stretches), the principal (`shell_principal`, muted) and a sign-out button (`shell_signOut`) |
| a row, gap 3, padded by spacers at both ends, hidden on a new frame | `shell_head_line` | line 2: a horizontal scroll (`shell_head_scroll`, stretch) holding the slot for the active tab's head row (`shell_head`, a row that stretches) |
| a column, surface `page`, stretch × stretch | `shell_page` | the page: zero-sized spacers above and either side of the content, each a gap of step 4 away, so the content is padded by step 4 |

- `page()` is the content container, where the client places the workspace.
- `navigator()` is the navigator, for its pins and search.
- `headSlot()` is the container a client mounts the active tab's own head row into — `Workspace::
  headRowForActive()` builds it — the top bar's second line, below the title, shown only while `setHeadShown(true)`.
  The mounted row fills the line's width, so its spacer pushes the actions to the trailing edge, and it scrolls
  sideways when it is wider than the bar: a horizontal scroll's content is at least its viewport's width on both
  frontends.
- `setHeadShown(shown)` shows or hides that second line. The frame builds widgets and never sees what is mounted in
  the slot, so it cannot tell when the head is empty: the client says, and a new frame starts with the line hidden.
  An application that mounts something else into `headSlot()` calls it itself.
- `setTitle` and `setPrincipal` set the top bar's texts. The first line centres its children across it, so the
  principal's line sits level with the sign-out button's label rather than on the line's top edge.
- The sign-out button calls `ShellFrameOptions::onSignOut`; with none, it is hidden.

## Signed in and signed out

`showSignedIn(false)` hides the sidebar and the top bar, so the page is all there is: what a client shows before
sign-in. `showSignedIn(true)` shows them again. The frame does not decide which: a client passes whether its own
session is signed in, and the principal to show with it (none while signed out). The generic client's rule is in
[`client/client.md`](../client/client.md#the-shell).

## The compact form

`setCompact(true)` gives the page nearly the whole window: the sidebar and the principal are hidden, and the top
bar's first line starts with the navigation menu, `shell_nav_menu`, a button-style menu whose trigger is the
framework icon `nav.menu` (drawn `☰` when the theme's set does not name it), named and titled "Navigation". It lists
`Navigator::menuEntries()` (the pins, then the groups as submenus, each count after its label and the current
screen's entry checked) and opens the entry chosen through `Navigator::choose`, so it opens what the sidebar's entry
would; the frame sets it again whenever the navigator says its entries changed. `setCompact(false)` shows the sidebar
and the principal again and hides the menu; `compact()` answers the form, and a new frame is not compact. The form
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
| The padding | Zero-sized spacers a gap away | A stack has a gap and no padding; spacers say it in the vocabulary that exists, and step 4 is 16 px on Qt Quick and a row and two columns on the terminal. |
| A head wider than its share of the bar | A second line that scrolls sideways | A head row (a subtitle, badges, actions) does not fit beside a title, a principal and a sign-out button at every width: on one line it overflowed its slot and the sign-out button was drawn over it. Wrapping to a line of its own is what every backend can draw with the widgets it has. Eliding loses the badges and actions, which are why the head exists; an overflow menu needs a menu node and a measuring pass the vocabulary does not have; and one line when it fits needs a flow container that measures its children, which ui/1 has none of, while a width-class rule cannot decide it, since a head overflows an `expanded` window too. |
| The width of the sidebar | 28 units, fixed | Wide enough for a navigator entry's title, and a unit on the terminal is a column. |
| A window too narrow for the sidebar | A compact form: the navigator's entries as one menu in the top bar | On a 60-column terminal the sidebar took half the screen and left a `compact` document about 30 units. A menu holds every entry in the space of one button, and is the navigator's own list, so the two never differ. Collapsing the sidebar to icons would need an icon per catalog entry, which the catalog does not carry. |
| The toast's one `show(text, tone)` call | Not separate setters plus `setVisible` | A widget's `setVisible` skips a write when the value already matches, so a second `setVisible(true)` while the toast is still showing would be silently dropped and its dismiss timer would not restart. |
| The toast's parent | `_main`, not `_root` or the page | `_root` is a row holding the sidebar and `_main` side by side; parenting there would make a floating widget compete for horizontal space as a third column. The page is replaced per screen, so parenting there would lose the toast on every navigation. |
