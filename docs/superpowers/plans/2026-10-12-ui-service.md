# The UI service: implementation plan (part 9, #894)

**Goal.** A server serves its screens through the UI service; a generic client fetches them, verifies
every byte against a manifest whose digest it got from `ui-hello`, and caches what it verified. `hello` and
`kProtocolVersion` are unchanged.

**Specs.** Spec 5 (`docs/superpowers/specs/2026-10-07-ui-document-design.md`) §12 delivery, versioning and
discovery, §12b permissions, §13 security, §14 "Compatibility" and "Security"; spec 6
(`2026-10-08-app-hosting-design.md`) §8, the catalog entries. In-tree: `docs/spec/core/wire.md`,
`include/morph/core/remote.hpp`, `include/morph/ui/document.hpp`, `include/morph/ui/frontend.hpp`.

## The split: two pull requests

The ticket names five envelope kinds. They fall into two groups that share nothing but the dispatch seam:

| | Kinds | Authorised as | Needs |
|---|---|---|---|
| **A (this PR)** | `ui-hello`, `ui-catalog`, `ui-fetch` | pseudo-actions of the service | the registry, the manifest, the catalog filter, the verified cache |
| **B** | `ui-evaluate`, `ui-file` | the action they evaluate, or the mutation that references the file | an `ActionDispatcher` evaluator, per-field errors, a file store |

A delivers the bundle, and every case of the ticket's "Done when" is a bundle case: old client and new server,
no UI service, an unknown `!`-key, a changed byte. B is the two kinds that act on application actions:

- `ui-evaluate` needs a new runner in `ActionDispatcher` (`registry.hpp`) that decodes, reconciles, recomputes
  and validates without `execute`, and a per-field error report that `forms` does not produce today
  (`allRequiredEngaged`, `allFieldBoundsSatisfied` and the validator answer one boolean each).
- `ui-file` is the only kind with server-side state (stored uploads), chunking (spec 6 §18) and a storage seam,
  and its authorisation rule is underdetermined: "as the mutation that references the file" names the upload's
  gate but not the download's, which reads a reference a query returned. That needs a decision on #894 before
  code.

So A says `Part of #894`, and B closes it. The seam A adds to `RemoteServer` is the one B plugs into.

## Design (A)

### The dispatch seam (`core/remote.hpp`)

`RemoteServer` answers the kinds it knows and replies `unknown envelope kind: <kind>` to the rest. The UI
service lives in `morph::ui`, which depends on core; core must not include it. So core gains an extension
point, and the UI service implements it:

- `backend::IEnvelopeService`: `serves(kind)` and `handle(request, authorizer) -> encoded reply`.
- `ServerConfig::services`: the services a server answers through, fixed at construction like the rest of the
  config. A null entry is refused by the constructor (`std::invalid_argument`).
- `dispatchEnvelope`: after every built-in kind, a kind a service serves is answered by it, on the server
  strand, with the verified principal stamped first (`stampVerifiedPrincipal`), so a service never sees the
  client's claim. A built-in kind cannot be taken over. A kind nobody serves is still
  `unknown envelope kind: <kind>`, which is how a server without the UI service answers `ui-hello`.
- Synchronous: the service returns its reply. The three kinds of A are reads of an immutable bundle and run in
  microseconds; nothing in them blocks. B's `ui-file` may need an asynchronous variant; adding a virtual with a
  default is additive.

### The shared wire shapes (`ui/bundle.hpp`)

Requests and replies travel in `Envelope::body` as JSON text, so `Envelope` keeps its fourteen members and
`encode`'s omission rule is untouched. A file travels as its exact bytes in an `ok` reply's `body`, so the
client hashes what it received, not a re-encoding.

- **Hashes**: `sha256:<64 lower-case hex digits>`, the spelling `Component::qml` already uses. SHA-256 is
  `session::detail::sha256` from `session_auth.hpp`, the repository's test-vector-verified implementation, so no
  dependency is added. It is a reference implementation; integrity here needs collision resistance, which
  SHA-256 gives whatever implementation computes it correctly, and the known-answer test pins that.
- **Manifest**: `{application, version, files: [{id, kind, hash, size, public}]}`, sorted by kind then id and
  written by glaze, so equal bundles have equal digests. Kinds: `screen`, `app`, `theme`, `messages`,
  `component`. The digest is the hash of the manifest's text.
