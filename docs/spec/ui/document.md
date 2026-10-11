# UI document — design

Design spec for `include/morph/ui/document.hpp`: a `ui/1` screen document as plain structs, its parser, and its
load-time validation. The document's meaning is spec 5 §3 and §5–§13
(`docs/superpowers/specs/2026-10-07-ui-document-design.md`) and spec 6 §3 for the lifecycle keys; the expressions
it holds are [`expressions.md`](expressions.md). Turning a document into mounted nodes is the interpreter's.

## Contents

- [The model](#the-model)
- [The vocabulary tables](#the-vocabulary-tables)
- [Parsing](#parsing)
- [Must-understand keys](#must-understand-keys)
- [Validation](#validation)
- [The catalog](#the-catalog)
- [The corpus](#the-corpus)
- [Design decisions](#design-decisions)

## The model

`Document` holds the `vocab`, the screen `id`, typed `params`, `title`, `dirty` and `identity`, the lifecycle
command lists (`onMount`, `onUnmount`, `onClose`, `onSave`, `onBackendChange`), `showErrors`, the screen's
`Scope`, `prefetch`, the declared `components` and `customize` points, and the `view`.

A `Scope` holds a scope's declarations: `ModelDecl` (alias, type, and an instance that is the default,
`private`, `shared` or keyed by an expression), `StateDecl`, `LetDecl`, `QueryDecl`, `MutationDecl` (with its
concurrency, `patch`, `optimistic`, `invalidates`, command lists and `offline`), `WatchDecl` and `CheckDecl`.
Every declaration records its JSON pointer.

A `Node` is a view node or a structured entry of one. Its `kind` is a node kind, or for an entry its shape
(`column`, `case`, `tab`, `menuItem`, `action`, `drop`). It holds expression properties, literal keys (`id`,
`model`, `action`, policies), events as command lists (a custom component's `on` events named `on.<event>`),
slots of nodes (`children`, `cell`, `columns`, `cases`, `loading`, `error`), objects of expressions (`a11y`,
`drag`, a custom node's `props`), JSON passed through for the forms engine (`overrides`, `fields`), and the
scope it opens: a row's, a dialog's or a case's.

A `Command` is one shape for every verb: `target` holds the verb's name operand, and its other operands sit
under their keys as expressions, literals and nested command lists. `if`'s branches are `then` and `else`;
`seq` and `all` keep their steps under the verb's name.

## The vocabulary tables

`nodeVocabulary()` lists every node kind and entry shape of `ui/1` with the keys it takes and what each holds
(`KeyRole`); `kCommonKeys` are the keys every node takes. `commandVocabulary()` lists every verb, what its own
value is (`OperandRole`) and its operands. The parser reads the tables, and so does anything else that needs
to know the vocabulary, so there is one definition.

A node kind and an entry shape may share a name (`column`): `findKind(kind, entry)` asks for one or the other.

## Parsing

`parseDocument(json)` returns the document or the first problem, with its JSON pointer.

- A single node where a list is expected reads as a list of one; so does a single command.
- A query with no `body` has an empty object as its body: a request without fields, not an idle query.
- A declaration's name must match `[A-Za-z_][A-Za-z0-9_]*`.
- A key the parser does not know is ignored.

## Must-understand keys

A key written `"!name"` must be understood. When the parser knows `name`, it reads the key as `name`. When it
does not:

- on a node, the node is marked `refused` with the reason and the rest of the document loads: a renderer shows
  the node's fallback;
- at the top level, in a declaration, or in a command, the document is refused.

## Validation

`validate(document, catalog)` returns every refusal, each with the document's id, the JSON pointer and the
reason. A client runs it before mounting, and the server when it registers a screen.

- **Names** resolve in the innermost scope first, then outward: comprehension and command bindings, a frame's
  bound names (params, `result` in a `patch`, `body` in an `optimistic`, a `run`'s `as` for the steps after it,
  `props` in a component's fallback, `boundary` in a boundary's `error`), state, `let`, then element ids. A name
  declared twice in one scope is refused.
- **Element ids** belong to the scope their node is declared in; a table's or `forEach`'s cells, and a node with
  a `scope`, open a scope of their own.
- **Types** are checked where both sides are known: bool properties (`visible`, `enabled`, `readonly`,
  `required`, `stale`, `autofocus`, `checked`, `open`, `busy`, `dismissible`, `multiple`), `when`, `dirty`,
  `assert`, `if`'s condition and `where`; arithmetic and logic operands; a field a known record lacks; `rows`
  as a list; a state's `init` against its declared type. A query's value takes its type from the catalog's reply,
  and a row its type from its list's element.
- **Cycles.** A cycle among `let` entries is refused, naming the chain. A watch that writes state another watch
  observes is an edge; a cycle among watches is refused, naming the chain of paths.
- **Catalog.** A model type, an action, an action of the alias's model type, `refreshOn` actions of the same
  model, and a component a `custom` node names must exist.
- **Instances.** `instance` as an expression needs a keyed model, and a row scope's alias must name its
  `instance`: a private instance per row is a server-side model per row.
- **Optimistic writes** may not name a field the server calculates.
- **Offline.** `"offline": "queue"` is refused on an action that carries a credential.
- **Context.** `cancelClose` is valid only in `onClose`, `retry` only inside a boundary's `error`,
  `{"ref": "event"}` only in a command list, and `{"row": ...}` only inside a row.
- **Commands** name what their scope declares: `set` state or an element, `reset` state, `run` and `upload` a
  mutation, `refetch` a query, `open` and `close` a dialog or drawer (or `close` `self`), `focus` an element, and
  `resetPrefs` a point or `all`.

A node marked `refused` is not validated: it never mounts.

## The catalog

`Catalog` is what a document is validated against: model types (`keyed`, `stateless`), actions (model type,
request and reply types, the reply rows' calculated fields, whether the action carries a credential), and the
bundle's component names. `addAction(id, model, schema)` fills an action from the schema the server serves: the
request from the schema, the reply from its `x-reply`, and the calculated fields from the reply rows' members
marked `x-computed` or `x-readonly`.

## The corpus

`tests/data/ui/documents.json` holds a catalog and named documents, each with the exact refusals (path and
reason) it must produce, or none. A document refused at parse lists its parse problem. The server's registration
and the interpreter are meant to read the same file.

## Design decisions

- **Tables, not a struct per kind.** About forty node kinds and two dozen verbs differ in which keys they take,
  not in how a key is read. One `Node` and one `Command` shape, driven by tables, keep the parser one function
  per role, and the validator one walk.
- **Parse stops at the first problem; validation collects them all.** A parse failure leaves no structure to
  check further; a validated document is whole, and an author fixes several refusals in one pass.
- **Validation is static and conservative.** A type that cannot be known (`host`, `env`, an element's value) is
  `any`, which fits anywhere: a refusal always names a real fault.
