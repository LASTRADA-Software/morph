# Server-hosted chat and MCP

A person working in a morph GUI types or speaks to a chat built into that GUI, and the application changes as if they
had used the screen. The assistant lives on the server. It is one more frontend: it calls the same model actions the
GUI calls, and the GUI shows the result through the push path every other change already uses.

## Why

Entering data by hand is slow when the person's attention or hands are elsewhere. Saying what happened is faster than
finding the fields. The framework already makes every change reach every open screen, so an assistant needs no
special route to the data and no per-screen code.

## The experience

1. On a screen for one record, the person says or types "add ignition test".
2. The action a button on that screen would run is called on the server, and the section for the test appears through
   server push.
3. They say "container 100, before 299, after 223". The assistant understands this against the screen and the
   conversation, runs the action the screen's form would submit, and the values appear the same way.

Success is that sequence working on a real screen with no screen-specific assistant code, and with the assistant's
tools, identity and data all coming from what the GUI already uses.

## Goals

This spec covers typed chat in the GUI:

- A chat panel in the existing Qt frontend, available to an application by server configuration, with no per-screen
  code.
- The chat knows the open screen and the primary of the model it is working on, so nobody has to say which record.
- The assistant calls the actions of that screen directly, destructive ones included. A change applies at once.
- The AI model provider can be replaced in one place, or chosen by configuration.

Two parts of the same product follow in specs of their own, once typed chat works, because nothing here depends on them:

- **Voice:** speech becomes text and takes the typed path, through a replaceable speech provider. Audio should upload
  through the `ui-file` side channel, since an envelope is capped at 8 MiB.
- **External MCP clients:** MCP's streamable HTTP transport hosted by the server, so a client such as Claude Desktop can
  connect as an extra frontend with no screen.

## Non-goals

- Filling in a form for the person to review, moving a selection, or navigating. The assistant changes data; it does
  not operate the open GUI.
- Reading what has been typed but not submitted. The assistant sees what the backend sees.
- An external MCP client attaching to someone's open GUI session. An external client has no screen.
- A confirmation step before a change, or a block on destructive actions.
- Screens built around a working copy, whose edits are not stored until a separate save and whose flags are set only by
  their own buttons. LIMS's sample entry screen is one. Supporting them is later work.
- New actions made for the assistant. If a person cannot do it through an action, neither can the assistant.

## Anti-goals

This has failed, even if it works in a demo, if:

- it gets past any check the GUI's own calls get: authentication, `authorize`, `authorizeInstance`, limits or the
  journal;
- an application has to write assistant code per screen, or change its models, to get the chat;
- a provider's name or request format leaks outside the one place that knows it;
- a context sent by a client can give a person more power than they already have.

## Decided by the owner

- The assistant mimics the frontend: same actions, same interaction with the backend.
- The chat belongs to the screen being viewed, and the assistant is told what is happening there.
- The server calls the AI model and holds its keys. The conversation lives on the server.
- Voice becomes text first, then follows the typed path.
- Changes apply at once, and spoken numbers are recorded as heard.
- The assistant may run destructive actions, such as a delete, like any other action the person may run.
- Frontends can be combined: the Qt GUI includes the chat, and external MCP clients are also possible.
- "Primary" is `env.primary`, the key identifying a shared model instance.
- Chat turns use the connection the GUI already holds, under the signed-in user, so the assistant shares its identity
  and its model instances.
- Providers are replaceable in one place, or configurable.
- Every change the assistant makes goes into the journal, marked as entered through the assistant and carrying the
  words as heard or typed.
- The proof lives in the LIMS example, with an ignition test, so the sequence above can be repeated in this repository.
- Each pair of screen and record has its own conversation, kept on the server. How long it is kept is server
  configuration, and nothing survives a server restart.

## How it fits what exists