- **`ui-hello`**: request `{vocabularies, qt, components, hostComponents, hostCommands, hostValues}`; reply
  `{application, version, vocabularies, digest}`.
- **`ui-catalog`**: reply `{entries, permitted, models, actions}`. `entries` are spec 6 §8's; `permitted` is the
  principal's permitted action ids (`{"can": …}`); `models` and `actions` are the `ui::Catalog` facts the
  client's load-time validation needs (`ModelInfo`, `ActionInfo`), for the documents the principal may fetch.
- **`ui-fetch`**: request `{hash}`; reply the file's bytes.
- **Parsing** (both sides): `parseJson` first (64 levels), then glaze with unknown keys ignored. A key that
  begins with `!` in an object this layer defines (the top level, a manifest file, a catalog entry) is
  must-understand and refuses the whole message. Every list, string and file is bounded (below).

### Bounds

| What | Bound | Where it is checked |
|---|---|---|
| A request body (`ui-hello`, `ui-fetch`) | 64 KiB | server, before parsing |
| A file | 4 MiB | registration; client, before hashing |
| Files in a bundle, catalog entries | 4096 | registration; client parse |
| Vocabularies, host components/commands/values | 256 each | both parsers |
| A name (id, vocabulary, host name) | 256 bytes | both parsers |
| JSON depth | 64 (`kMaxJsonDepth`) | every parse |

The envelope itself is already capped at 8 MiB by `wire::decode`; a 4 MiB file escapes to at most 6 bytes per
byte only for control characters, which documents and QML do not contain, and the registration bound refuses a
file whose escaped form would not fit (`encode` of the reply is measured at registration).

### Authorisation

Every kind calls the authoriser. The pseudo-model is `ui`; the pseudo-actions are `hello`, `catalog`, `fetch`
(spec 5 writes the first as `ui/hello`).

| Kind | Gate |
|---|---|
| `ui-hello` | `authorize(session, "ui", "hello")`: an application with a login screen allows it for an empty principal |
| `ui-catalog` | `authorize(session, "ui", "catalog")` |
| `ui-fetch`, a public file or the manifest | the `ui-hello` gate: whoever may learn the digest may fetch what it covers that is public |
| `ui-fetch`, any other file | `authorize(session, "ui", "fetch")`, and for a screen, the catalog's permission rule: every top-level query permitted |

A hash nobody serves is answered `not found` only after the `fetch` gate passes, so a caller the gate refuses
cannot probe which hashes exist. The manifest lists every file's id and hash, not contents; it is public to the
`ui-hello` gate because a client needs it before sign-in to find the login screen. A deployment that must not
disclose screen ids before sign-in refuses `ui-hello` to an empty principal.

### The catalog filter (§12b, spec 6 §8)

For each entry, in registration order:

- `available: false` (not built) is listed disabled with its own reason.
- Otherwise, the principal may see the screen when it may run every query the document declares at its top
  level (`authorize(session, modelType, action)`). If not: `whenDenied: hide` (the default) omits it;
  `disable` lists it with `available: false, reason: "denied"`.
- `permitted` is every action any served document names in a query or mutation, at any scope, that the
  principal may run.
- A query's model type is its alias's type, resolved through the enclosing scopes; a borrowed alias's comes from
  the registry's `ui::Catalog`. An action whose model type resolves neither way refuses registration.

The app document is one file for every user (§12b: a document is the same for everyone, one hash); the shell's
menu omits what the catalog omits, on the client.

### The server registry (`ui/service.hpp`)

- `UiRegistry(ApplicationInfo, ui::Catalog)`: `addScreen(entry, text)` loads the text with the registry's
  catalog (load-time validation, §11), checks the entry (id matches, `whenDenied` is `hide` or `disable`, no
  duplicate) and bounds; `addEntry(entry)` lists a host screen or one not built, with no document;
  `addFile(kind, id, bytes, public)` adds the app document (validated as a document), a theme, a message
  catalogue or a component; `requireVocabulary` adds one the bundle needs beyond `ui/1` and `expr/1`.
