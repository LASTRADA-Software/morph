# The host vocabulary — design

An application has commands and values no document can define: restart the process, open a path in the operating
system, a count the application's own sync engine keeps. The client registers them, named, typed and versioned, in a
`HostVocabulary`; a document runs a command with `{"host": name, "args": e}`, reads a value with `{"host": name}`,
and asks `{"can": "host:name"}` before it shows a control. This implements spec 6 §14 (`host/1`,
`docs/superpowers/specs/2026-10-08-app-hosting-design.md`) and the host half of spec 5 §13.

| Header | What it holds |
|---|---|
| `include/morph/ui/host_vocabulary.hpp` | `HostArg`, `HostCommand`, `HostValue`, `HostVocabulary` |
| `include/morph/ui/interpreter.hpp` | The session's seam: `HostCommandResult`, `HostCommands`, `Session::setHostCommands` |

## Contents

- [Registration](#registration)
- [Running a command](#running-a-command)
- [Trust](#trust)
- [Values](#values)
- [`can`](#can)
- [The handshake](#the-handshake)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## Registration

```cpp
morph::ui::HostVocabulary host;
host.add(HostCommand{ .name = "restartApp", .version = 1, .args = {}, .safeForUntrusted = false, .run = restart });
host.add(HostCommand{ .name = "openPath", .version = 1, .args = { {"path", "string"} },
                      .safeForUntrusted = false, .run = openPath });
host.add(HostValue{ .name = "sync.needsYouCount", .version = 1, .type = "int", .initial = Value{std::int64_t{0}} });
host.connect(session);
```

- An argument's and a value's type is a document type (`parseType`): `string`, `int?`, `list<string>`, …
- Refused with `std::invalid_argument`: an empty or overlong name, a name registered twice, a command with no `run`,
  an argument with an empty or repeated name, a type that does not parse, an initial value not of its type.
- The vocabulary lives on the runtime's owner, like the session it is connected to, and outlives it. Registering
  after `connect` reaches the session too.

## Running a command

`{"host": "openPath", "args": {"obj": {"path": e}}}`: `args` is an expression, so a record is written with `obj`;
it evaluates to a record, or to `null` when the document gives none.

1. A command nobody registered is **missing**: nothing runs, and the interpreter reports
   `host command `x` is missing`, once per command path, as every diagnostic.
2. An unsafe command on a connection that is not verified is **refused** (below).
3. Every declared argument is checked with `Type::admits`. An absent argument is `null`, which only an optional type
   admits; an argument the command does not declare, or `args` that is not a record, is refused. The refusal names
   the argument and the type.
4. Only then does `run` get the checked record, every declared argument present (`null` for an absent optional one).

A refusal is reported and the command does not run; the command answers as one that did not succeed. The
application's `InterpreterHost::hostCommand` is not asked: a session with a runner
(`Session::setHostCommands`) answers every host command itself, and a session without one asks the host as before.

## Trust

A command marked `safeForUntrusted: false` runs only from a document that came over a verified connection: a
verified TLS connection, or loopback with the development flag (spec 5 §13). No type in `morph::ui` observes the
connection, so the application says so: `setConnectionVerified(true)` once its source opened over such a
connection. It starts `false`, so until the application says otherwise an unsafe command is refused with
`host command `x` needs a verified connection`, and `{"can": "host:x"}` is false. A value is read-only and needs no
gate.

## Values

`{"host": "name"}` reads the session's host values, a reactive signal, so a binding on one updates when
`HostVocabulary::set(name, value)` changes it. `set` refuses a value nobody registered or one not of the declared
type, and changes nothing then. A value the client does not have reads as `null`, so a document guards with
`coalesce`.

## `can`

`{"can": "host:name"}` is true when the client can run the command now: it is registered and, if unsafe, the
connection is verified. It is answered from `Session::setHostCommands`, never from the server's permitted set
(`setPermitted`): that set is the server's answer about actions, refreshed after `signIn` and after a `denied` reply,
and the client's commands are not the server's to grant or drop.

## The handshake

`describe(hello)` fills `HelloRequest::hostCommands` and `hostValues` with every registered name and its version,
unsafe commands included: the handshake says what the client has, and the trust gate decides per run.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Where the registry lives | `morph::ui`, no toolkit | The terminal client runs the same documents; a Qt spelling would only forward. |
| How the interpreter reaches it | A runner on the `Session` | The session already holds the host values; one seam, no new `InterpreterHost` hook. |
| Who knows the connection is verified | The application, through `setConnectionVerified` | Nothing in `morph::ui` observes the transport; the default refuses. |
| `can` for an unsafe command while unverified | `false` | A document shows a control only where it works. |
| How values update | `set` pushes into the session's signal | A binding updates without the vocabulary knowing the reactive graph. |

## Not yet

- **`switchBackend`** is not a host command. The interpreter already refuses a backend the application's
  `InterpreterHost::switchBackend` does not list; `AppSource` listing its backends is the backend switch's
  (spec 6 §9).