| Exists today | Used for |
|---|---|
| `ui::McpAdapter` (`include/morph/ui/agent.hpp`): tools from the catalog and `schemas`, `tools/call` as an `execute` | The tool list and tool calls, for both the built-in assistant and external clients |
| `IEnvelopeService` (`include/morph/core/remote.hpp`) | The server's chat kinds, reached over the GUI's WebSocket. It needs new seams, below |
| `RemoteServer::handle(msg, reply)` | The assistant's calls, carrying the client's session and the id of the screen's instance. It can be called from any thread, and executes on one instance serialise on its strand, so a call and the screen's own interleave only between actions |
| `RemoteServer::post(cid, envelope)` | A turn's progress events, to the one connection. It can be called from any thread |
| `event` envelopes with a topic (`include/morph/core/wire.hpp`) | Streaming a reply to the client on a new topic. The client ignores topics it does not know |
| Records, effects and fan-out (`docs/KEEPING-SCREENS-CURRENT.md`) | The GUI's reaction. Nothing new |
| `ui::ShellFrame`, `DocumentScreenHost` | The panel's home, and which screen is open |
| `client::Source` exchange | How the panel sends a chat turn |

## What has to change

**The adapter acts on instances it is given.** `McpAdapter` always opens a connection and registers its own instances.
The built-in assistant has to act on the instances the GUI's connection already holds, because screens declare
`Instance::Private` even on a keyed model, and a separate instance would hold separate state. External clients keep
today's behaviour of their own connection.

A chat turn arrives on the GUI's connection, under its session. The executes the turn causes are sent unscoped
(`cid == 0`), naming the screen's instance ids:

- `dispatchExecute` checks the session and the instance's owner, not the connection, so every check still applies.
- Fan-out still reaches the GUI, through the instance's holding scopes and the matching principal.
- The connection's principal baseline is left alone. A GUI that signs in again during a turn would otherwise flip it on
  each envelope, dropping its watches each time.
- The GUI's call ids cannot collide with the assistant's, because the cancel table is keyed by connection and call id.
- The cost is that an unscoped execute is not cancellable, so cancelling a turn stops it between actions and does not
  interrupt an action already running.

**The tools narrow to the screen.** A screen document names its queries, mutations and model-bound nodes. The built-in
assistant's tools are those, bound to the model instances the screen holds, and keyed by the screen's alias for each
model: the adapter's present key, the model type, cannot tell two instances of one type on a screen apart. An external
client keeps every action `permitted` lists.

**The GUI says where it is.** A chat turn carries the screen's id, its params, and each of its models' instance and
primary. The server treats this as a hint about what the person means. Every action it leads to is checked as usual.
The assistant reads current data by running the screen's own queries through the same tool path, so the client does
not send its data. The interpreter's `ModelChannel` exposes only `execute`, and `Session` has no accessor for instance
ids, so the client needs a new way to read them.

**The turn runs off the server strand.** `IEnvelopeService::handle` runs on the strand, must not block, and returns its
one reply synchronously, and a model call takes seconds. The service accepts the turn and answers at once, runs the loop
on its own executor, and streams progress as events on a new topic to that connection only. The panel matches them to
the turn by a turn id, because replies and events travel different paths. A turn in progress can be cancelled by the
person, which must also cancel the provider's in-flight call, and it ends when the connection closes.

**Events are hints.** `post` writes nothing while a connection is behind and owes a `resync`, and refuses an oversized
envelope, so a turn's final text could vanish with no error. The server holds each turn's state and a request kind reads
it. The panel reads it when a turn starts, on `resync`, and on reconnect. A reconnect is a new connection with new
instance ids, so a running turn ends and the next one carries the new ids, while the conversation, keyed by principal,
screen and record, continues.

**A model's output is untrusted input.** A malformed tool call, or one naming a tool that is not on the screen, comes
back to the model as a tool error and counts against the cap. It never faults the executor. The tool list is worked out
per turn from the screen and the current catalog, not the adapter's catalog cached for its life.

**New seams on `RemoteServer` and its services.** A service today gets no connection id, no server handle and no word
of a closing connection, and it is built before the server exists. The chat service needs:

