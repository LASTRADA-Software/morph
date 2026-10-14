# Kanban parity: members, rules, activity, comments and attachments on the screens

Part of #899 (spec 4 §5, "kanban"). kanban's screens gain what its presenters and bridges still held alone, so #900
can retire them: a public sign-in screen, and a board in tabs — the board with a task's detail, its members, its
rules and its activity.

## What the old tests pin, and where it goes

| Old test | Claim | Screen test |
|---|---|---|
| `test_project_admin_presenter.cpp`, `test_project_admin_qml_bridge.cpp` | log in; list projects; give and remove a member's role | "the members tab gives someone a role and removes them" (and every test signs in) |
| `test_board_presenter.cpp`, `test_board_qml_bridge.cpp` | open a board; columns, lanes, tasks; move a task; comment on it; create, list and delete rules over the board's columns; activity | "a new project's board takes columns…", "a task's detail lists its comments…", "the rules tab creates a rule…", "the activity tab follows what the board does" |
| `test_board_qml_bridge.cpp` (attachments) | upload a file, record it, download it back | "a picked file is attached to a task, and downloads back byte for byte" |
| `test_board_offline_bridge.cpp`, `test_kanban_offline.cpp` | a move made offline is queued and replayed on reconnect; dead letters are counted | **not yet**: the screen harness's simulated mode cannot disconnect, and the generic client has no offline queue in remote mode. Left on #899. |
| `test_board_concurrent_drag.cpp` | N concurrent moves never desync positions | covered by the model's own tests; the screen's moves are `serial` |

## Decisions

- **Sign-in is its own public screen.** The catalog lists a screen only to whoever may run its queries, so a screen
  that holds both sign-in and the project list is invisible before sign-in.
- **A new rule is made in a dialog.** A form's `Choice` fetches its options once, when the form is built (#965); in a
  dialog the form is built when it opens, after `OpenBoard`, so the board's columns are there to pick.
- **The board's forms refetch the activity.** A form's submit is not a mutation, so `refreshOn` does not see it
  (#950).
- **In process, the activity is an in-memory journal** that `prepareLocal` installs as the default action log; a
  keyed board journals under its project id in local mode as on a server (#964).
- **Files.** The generic client reads a picked file and uploads it over the file side channel, and saves a download
  in its downloads directory; local mode keeps uploads in a `MemoryFileStore`.

## Found on the way

- #964: a keyed local instance journalled under the empty key. Fixed here.
- #965: a document form's `Choice` fetches its options once, at mount. Worked around with a dialog.
- A debug misuse report always claimed an off-owner run; it now prints its site first.

## Verification

`ladder_kanban_screen_tests`: 7 test cases, 139 assertions. The full suite of the bank build passes (4262 tests),
every render smoke included.
