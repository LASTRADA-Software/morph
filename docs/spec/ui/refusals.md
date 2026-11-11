# Action refusals — design

An action that is refused explains itself to the person in words, in their locale, and never with an exception text.
This page is the mechanism that makes that possible: a failure that carries a code and named arguments to the client,
and a message-catalogue lookup by a key that is known only when the failure arrives. The words themselves are the
application's, in its catalogues.

| Header | What it holds |
|---|---|
| `include/morph/core/action_refusal.hpp` | `morph::model::RefusalArg`, `morph::model::ActionRefusal` |
| `include/morph/core/wire.hpp` | the `err` envelope's `refusal` member (`wire::RefusalBody`), `wire::executeFailure` |
| `include/morph/ui/expression.hpp` | the `refusal` form, `t` with a key expression, the `refusalText` function |
| `include/morph/ui/author.hpp` | `refusal()` on query and mutation handles, `tAt`, `refusalText` |

## Contents

- [What a refusal is](#what-a-refusal-is)
- [How it travels](#how-it-travels)
- [How a document reads it](#how-a-document-reads-it)
- [The catalogue lookup by a run-time key](#the-catalogue-lookup-by-a-run-time-key)
- [`refusalText`](#refusaltext)
- [Load-time checks](#load-time-checks)
- [Wire compatibility](#wire-compatibility)
- [Invariants](#invariants)
- [Alternatives considered](#alternatives-considered)

## What a refusal is

`morph::model::ActionRefusal` is an exception deriving from `std::runtime_error`. It carries

- a **code**: a dotted name the application owns (`lims.billing.invoiceLeased`). It is the key the catalogue words,
  and so it is stable across releases of the wording;
- **named arguments**: `RefusalArg{name, value}` pairs, both strings (`{"holder", "alice"}`). They are the facts the
  sentence needs. A value that is a number or a date is written by the throwing side as the text the sentence should
  show, because the server does not know the client's locale;
- a **message** for logs and tests, the `what()` text, which the application writes for a developer ("SubmitInvoice:
  invoice 7 is being edited by 'alice'").

The type is named `ActionRefusal` and not `Refusal` because `morph::ui::Refusal` already names why a bundle or a
screen was not accepted at load, a different thing a reader must not confuse with an action's.

The wire carries a failure as one `message` string. A screen cannot word that text: it is written in one language, with
action names and ids in it, and the server cannot word it in the client's locale because it does not know it. The
mechanism therefore sends the *facts* (the code and the arguments) and leaves the *words* to the client.

## How it travels

**Server.** `RemoteServer` settles an execute from the exception the dispatcher hands it. When that exception is an
`ActionRefusal` (any type deriving from it), the `err` reply carries `refusal: {code, args}` beside the message. The
journal records the message as it does for any failed action. An exception that is not an `ActionRefusal` yields an
`err` with no `refusal` member.

An action guard's refusal travels the same way: a guard that throws an `ActionRefusal` is rethrown unchanged through
`runGuardJournaled` on every execution site, so the `err` carries the code and arguments exactly as for a throw from
`Model::execute`.

**Client over a connection.** `wire::executeFailure(envelope, message)` builds the exception a pending execute is
settled with: an `ActionRefusal` (code, arguments and the envelope's message) when the `err` carries a `refusal`, else
the `std::runtime_error` of today. `net::SocketBackend`, `QtWebSocketBackend` and `SimulatedRemoteBackend` all use it,
so a refusal reaches a completion as the same type whichever transport carried it.

**In process.** `Bridge::localOp` and `localTaskOp` reject the completion with the exception the model or its guard
threw, unchanged, so the completion holds the `ActionRefusal` itself.

The interpreter reads a failed completion's exception the same way on every path: if it is an `ActionRefusal`, the
mutation's or query's refusal state holds `{code, args}`; otherwise it holds `null`.

## How a document reads it

`{"refusal": "m"}` reads a mutation's or a query's refusal: `null` when the last run did not fail with an
`ActionRefusal` (or has not failed), otherwise a record `{"code": "...", "args": [{"name": "...", "value": "..."}]}`.
It is set and cleared exactly as `error` is: a successful run clears it, and a run superseded under `latest` does not
write it. The author API is `refusal()` on `MutationHandle` and `QueryHandle`.

`errorKind` is unchanged: a refusal is kind `error` like any other failure of the model. A document that wants to
branch on a refusal tests `engaged(refusal)`.

`refusalText(refusal)` is the words: see [`refusalText`](#refusaltext). The `error` text is the exception's `what()`
and a screen never shows it for a refusal.

## The catalogue lookup by a run-time key

`t`'s key is a literal string in the document, so a message whose key is known only when a failure arrives cannot be
written. The `t` form therefore accepts, in place of its literal key, `{"expr": <expression>}` evaluating to a string:

```json
{ "t": { "expr": { "ref": "code" } }, "fallback": "Refused.", "args": { "holder": { "ref": "who" } } }
```

(`tAt(key, fallback, args)` in the author API.) The wrapper is needed because a bare object in `t`'s key position is
otherwise an error, and it keeps the literal form unchanged.

`args` may be a record of expressions, as today, or `{"expr": <expression>}` evaluating to a **list of `{name, value}`
records**, which becomes the message's named arguments. A record of expressions whose only field is named `expr` is
therefore read as the wrapper, not as a field; an application that needs an argument called `expr` uses the list form.
An argument named `holder` is the MessageFormat 2 variable `$holder`; its value is a string, formatted as text. No
`:number` or `:date` is applied to it: a pattern that wants one names it (`{$amount :number}`), which then formats the
string only when it reads as a number, and shows `{$amount}` with an error otherwise. A list element that is not a
record with a string `name`, or whose `value` is not a string, is a type error at run time: reported, and the element
left out.

The fallback stays a literal string: it is checked at load as it is today.

## `refusalText`

`refusalText(refusal)` yields the sentence a screen shows, built from the refusal's code and arguments:

1. the catalogue's message for the **code**, formatted with the arguments;
2. else the **class sentence**: the message for the first of the code's class keys that the catalogue has. The class
   keys are each dotted prefix of the code, longest first, each followed by `.generic`; for
   `lims.billing.invoiceLeased` they are `lims.billing.generic`, then `lims.generic`. morph knows no prefix by name; the
   keys follow from the code alone, so an application organises its sentences by whatever prefixes it uses;
3. else the interpreter's built-in sentence, `This was refused.`.

A `null` refusal yields `null`, as every function of `expr/1` does for a `null` argument, so a screen may bind the text
unconditionally; it shows nothing until a refusal arrives.
The exception's message is never shown: it is for logs.

The code's arguments are given to whichever message is chosen, the class sentence included, so a `lims.generic` that
says "Held by {$holder}" works for every code that carries `holder`.

Resolution uses the session's locale and provider like `t`, so it re-evaluates when the locale changes.

## Load-time checks

A literal `t` key is checked as the bundle checks it today. A run-time key cannot be: the bundle does not know which
codes a server sends. The interpreter therefore reports a **missing run-time key** as a problem (the diagnostics a client
logs and `ScreenHarness::problems()` collects), once per key and screen (the interpreter keeps its diagnostics per mounted screen). For `refusalText` the report is for the
refusal's *code* being absent from the catalogue, whether or not a class sentence then stood in; a class key being
absent is not reported, since the chain is meant to fall through. An application covers its codes with a test of its own
over its catalogues; the interpreter's report is the backstop for a code the test missed.

## Wire compatibility

The `refusal` member is omitted when absent, as every default-valued member is, so an `err` for a failure that is not a
refusal is byte-for-byte what it was. A client that does not know the member ignores it, because `decode` reads with
`error_on_unknown_keys = false`, and shows the message as before. A server that does not send it yields a plain
`runtime_error`, and `refusal` reads `null`. No protocol version changes.

## Invariants

- A thrown `ActionRefusal` reaches the client as an `ActionRefusal` with the same code and arguments, on every path.
- A thrown exception that is not an `ActionRefusal` never produces a `refusal` member.
- The message of a refusal is never an input to the sentence a screen shows.
- `refusalText` makes no catalogue lookup of its own beyond the code and its class keys, and invents no wording but the
  built-in sentence.

## Alternatives considered

- **Wording on the server in the client's locale.** The server would need the locale per call, and the words would live
  in two places (the catalogue the client holds and the server's code).
- **A status-code enum in morph.** The codes are the application's domain words; morph would have to name them.
- **Parsing the message.** Fragile, and the message is in one language.
- **A load-time list of codes per bundle.** The server's codes are the application's and change without the bundle, so
  the list would be stale in exactly the case it exists for.
- **A new error kind for refusals.** It would change what every existing `errorKind` comparison sees for an exception
  that is now an `ActionRefusal`; the separate `refusal` state gives the same branch without that.