- the connection id with each envelope, beside the `ConnectionFacts` it already gets;
- a handle to the server, bound after construction, because services are consumed by the constructor, and not extending
  the server's life;
- a notification when a connection closes;
- an internal way to run an execute with a server-set origin, which no transport can reach.

The chat kinds pass an `authorize` gate of their own.

**Identity during a turn.** Each tool call carries the session of the turn and goes through the usual stamping, so
nothing is cached past the server's own checks. The server cannot see a client's sign-out, which is client-only, and
learns of a principal change only from the connection's next envelope. So the panel sends an explicit end-of-chat when
the client signs out, which stops the turn. A connection that closes stops it too. A token that remains valid after
sign-out is not something this feature can revoke.

**Limits.** Chat configuration caps tool calls per turn, turns in progress per principal and per connection, and
conversations and their size per principal. A turn over its cap stops and says so in the chat. These matter more because
destructive actions are allowed: a record's contents reach the model, so they could steer it, and the cap bounds what one
turn can do. Provider spend is not tracked here beyond these caps.

**The journal knows the assistant.** A change made through the chat has the signed-in user's principal, so the principal
alone cannot tell it from a typed one. Its journal entry also records that it came through the assistant, and the words
of the turn that led to it.

- **The mark cannot be forged.** `session::Context` is reflected whole onto the wire, so a field on it would depend on
  every decode path clearing it. The origin stays off the wire: it is a server-only field of the run, exposed on the model
  strand through a second scoped thread-local beside `session::current()`. `recordIfAttached` copies it into a new
  `journal::LogEntry` field, with the entry's JSON form and its spec updated. A model that appends its own entries, as
  LIMS's `SelfJournal` does, bypasses `recordIfAttached`, so it has to read the origin too, or its entries carry no mark.
- **A log must really attach, once.** `logProvider` attaches only when an envelope has a `contextKey`, which the
  interpreter never sends. `journal::setActionLog` covers every `BRIDGE_REGISTER_MODEL` type created after it is
  installed, but not a custom factory. In LIMS it would double-journal: `SampleModel` receives the log, arms
  `SelfJournal`, and appends its own entry while the dispatcher appends another for each `Loggable::Yes` action. So LIMS
  does not install it. The ignition model's registration attaches the log itself, and `SampleModel` is left as it is.
  Making an application-wide log safe beside self-journaling models is later work. An entry attached this way has an
  empty `entityKey` unless its registration supplies one, so the record has to be readable from the payload.
- **A journaled chat action.** An action the assistant runs is journaled unless it is a read or its registration opts out
  with `Loggable::No`. An action with effects that has opted out is logged at `warn` when the chat service starts, and
  the tool result says it was not journaled. It is not refused, because working-copy edits legitimately opt out and the
  later save is the journaled change.

**Conversations.** The service keeps one conversation per principal, screen and record. A different record starts a fresh
conversation, and returning to one continues it. A screen with no record has one conversation per principal and screen.
Retention is configured as an idle time limit, and a conversation also ends on the explicit end-of-chat.

**Tabs.** The panel follows the workspace's active tab, and a turn belongs to the tab it started on. Background tabs
holding the same data also receive the change, as they would from any other client.

**The proof screen.** A new LIMS ignition test screen whose actions store directly, are journaled, and declare effects
on a shared record, with a section for each test. It is built the way any LIMS screen is, with no assistant code in it.

**Provider seam.** A small interface for the model (messages and tools in, a cancellable stream of text and tool calls
out), selected in the application's server configuration. Nothing outside an implementation names a provider. With no
model provider configured, the chat is off and the panel does not show.

**The Qt panel.** A chat panel in the shell, for every frontend the shell supports, with streamed replies and a way to
cancel. `ShellFrame` is a sidebar, a head and a page with no third region, so this is new layout work.

**Outbound HTTPS.** The framework has no HTTP client, and `vcpkg.json` lists none. A provider implementation needs
one, kept behind the provider seam so core takes no new dependency.

## Constraints

