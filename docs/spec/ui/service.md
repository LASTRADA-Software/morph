# The UI service — design

The UI service delivers an application's screens to a generic client: a server registers its screen documents
and files in a `UiRegistry`, serves the built bundle through `UiService` as three envelope kinds, and a client
fetches them with `UiClient`, verifying every byte against a manifest whose digest it got from the handshake.
It implements spec 5 §12 (delivery, versioning, discovery), §12b (the catalog filtered for the principal) and the
delivery half of §13 (`docs/superpowers/specs/2026-10-07-ui-document-design.md`), and spec 6 §8's catalog
entries.

| Header | What it holds |
|---|---|
| `include/morph/ui/bundle.hpp` | What both sides share: content hashes, the manifest, the request and reply bodies, their bounds and parsers |
| `include/morph/ui/service.hpp` | The server: `UiRegistry`, `ServedBundle`, `UiService` |
| `include/morph/ui/service_client.hpp` | The client: `IBundleStore`, `MemoryBundleStore`, `VerifiedCache`, `UiClient`, `LoadedBundle` |

The seam it plugs into is `RemoteServer`'s envelope services ([`../core/backend.md`](../core/backend.md#envelope-services));
the kinds are in the wire kind table ([`../core/wire.md`](../core/wire.md#the-ui-services-kinds)).

## Contents

- [The kinds](#the-kinds)
- [Content addressing](#content-addressing)
- [Authorisation](#authorisation)
- [The catalog](#the-catalog)
- [Bounds](#bounds)
- [Versions and must-understand keys](#versions-and-must-understand-keys)
- [The registry](#the-registry)
- [The client](#the-client)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## The kinds

| Kind | Request `body` | `ok` reply `body` |
|---|---|---|
| `ui-hello` | `HelloRequest`: `{vocabularies, qt, components, hostComponents, hostCommands, hostValues}` | `HelloReply`: `{application, version, vocabularies, digest}` |
| `ui-catalog` | empty | `CatalogReply`: `{entries, permitted, models?, actions?, views?, wizards?}` |
| `ui-fetch` | `FetchRequest`: `{hash}` | the file's exact bytes |
| `ui-evaluate` | the action's JSON body; `modelType` and `actionType` name it | `EvaluateReply`: `{body, errors, ready}` |
| `ui-file` | `FileRequest`: `{op, …}`, one step of an upload or a download | `FileReply`: the members its `op` answers |

Every request carries the envelope's `session`; the server stamps the verified principal onto it before the
service sees it. `ui-hello` refuses a client that does not speak every vocabulary the bundle needs with
`err "missing vocabulary: <v>[, <v>…]"`; the bundle always needs `expr/1` and `ui/1`, and whatever the
registry adds (`requireVocabulary`). `hello` and `kProtocolVersion` are unchanged.

## Content addressing

- A **content hash** is `sha256:` and the 64 lower-case hexadecimal digits of the SHA-256 digest
  (`contentHash`, `isContentHash`), the spelling a document's `qml` component reference already uses. The digest
  is `session::detail::sha256`, the repository's own FIPS 180-4 implementation, checked against the standard
  vectors, so no dependency is added.
- The **manifest** is `{application, version, files: [{id, kind, hash, size, public}]}`, its files sorted by kind
  then id, written by glaze. Equal bundles therefore have equal manifests and equal digests. The kinds are
  `screen`, `app` (the app document), `theme`, `messages` and `component`.
- The **digest** is the content hash of the manifest's text, and travels in the `ui-hello` reply.
- The client hashes what it received: a `ui-fetch` reply's `body` is the file's bytes, not a re-encoding of
  them. It checks the manifest against the digest, and each file against its manifest entry, before it keeps or
  uses either. One changed byte refuses the bundle (`ConnectError::Kind::Unverified`).
- The digest is only as trustworthy as the channel that carried it, as spec 5 §12 says: content addressing
  protects against a corrupted cache and a corrupted or substituted file, not against a peer that controls the
  handshake.

## Authorisation

Every kind calls the authoriser. The pseudo-model is `ui`.

| Request | Gate |
|---|---|
| `ui-hello` | `authorize(session, "ui", "hello")` — spec 5's pseudo-action `ui/hello`. An application that serves a login screen allows it for an empty principal. |
| `ui-catalog` | `authorize(session, "ui", "catalog")` |
| `ui-fetch` of the manifest or a public file | the `ui-hello` gate |
| `ui-fetch` of any other file | `authorize(session, "ui", "fetch")`; for a screen, also the catalog's rule below |
| `ui-evaluate` | `authorize(session, modelType, actionType)`: a principal who may not run the action may not evaluate it |
| `ui-file` `begin` | `authorize(session, model, action)`: as the mutation that will reference the file |
| `ui-file` `chunk`, `end`, `read` | the upload id or file reference belongs to the session's principal |

A refusal is `err "unauthorized"`. An `IAuthorizer` that switches on its model type sees `ui` for the first three
kinds, the screen's own model types for the catalog's, and the action's model type for `ui-evaluate`.

- **The manifest is public to the `ui-hello` gate.** A client needs it before sign-in to find a public login
  screen. It lists the ids and hashes of every file, not their contents. A deployment that must not disclose
  screen ids before sign-in refuses `ui-hello` to an empty principal, and serves no public screen.
- **A screen the catalog would not list as available is not served.** `ui-fetch` of a non-public screen admits
  the principal only when the entry is available and every query the document declares at its top level is
  permitted — the catalog's own rule — so a hidden or denied screen's document is not reachable by its hash.
- **No hash oracle.** A hash the bundle does not hold is answered `err "not found"` only to a caller the `fetch`
  gate admits; anyone else gets `unauthorized`, so the gate decides before the lookup's answer is disclosed.
- **Two entries with equal bytes are one file.** It is served when any entry that names it admits the caller.
  Equal bytes disclose nothing the admitted entry does not.
- The server still authorises every call a screen makes; the catalog and `can` decide only what is shown
  (spec 5 §12b).

## The catalog

For each registered entry, in registration order:

1. An entry registered `available: false` (not built yet) is listed as it is, with its own `reason`.
2. Otherwise the principal may see the screen when it may run every query the document declares at its top level
   (`authorize(session, modelType, action)`). A host screen has no document and no query, so it is always listed.
3. A screen it may not see is omitted when `whenDenied` is `hide` (the default) and listed with
   `available: false, reason: "denied"` when it is `disable`.

- `permitted` is every action any served document names — a query, a mutation or a model-bound node such as a
  `form`, at any scope, and the app document's — that the principal may run. `{"can": …}` reads it.
- `models` and `actions` carry the `ui::Catalog` facts (`ModelInfo`, `ActionInfo`) the registry's catalog gave,
  for the documents the principal may fetch: the app document, and every listed screen that is available or
  public. They are absent when the registry had no lookup, which leaves the client's lookup empty and skips its
  rules, as on the server. `CatalogReply::facts()` turns them into the lookups `loadDocument` takes.
- `views` and `wizards` carry the forms documents a `collection` or `wizard` node builds from, keyed by
  the action the node names, for the same documents. The registry takes them from its catalog's `view`
  and `wizard` lookups, for every action a document names. The author builders put them there
  (`collection<View>`, `wizard<Wizard>`). `facts()` turns them into `Catalog::view` and
  `Catalog::wizard`.
- A query's model type is its alias's, resolved through the enclosing scopes. Load-time validation has already
  resolved every alias and requires each to name a type, so the walk cannot miss.
- **The app shell.** The app document is one file for every user, cached and verified by one hash (§12b); the
  catalog carries no shell of its own, and the client's shell omits from its menu what the catalog omits.

## Bounds

| What | Bound | Checked by |
|---|---|---|
| A request body (`ui-hello`, `ui-fetch`) | 64 KiB (`kMaxRequestBytes`) | the server, before parsing |
| A file | 4 MiB (`kMaxFileBytes`), and its escaped `ok` reply within `wire::kMaxEnvelopeBytes` | the registry; the client, before hashing |
| Files in a manifest, entries in a catalog | 4096 (`kMaxBundleFiles`) | the registry; the client's parser |
| `permitted`, model, action, view and wizard facts | 16384 each (`kMaxCatalogFacts`) | the client's parser |
| Vocabularies; host components, commands, values | 256 each (`kMaxListItems`) | both parsers; the registry |
| A name: id, kind, vocabulary, host name, application id or version | 256 bytes (`kMaxNameBytes`) | both parsers; the registry |
| JSON depth | 64 (`kMaxJsonDepth`) | every parse, before the parser runs |

A file's escaped size matters because the reply carries it as a JSON string: a control byte is six bytes on the
wire, so a 4 MiB file of them would not fit an 8 MiB envelope. The registry refuses such a file, so a reply never
exceeds what the client's `wire::decode` accepts.

## Versions and must-understand keys

Within a vocabulary version, changes are additive: every parser ignores a key it does not know. A key that begins
with `!` in an object this layer defines — a message's top level, a manifest file, a catalog entry, a model's or
action's facts — is must-understand: a parser that does not know it refuses the whole message. The client reports
that as `Unsupported`. A document's own `!` keys are the document loader's (`loadDocument`), which refuses the
document; the registry runs the loader at registration, so such a document is a registration failure on the
server, never a surprise on a client.

| Case | What happens |
|---|---|
| An old client, a new server | It sends none of the `ui-*` kinds. Its `hello` reply is byte-for-byte what a server without the service gives, and `register`, `execute` and the rest are unchanged. |
| A new client, a server without the UI service | `err "unknown envelope kind: ui-hello"`; `UiClient` reports `NoUiService` ("the server serves no UI"). |
| A client missing a vocabulary the bundle needs | The server answers `err "missing vocabulary: <v>"`; `UiClient` reports `Unsupported` with that message. A reply that names a vocabulary the client lacks is refused by the client too. |
| A newer server adds an optional key | Ignored. |
| A newer server adds a `!` key | Refused, `Unsupported`, naming the key. |

## The registry

`UiRegistry(ApplicationInfo, ui::Catalog)` collects, then `build()` freezes:

- `addScreen(entry, text)` loads the document with the registry's catalog (spec 5 §11's load-time validation),
  and refuses it when the entry id is empty, too long, registered already or not the document's id, when
  `whenDenied` is neither `hide` nor `disable`, when the entry names a `host`, when the document is the app
  document, or when the text cannot be served. The text is served byte for byte.
- `addEntry(entry)` lists an entry with no document: a host screen, or one not built (`available: false`).
- `addFile(BundleFile{kind, id, bytes, isPublic})` adds the app document (`app`, loaded and validated, id `app`), a theme, a
  message catalogue or a delivered component. A message catalogue's id is `i18n/<locale>.json`, and one whose shape
  `parseCatalogue` refuses or that has a pattern outside the `ui/1` subset is refused
  ([`catalogue.md`](catalogue.md)). A theme file is a theme index or a DTCG token file, refused when it does not
  parse; `build` refuses a theme some combination of which does not resolve ([`theme.md`](theme.md)).
- `requireVocabulary(v)` adds a vocabulary the bundle needs.
- `build()` refuses a screen that names a delivered component (`qml`) whose hash no `component` file has, and a
  bundle over the bounds. It returns a `std::shared_ptr<ServedBundle const>`: the manifest, the digest, every file
  by hash, the screens with their top-level queries and named actions, and the facts.

`UiService(bundle)` serves one built bundle and never changes it, so it needs no lock on the server strand.

**Feeding it from authoring.** A screen written with the C++ builders is added by its emitted text: the
authoring registry's `RegisteredScreen{id, text, document}` goes in as `addScreen(entry, registered.text)`, so
neither registry depends on the other.

## The client

`UiClient(exchange, hello, store)` opens a bundle over a transport the caller supplies. `Exchange` is a function
that sends one `wire::Envelope` and calls back once with the reply, or with why the transport could not deliver
it; it assigns the request's `callId` and matches the reply. `exchangeOver(transport, cbExec)` is the exchange over
a wire transport's `IEnvelopeTransport::request` (`SocketBackend`, `QtWebSocketBackend`;
[`../core/backend.md`](../core/backend.md#envelope-requests--ienvelopetransport)): the reply, `ok` or `err`, is the
exchange's reply, delivered on `cbExec`, and a failure to deliver is the error, as its message. The transport is
borrowed, so a caller that may destroy it while a reply is still queued guards the exchange itself. A local client
can answer the exchange in process with `UiService::handle`, which runs the same verification.

`open(session, done)`:

1. `ui-hello`. A transport failure is `Unreachable`; `unknown envelope kind: ui-hello` is `NoUiService`;
   `missing vocabulary: …` is `Unsupported`; any other refusal is `Refused`; a reply it cannot read is
   `Unsupported`. A vocabulary the reply needs and the client lacks is `Unsupported`, naming it.
2. The manifest: from the cache by the digest, or fetched by it and verified.
3. `ui-catalog`.
4. Every file the principal may use that the cache does not hold, fetched together rather than one round trip
   each, and each verified: the app document, themes, message catalogues, components when the client loads them,
   and the screens the catalog lists as available or public. A kind the client does not know is not fetched.
5. `done` with a `LoadedBundle`: the `ui::Bundle` the shell mounts (`applicationId`, the digest's hexadecimal
   digits, the documents by id), the hello reply, the manifest, the catalog and every verified file by hash.

`done` is called exactly once, from whichever thread the exchange calls back on. The fetches of step 4 may be
answered on different threads; what they change is guarded by the run's mutex. Each run holds what it needs, so it
outlives the `UiClient` that started it.

- **The cache.** `VerifiedCache` over an `IBundleStore` hands out only bytes that hash to their key: a store entry
  that no longer verifies is dropped and fetched again. An unchanged manifest fetches nothing; a principal who
  may now see more fetches only the new screens. `MemoryBundleStore` is the in-memory store; a persistent one is
  the application's.
- **Offline.** `openOffline(applicationId)` rebuilds the last bundle the store verified (`lastDigest`), from the
  cache alone, leaving out files that no longer verify. Its catalog is empty: the catalog is per principal and
  is not content-addressed.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Where the kinds are dispatched | An envelope-service seam in `RemoteServer`, implemented in `morph::ui` | Core must not include the UI layer; built-in kinds stay first, so a service cannot take one over. |
| Where request fields travel | In `body`, as JSON | `Envelope` keeps its fourteen members and its omission rule; the change stays additive. |
| How a file travels | Its exact bytes in the reply's `body` | The client hashes what arrived; a re-encoding could not be checked against the manifest. |
| The manifest's audience | Public to the `ui-hello` gate | A client needs it before sign-in to find the login screen; it discloses ids and hashes, not contents. |
| A hidden screen's document | Not served | Otherwise anyone who read the manifest could fetch a screen the catalog hides. |
| The app shell | The app document, one hash for everyone | §12b keeps documents the same for every user so they are cached and verified once; the client filters the menu against the catalog. |
| The catalog's facts | Only for documents the principal may fetch | They name model types and actions; a hidden screen's should not be disclosed. |
| Fetching | All missing files at once | One round trip of latency, not one per file. |
| SHA-256 | The repository's implementation | No new dependency; integrity needs only a correct SHA-256, which the known-answer test pins. |

## Evaluation

`ui-evaluate` answers spec 5 §7's preview: a query that names `evaluate` instead of `action` asks the server for
the action's body with its computed fields filled and its field errors, without running it.

- **On the server**, `UiService` hands the body to `ActionDispatcher::evaluate` of the dispatcher it was built
  with — the server's own, so it knows exactly the actions the server runs. That decodes, reconciles, recomputes
  and checks, and never calls `Model::execute`: no instance, no journal or action-log entry, no effect.
  `forms::fieldErrors` gives the errors: `required`, `out of range` (a `Quantity` outside its unit's bounds) or
  `out of bounds` (a declared `minimum`, `maximum` or `multipleOf`), by field name. `ready` says whether an
  `execute` of the same body would reach the model. A body that does not decode is `err` with the decoder's
  reason; a body the checks refuse is an ordinary `ok` answer.
- **Limits.** An evaluation runs synchronously on the server strand, like the other kinds, so it never holds an
  in-flight slot (`LimitPolicy::maxInFlightExecutes` counts executes that wait on a model strand). The server has
  no per-principal rate limit; spec 5 §7's "rate limits per principal" has nothing to count against until one
  exists.
- **On the client**, `evaluatorOver(exchange, session, cbExec)` sends each evaluation as one `ui-evaluate`, and
  `BridgeDispatcher` takes it as its `Evaluator`. The query's value is `evaluationValue` of the reply: the body's
  members, with the field errors under `errors` and the verdict under `ready`, so `errors.<field>` attaches each
  error to its element. An action field named `errors` or `ready` is shadowed by them.

## Files

`ui-file` is the file side channel of `files/1` (spec 6 §18). A server serves it when its `UiService` is built with
an `IFileStore` (`ui/file_store.hpp`); without one, `ui-file` is an unknown kind.

| `op` | Request | Reply |
|---|---|---|
| `begin` | `{name, type, size, model, action}` | `{upload}`: an upload id |
| `chunk` | `{upload, offset, data}`: `data` is base64url without padding | `{received}`: the bytes received so far |
| `end` | `{upload}` | `{ref}`: the file's reference |
| `read` | `{ref, offset, length}` | `{data, total, name, type}` |

- **Authorisation.** An upload is authorised as the mutation that will reference it: `begin` names that mutation's
  model type and action, and the server asks `authorize(session, model, action)`. A download is authorised by its
  **reference**, a capability: `file:` and 32 hex digits read straight from `std::random_device`, unguessable,
  issued for one principal. Presenting it as that principal is the authorisation; as any other principal it is
  `not found`, exactly as a reference that does not exist, so a reference cannot be probed.
- **Who issues a reference.** An upload's `end` issues one to the uploader. A server that returns a file in a reply
  issues it with `IFileStore::issue(principal, info, bytes)` for the principal the reply is for, and puts the
  reference in the reply. A model does not know the principal, so the issuing is done where the server knows it:
  the handler or service the application builds around the store.
- **Chunks.** A chunk carries at most `kMaxFileChunkBytes` (256 KiB) of file, and a `ui-file` request body is at
  most `kMaxFileRequestBytes`. A chunk's `offset` must be the bytes received so far (`bad offset` otherwise), so a
  repeated or reordered chunk is never applied; bytes past the size `begin` announced are `too large`; `end` before
  every byte arrived is `incomplete`. A `read` returns at most `kMaxFileChunkBytes` from `offset`.
- **`MemoryFileStore`** bounds a file (`maxFileBytes`) and a principal (`maxBytesPerPrincipal`, counting the sizes of
  its uploads in progress), and drops an upload that goes `uploadTtl` without a chunk. It is thread-safe: the service
  calls it on the server strand while an application issues references from its models' strands.
- **The client.** `FileTransfer(exchange, session)` uploads bytes (`upload(info, bytes, model, action, progress,
  done)`) and downloads a reference (`download(ref, progress, done)`), one chunk in flight, with progress after each.
  Reading the file the user picked is the platform's: a host's `InterpreterHost::uploadFor` reads it, then hands the
  bytes to `FileTransfer`. The interpreter calls `uploadFor` with an `UploadRequest` that names the mutation of the
  document's `{"upload": e, "to": "m"}`; its default calls `upload(file, …)`, so a host that overrides only that
  keeps working.

## Not yet
- **Development reload** (spec 5 §12) is the generic client's loop over `UiClient::open`, which already fetches
  only what changed.
- **Several vocabulary versions of one screen.** There is one vocabulary version, `ui/1`; the registry keeps one
  document per screen.
