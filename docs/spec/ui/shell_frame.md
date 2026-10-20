# The shell frame (`ui::ShellFrame`)

`include/morph/ui/shell_frame.hpp`. The frame a client shows its screens in: a sidebar with the navigator, a top bar,
and the page the workspace's tabs sit on. It is built from backend widgets, so every frontend shows it.

## Contents

- [Shape](#shape)
- [Signed in and signed out](#signed-in-and-signed-out)
- [Design decisions](#design-decisions)

## Shape

`ShellFrame(backend, parent, options)` appends, to `parent` (a backend root when it is null):

| Widget | Test id | What it is |
|---|---|---|
| a row, stretch × stretch | `shell` | the whole frame |
| a row, surface `sidebar`, `fixed(kShellSidebarWidth)` (28) × stretch, gap 3, its content padded by spacers | `shell_sidebar` | the application's name (`shell_application`, a heading) and a `ui::Navigator` over `ShellFrameOptions::navigator` |
| a column, stretch × stretch | — | the main column |
| a row, surface `toolbar`, gap 3, `align: center`, padded by spacers at both ends | `shell_topbar` | the shown screen's title (`shell_title`, a heading that stretches), the principal (`shell_principal`, muted) and a sign-out button (`shell_signOut`) |
| a column, surface `page`, stretch × stretch | `shell_page` | the page: zero-sized spacers above and either side of the content, each a gap of step 4 away, so the content is padded by step 4 |

- `page()` is the content container, where the client places the workspace.
- `navigator()` is the navigator, for its pins and search.
- `setTitle` and `setPrincipal` set the top bar's texts. The bar centres its children across it, so the principal's
  line sits level with the sign-out button's label rather than on the bar's top edge.
- The sign-out button calls `ShellFrameOptions::onSignOut`; with none, it is hidden.

## Signed in and signed out

`showSignedIn(false)` hides the sidebar and the top bar, so the page is all there is: what a client shows before
sign-in. `showSignedIn(true)` shows them again. The frame does not decide which: a client passes whether its own
session is signed in, and the principal to show with it (none while signed out). The generic client's rule is in
[`client/client.md`](../client/client.md#the-shell).

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Built from widgets | Backend widgets, not a document | The frame is the client's, not an application's, and every frontend has the widgets. |
| The padding | Zero-sized spacers a gap away | A stack has a gap and no padding; spacers say it in the vocabulary that exists, and step 4 is 16 px on Qt Quick and a row and two columns on the terminal. |
| The width of the sidebar | 28 units, fixed | Wide enough for a navigator entry's title, and a unit on the terminal is a column. |