- `build()` checks that every delivered component a screen names by hash is in the bundle, writes the
  manifest and returns an immutable `ServedBundle`.
- `UiService(std::shared_ptr<ServedBundle const>)` is the `IEnvelopeService`.
- **Feeding it from authoring.** `ui::ScreenRegistry` (`author.hpp`, open in #920, not merged) yields
  `RegisteredScreen{id, text, document}`; `addScreen(entry, registered.text)` takes the text, so the authoring
  registry feeds this one without either depending on the other. Nothing here includes `author.hpp`.

### The client (`ui/service_client.hpp`)

- `IBundleStore` (`get`, `put`, `lastDigest`, `setLastDigest`) and `MemoryBundleStore`. A disk store is the
  generic client's (part 10).
- `VerifiedCache` over a store: `get(hash)` returns bytes only when they hash to `hash` (a corrupted store entry
  is a miss), `put` refuses bytes that do not.
- `UiClient(exchange, hello, store)`: `open(session, done)` runs `ui-hello`, checks vocabularies, takes the
  manifest from the cache or fetches it by digest and verifies it, runs `ui-catalog`, then fetches every file it
  needs that the cache does not hold — all at once, not one round trip each — verifies each, and delivers a
  `LoadedBundle` (the `ui::Bundle` for the shell, the hello reply, the manifest, the catalog, the other files,
  and `catalogFacts()` for load-time validation). `openOffline(applicationId)` rebuilds the last verified bundle
  from the cache alone.
- The exchange is a function, so the transport is the caller's: the generic client (part 10) binds it to
  `SocketBackend` or Qt; the tests bind it to `RemoteServer::handleInline`.
- Replies map onto `ConnectError`: a transport failure is `Unreachable`; `unknown envelope kind: ui-hello` is
  `NoUiService`; `missing vocabulary: <v>`, a vocabulary the reply needs that the client lacks, or a reply it
  cannot read (an unknown `!`-key) is `Unsupported`; a hash mismatch is `Unverified`; `unauthorized` and every
  other refusal is `Refused`.

### Not in A, and why

- `ui-evaluate`, `ui-file`: B, above.
- **Development reload** (re-sending `ui-hello` every second and remounting): it is the generic client's loop
  over `UiClient::open`, which already fetches only changed files.
- **Several vocabulary versions of one screen**: there is one vocabulary version, `ui/1`. The registry keeps one
  document per screen; `ui/2` adds the per-version choice.

## Tasks (one commit each)

1. This plan.
2. `core`: `IEnvelopeService` and `ServerConfig::services`; tests: a served kind, an unserved kind, a built-in
   kind a service cannot take, the verified principal, a null service refused.
3. `ui/bundle.hpp`: hashes, manifest, the wire shapes and their parsers; tests: the SHA-256 known answer,
   round trips, every refusal (bounds, depth, `!`-keys, malformed hashes, duplicates).
4. `ui/service.hpp`: `UiRegistry`, `ServedBundle`, `UiService`; tests: registration refusals, deterministic
   digest, each kind's gate, the catalog filter, fetch refusals.
5. `ui/service_client.hpp`: the store, the verified cache and `UiClient`; tests: the spec 5 §14 compatibility
   and security cases end to end through `RemoteServer`, the cache (nothing fetched on an unchanged manifest,
   a corrupted entry refetched, offline open).
6. Docs: `docs/spec/core/wire.md` kind table, `docs/spec/ui/service.md`, `docs/spec/core/backend.md` (the
   seam), spec 5 corrections, `docs/spec/README.md`, `docs/ARCHITECTURE.md`, CHANGELOG, CMake FILE_SET.

## Verification

- Build `morph_tests` (`-DMORPH_BUILD_EXAMPLES=OFF`, `-j 4`) and run the `[ui-service]`, `[remote]` and
  `[wire]` tags, then the whole binary.
- Mutations, each restored after: skip the digest comparison in the client; skip the `ui-fetch` gate on a
  non-public file; skip the screen permission rule in the catalog. Each must fail a test.
- Coverage: a clang coverage build of `morph_tests` if the machine allows it; otherwise say the number is
  inferred from the refusal tests and leave it to CI's coverage leg.
- Doxygen with `WARN_AS_ERROR`; clang-format 22 on every changed file.