- Coding rules in `AGENTS.md`, including the effects checklist in `docs/KEEPING-SCREENS-CURRENT.md` for any new action,
  and complete Doxygen on public symbols.
- `docs/spec/ui/agent.md` and `docs/spec/core/backend.md` are updated with any change they describe. A new spec is added
  for the chat service.
- Core must not learn about the UI or chat layer. Chat kinds stay behind `IEnvelopeService`.
- Tool results and record contents go to a model as text, so a record's contents can try to instruct it. The permission
  checks hold regardless, and the system prompt treats tool output as data.

## Proof

The chain needs testing as a chain, since each part passing alone proves little. No test today makes a `tools/call`
over a real pipe, or follows one to a screen rendering the change.

- A fake model provider scripted to call tools, driving a real server and a real session: "add a test" appears on a
  mounted screen through a pushed event, without a refresh declared anywhere.
- The same test with the action's effect removed must fail.
- A refused action is reported in the chat and changes nothing.
- A tool on a screen's private instance reaches that instance and not a new one.
- A context naming a record the person may not see gets nothing more than the same action from the GUI would. This runs
  under an authorizer that enforces ownership, because LIMS's `authorizeInstance` allows everything and would pass even
  with no check at all.
- The LIMS ignition test: both utterances from the experience above, through the fake provider, show on the mounted
  screen.
- A change through the chat has exactly one journal entry, with the assistant mark and its words. The same action from
  the GUI has one entry and no mark, and a client trying to send the mark does not get one.
- A destructive action run through the chat takes effect, is journaled and reaches a watching screen.
- A provider scripted to call tools forever stops at the cap, and the turn says so.
- Cancelling a turn and closing the connection each stop a running turn, and its provider call.
- Two records on one screen keep separate conversations, and the configured retention is honoured.
- A refused action leaves both the record and the journal unchanged, asserted by reading them.
- Tests of the screen wait for the `changes` event, not the turn's end: changes are coalesced for 50 ms, so a turn can
  finish before the screen updates.
- A dropped final event is recovered by reading the turn's state, and a reconnect during a turn leaves the conversation
  intact and the next turn on the new instance ids.
- Two instances of one model type on a screen are told apart by alias.
- With no provider configured, the panel does not show and the chat kinds refuse.
- The chat kinds' `authorize` gate refuses an unauthorised principal, end-of-chat stops a turn, the per-principal and
  per-connection caps hold, the opt-out warning fires, and a malformed tool call returns as a tool error.

## Left to the builder

- The names and shape of chat kinds and of the event topic.
- The provider interface's exact signature, and which HTTP client backs the first implementation.
- Which provider ships first. The owner's direction was flexibility; Claude is a sensible first model provider.
- The panel's layout within `ShellFrame`, which has not been designed in detail.
- The cap values and retention defaults.
- Order of work. Server seams and typed chat through a fake provider first, then a real provider, is this author's
  suggestion.

## Calls made by this document's author, not the owner

The owner handed these over.

- With no provider configured, chat is off.
- Voice and external MCP clients get their own specs, after typed chat.
- External clients will keep every `permitted` action rather than a narrowed list.
- A running turn can be cancelled, and ends when the connection closes.
- The proof is a new ignition test screen rather than LIMS's sample entry screen, whose working copy would not show a
  change applying at once.
- Conversations also end on an idle limit, because the server cannot see sign-out.
- An action that opted out of logging warns rather than being refused.
- The caps are in the design and destructive actions are not blocked.
- Executes run unscoped, so a running action cannot be interrupted by cancelling its turn.
- LIMS gets no application-wide log, so `SampleModel` keeps its present journal behaviour.

## Not verified

From reading the code; nothing was built or run for this design.

Settle these first, with tests, because each could change the seams above:

- That an unscoped execute carrying the client's session reaches a screen's private instance on a running server, and
  that the GUI gets the resulting event. This is traced through the code, and has not been run.
- That a registration can attach a log to the ignition model, giving exactly one entry per action on a screen's instance.
