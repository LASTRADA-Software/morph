# Bookmarks — the design

The screens of the bookmarks example as designed, one artboard per screen. The canvas is Claude's design artifact
**Morph Bookmarks**; these files are its contents (`canvas.json` lays the artboards out, each `.dc.html` is one
artboard), committed so the screens the example builds can be compared with what was designed.

| Artboard | Screen | What it shows |
|---|---|---|
| `SignIn.dc.html` | `bookmarks.login` | A centred card: the application's name, the username, a primary sign-in button, the refusal under it. |
| `Main.dc.html` | `bookmarks.main` | The frame (sidebar, top bar, tabs) around a toolbar (search, tag filter, show archived, a primary "New bookmark") and a card holding the bookmarks table with selection; the count and the bulk actions (archive and unarchive the selection) under it. |
| `Edit.dc.html` | `bookmarks.main`, the edit drawer | A drawer from the right with the edit form, opened by activating a row; Save primary, Delete danger. |
| `Tags.dc.html` | `bookmarks.tags` | The tags with their counts in a card, beside a rename card and a merge card. |
| `Shared.dc.html` | `bookmarks.shared` | The shared feed, one card per bookmark, newest first. A shared bookmark carries no owner, so the card shows none. |
| `Import.dc.html` | `bookmarks.import` | An import card (paste a Netscape bookmarks file; the result as a success banner) beside an export card. |

## How it maps to morph

Every element is something the `ui/1` vocabulary and the `ui-theme/1` defaults build, so the colours, radii and
heights here are the theme's defaults and no theme file is needed:

| Design | morph |
|---|---|
| The dark navigator column | the app shell's sidebar (surface `sidebar`), its groups the catalog's `group` |
| The white bar with the title, the user and Sign out | the app shell's top bar (surface `toolbar`) |
| The tinted page and the white cards | surfaces `page` and `card` |
| A blue filled button, a red one | `button` with `emphasis` `primary`, `danger` |
| The bookmarks table with check boxes | `table` over `ListBookmarks`, multiple selection; activating a row opens the edit drawer |
| The tag filter | a `select` over `ListTags` writing a state, and an "All tags" button that clears it (a select over query rows has no "none" option of its own) |
| The tag pills | `badge` (tone `info`); visibility `badge` tones `ok` and `neutral` |
| The edit drawer | `drawer` holding a `form` over `EditBookmark`, prefilled from `GetBookmark` (a list row carries no description or notes, and an edit replaces every field) |
| The success line after an import | `banner` (tone `ok`) |
| Spacing | steps: 2 between fields (8 px), 4 around the content (16 px), 5 between cards (24 px) |
