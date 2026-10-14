# pastebin, bookmarks and polls on server-defined screens (part 13, #898)

**Goal.** Spec 4 §5's three rungs as UI documents, each with screen tests on the harness, a render smoke over every
screen, and an entry in the generic client's local mode.

**Specs.** Spec 4 §5 (pastebin, bookmarks, polls), §7. In-tree: `docs/spec/ui/author.md`,
`docs/spec/ui/service.md`.

## What building found

- **The actions a query refreshes on were not named.** `DocumentActions` collected a query's own action but not its
  `refreshOn`. A document whose `refreshOn` named an action used nowhere else got no fact for it in the
  `ui-catalog` reply, and the client refused the document at load. polls' vote screen was the case: it refreshes
  on `FinalizePoll`, which it never runs. The walk now names them, with a registry test.
- **The screen harness finds a widget inside a form.** Several forms on one screen repeat their field test ids, and
  three test files had each hand-written the same walk. `findIn`, `editField` and `submitForm` replace it.
- **`signIn` takes a mutation, not a `form`.** bookmarks' Login is a mutation over a text input, so its reply can be
  installed. Spec 4 §5 lists it among the forms.
- **`set` writes a static path**, so a per-option pick cannot be written from a table row.
  - polls' votes are a `SubmitVotes` form whose `votes` list is prefilled from the poll's options, one choice per
    option.
  - Creating a poll is a form over `CreatePoll`, whose option list the forms engine edits (add and remove), rather
    than a `forEach` over a state list.
  - The vote table's rows are the options, with their counts; the view tree's table has static columns.

## Kept, not retired

Each rung's `gui/`, `gui_lib/` and `gui_wasm/` stay. Their WebAssembly clients link `gui_lib` and the QML, and the
browser demos they deploy return with the generic client under Emscripten. #900 retires them.

## Verification

Per rung:
- the screen tests and the render smoke;
- each test mutated against a fresh binary;
- clang-tidy on the changed lines, headers included.
