# bank on server-defined screens: implementation plan (part 12, #897)

**Goal.** Spec 4 §5's bank:
- the screens are login, then accounts, move money, cards, payees and loans behind a sidebar;
- account cards, the transfer flow and the loan schedule are custom components with built-in fallbacks;
- the A/B stale-selection ordering gets a screen test;
- the browser demo runs in local mode over the in-memory models.

**Specs.** Spec 4 §1, §5 (bank), §7, §8 item 3; spec 6 §5 (sign-in). In-tree: `docs/spec/ui/author.md`,
`docs/spec/ui/interpreter.md`.

## What planning found

1. **The part is two slices.**
   - **(a)** The screens, the client's sign-in and the retired desktop QML GUI.
   - **(b)** The browser demo. The generic client and the Qt Quick renderer are not built under Emscripten today
     (`MORPH_BUILD_EXAMPLES AND NOT EMSCRIPTEN`), and `morph/table` does not compile against Emscripten's libc++
     (#953). Slice (b) starts once that is fixed.
2. **The generic client cannot sign in.** The interpreter's `signIn` command asks the application's
   `InterpreterHost`, and the client shell passes none.
   - The shell answers `signIn` by adopting the reply's `principal` into the source's request session, which is
     the bridge's default session in local mode.
   - `signOut` returns to the source's development user.
   - Remote mode needs a token the server issues, which bank's `AuthResult` does not carry, so it answers
     "not supported".
3. **The sidebar is the screens' own.** The client shell mounts one screen at a time. Each bank screen is a row of
   the sidebar, a composition in `bank_screens.cpp`, and the page. A workspace with a navigator (#893) can replace
   it later.
4. **Amounts.** bank's money actions take minor units (`amountMinor`), and no expression parses text into a
   number. The money actions are `form` nodes, and the forms engine turns the text into an integer. The field is
   labelled in cents. A `parse` function in the expression vocabulary would let a text input feed it directly.

## Slice (a)

- `examples/common`:
  - `ClientSource::adoptPrincipal(principal)`;
  - the client shell's `InterpreterHost` answers `signIn` and `signOut`;
  - the screen harness gets the same.
- `examples/bank/screens/` (`bank_screens`, linking `bank_lib`): `bank.login`, `bank.accounts`, `bank.move`,
  `bank.cards`, `bank.payees`, `bank.loans`.
  - The sidebar navigates between them and signs out.
  - `AccountCard`, `TransferFlow` and `LoanSchedule` are host components with fallbacks.
- `morph_examples_client --app bank` (`--db`).
- The screen tests:
  - sign-in, then the accounts of the signed-in user;
  - open an account and deposit;
  - the A/B selection: choose B, deposit, and B is still chosen and holds the money;
  - transfer, payees, cards, a loan and its schedule.
- A render smoke over every screen.
- The desktop GUI (`gui/`) stays for now: `gui_wasm/` compiles its controllers and QML verbatim, so both retire
  together in slice (b).

What building slice (a) found:

- **A mutation's `result` is read through a `let`.** A field cannot be read below a mutation's result directly, so
  the login screen names each reply.
- **`blankAs` does not make a field optional.** Each transaction DTO declares its description optional
  (`optionalFields`), which is what its validator already allowed.
- **CI ran none of the non-rung screen tests.** Their labels did not match `ctest -L ladder`, and the ladder job
  built no Qt Quick, so no render smoke ran. Both are fixed on #949's branch, which this stacks on.

## Verification

Per slice:
- GCC full;
- the screen tests and the render smoke;
- each test mutated against a fresh binary;
- clang-tidy on changed lines with the file list split;
- clang 21 `-Wdocumentation` on changed headers.
