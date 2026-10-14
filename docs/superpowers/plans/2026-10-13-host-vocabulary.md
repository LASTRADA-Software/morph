# The host vocabulary: implementation plan (part 8, slice b2, #893)

**Goal.** An application registers the commands and values only it has (restart, open a path, a count its own sync
engine keeps), typed and versioned. A document runs a command with validated arguments, reads a value reactively,
asks `{"can": "host:name"}`, and an unsafe command never runs from a document that came over an unverified
connection. Part of #893.

**Specs.** Spec 6 (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`) §14 and the §12 "Host vocabulary"
tests; spec 5 (`2026-10-07-ui-document-design.md`) §12 (the handshake) and §13 (trust). In-tree:
`include/morph/ui/interpreter.hpp` (`InterpreterHost::hostCommand`, `Session::setHostValues`, `{"can": …}`,
`runHostCommand`), `include/morph/ui/bundle.hpp` (`HelloRequest::hostCommands`, `hostValues`),
`include/morph/ui/frontend.hpp` (`AppSource`).

## What planning found

1. **The document half exists.** `{"host": "name", "args": e}` parses and calls `InterpreterHost::hostCommand`,
   which answers only whether the command exists; `{"host": "name"}` reads `Session`'s host values, a reactive
   signal the application fills with `setHostValues`; `HelloRequest` carries host command and value names with
   versions. What is missing is a registry with types, the argument check, the trust gate, and a `can` that answers
   from the client's own commands.
2. **`{"can": "host:name"}` reads the server's permitted set.** `Session::can` answers from `setPermitted`, which
   the catalog reply fills, and the doc comment asks the host to merge `host:` names into it. The two sets have
   different owners (the server's authoriser and the client's registry), and a catalog refresh after `signIn`
   would drop the host's names. `host:` names get their own set.
3. **Nothing knows whether the connection is verified.** No type in `morph::ui` records it; the transport and the
   `AppSource` know. The smallest seam is a flag on the registry, `setConnectionVerified(bool)`, set by the
   application when its source opens, and `false` until then: an unsafe command is refused unless the application
   says otherwise.
4. **`switchBackend` already refuses an unlisted backend.** The interpreter reports `backend `x` is not listed`
   when `InterpreterHost::switchBackend` answers `false`, and `test_ui_interpreter_commands.cpp` covers it.
   `AppSource` has no `backends()` list yet; listing backends and switching them is the backend switch's (spec 6
   §9), not this slice's.
5. **The registry belongs in `morph::ui`, not `qt_quick`.** Spec 6 §14 spells it `qt_quick::HostCommands::add`, but
   the terminal client runs the same documents and needs the same commands; nothing in it needs a toolkit. A Qt
   spelling would only forward, so none is added; the spec is corrected.

## Design

### The session's seam (`interpreter.hpp`, the host regions)

- `HostCommandResult { Ran | Missing | Refused, reason }` and `HostCommands { available, run }`.
- `Session::setHostCommands(HostCommands)`: `available` is a reactive set of the command names the client can run
  now; `{"can": "host:name"}` reads it (and no longer the permitted set). `run` runs a command and says what
  happened.
- `runHostCommand`: with a session runner, `Missing` reports `host command `x` is missing` and `Refused` reports the
  runner's reason, each once per path and reason as every diagnostic; neither runs the command. Without a runner the
  interpreter asks `InterpreterHost::hostCommand` as now.

### The registry (`ui/host_vocabulary.hpp`)

```cpp
struct HostArg { std::string name; std::string type; };          // a document type: "string", "int?", …
struct HostCommand { std::string name; std::int64_t version = 1; std::vector<HostArg> args;
                     bool safeForUntrusted = false; std::function<void(ValueRecord const&)> run; };
struct HostValue { std::string name; std::int64_t version = 1; std::string type; Value initial; };
class HostVocabulary {
    void add(HostCommand); void add(HostValue);          // refused: a bad name or type, a duplicate, no `run`
    bool set(std::string const& name, Value value);     // a declared value, of its type
    void setConnectionVerified(bool verified);
    void connect(Session& session);                     // values, `can` names and the runner
    void describe(HelloRequest& hello) const;           // names and versions for `ui-hello`
};
```

- **Arguments.** `args` evaluates to a record (or `null` for none). Every declared argument is checked with
  `Type::admits`; an absent one is `null`, which only an optional type admits; an argument the command does not
  declare is refused. A refusal names the argument and the types, and the command does not run.
- **Trust.** `safeForUntrusted: false` runs only while the connection is verified; otherwise `Refused` with
  `host command `x` needs a verified connection`, and `can` is false for it.
- **Values.** `set` checks the type and pushes the whole record into the session's signal, so a binding updates; a
  value nobody declared reads `null`, as every absent host value does.
- **Owner.** Every call is made on the runtime's owner, where the session lives.

## Tasks (one commit each)

1. This plan.
2. The session seam in `interpreter.hpp`: `HostCommands`, `setHostCommands`, `can` for `host:` names, and
   `runHostCommand` through the runner.
3. `ui/host_vocabulary.hpp` and its tests: the §12 "Host vocabulary" cases through a mounted screen, registration
   refusals, and `describe` filling `HelloRequest`.
4. `docs/spec/ui/host_vocabulary.md` (new), `docs/spec/ui/interpreter.md`, spec 6 §14 (the registry's namespace
   and shape, the verified flag), CHANGELOG, CMake FILE_SET.

## Verification

- Mutations, each checked against a freshly linked binary: skip the argument check; skip the trust gate; answer
  `can` from the permitted set again. Each must fail a test.
- GCC Debug `morph_tests` in full; clang-tidy on the changed lines; Doxygen with `WARN_AS_ERROR`; clang-format on
  every touched file.
