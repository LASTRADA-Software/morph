# Reshaping `examples/bank` onto stateful models — planned

**Status:** planned, not implemented. This document is a design proposal, not a
description of current behaviour. The authoritative present-tense specs are in
[`docs/spec/`](../spec).

## Contents

- [The gap this closes](#the-gap-this-closes)
- [Why the current bank cannot demonstrate morph](#why-the-current-bank-cannot-demonstrate-morph)
- [The reshaped model set](#the-reshaped-model-set)
- [`AccountModel` — the worked example](#accountmodel--the-worked-example)
- [`CustomerModel` — the per-user repository](#customermodel--the-per-user-repository)
- [`LedgerModel` — where cross-instance atomicity lives](#ledgermodel--where-cross-instance-atomicity-lives)
- [Hydration, write-through, and deactivation](#hydration-write-through-and-deactivation)
- [What the GUI stops doing](#what-the-gui-stops-doing)
- [The WASM build](#the-wasm-build)
- [Migration order](#migration-order)
- [Design decisions](#design-decisions)
- [Failure modes](#failure-modes)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## The gap this closes

morph's central claim is in the README's first paragraphs: *you write the model
as plain, single-threaded C++, and the framework owns concurrency — one strand
per model instance serialises that model's calls, so model authors never touch a
mutex.*

`examples/bank` is the library's largest worked example and the one a reader
reaches for to see that claim in action. It does not demonstrate it. Every bank
model is **stateless**: the only member any of them declares is the
`std::optional<Lightweight::DataMapper>` inherited from
`bank::db::WithMapper` — a database connection, not domain state.

That means the per-model strand protects nothing. There is no state to
serialise access to, no state to keep in memory, and no state that a second
handler could usefully share. Every action is a full round trip to SQLite, so
the example demonstrates the *bridge* while leaving morph's model layer looking
like a thin RPC shim over a database.

This document proposes reshaping the bank so its models hold the state they are
named after. It is a prerequisite for
[shared_model_instances.md](shared_model_instances.md) and
[instance_subscriptions.md](instance_subscriptions.md) having any demonstrable
effect: a primary key identifies nothing when instances carry nothing, and
sharing an instance preserves nothing when there is nothing to preserve.

## Why the current bank cannot demonstrate morph

Concretely, today:

```cpp
class AccountModel : private db::WithMapper {
public:
    dto::AccountInfo   execute(const dto::OpenAccount&);
    dto::AccountList   execute(const dto::ListAccounts&);
    dto::AccountInfo   execute(const dto::GetAccount&);
    dto::CommandResult execute(const dto::CloseAccount&);
};
```

One `AccountModel` answers for **every** account of **every** user. Its
identity is nothing; its state is nothing. Three consequences follow, all
visible in the shipped GUI:

- **Five instances, five connections.** `AccountController`,
  `TransactionController`, `LoanController`, `CardController` and
  `PayeeController` each construct a `BridgeHandler<bank::AccountModel>`. Since
  a handler registers one instance ([bridge.md](../spec/core/bridge.md)), the
  desktop GUI holds five `AccountModel` instances and therefore opens five
  SQLite connections for what is logically one thing.
- **Reads cost a query.** `GetAccount` re-selects a row the process may have
  read a millisecond earlier, because nothing retains it.
- **The strand is decorative.** Its documented purpose is to let a model own
  mutable state without locking. No bank model has any.

## The reshaped model set

The reshape splits today's per-*domain* models into models keyed by the entity
they are actually about. Each keyed model type declares its key as a nested
alias, detected structurally (see
[shared_model_instances.md](shared_model_instances.md)).

| Model | Key | In-memory state | Actions |
|---|---|---|---|
| `AccountModel` | `AccountId` (account row id) | one `AccountRecord`: balance, status, overdraft, currency, kind | `GetAccount`, `Deposit`, `Withdraw`, `CloseAccount` |
| `CustomerModel` | `UserId` (owner) | the customer row + their account id list | `ListAccounts`, `OpenAccount` |
| `LedgerModel` | *(unkeyed)* | none — owns the atomic write | `Transfer`, `History` |
| `AuthModel` | *(unkeyed)* | none | `Login`, `Logout` |

`LoanModel`, `CardModel`, `PayeeModel`, `PaymentModel`, `StatementModel`,
`BudgetModel` and `NotificationModel` keep their current shape in the first
pass; see [Migration order](#migration-order).

The split is the point: `ListAccounts` was never an account-scoped operation —
it is scoped by *user*, which is why the current single model has to take an
`owner` field on half its actions. Once `AccountModel` is keyed by account,
those fields disappear, because the instance already knows which account it is.

## `AccountModel` — the worked example

```cpp
namespace bank {

/// One customer account, held in memory for the lifetime of the instance.
class AccountModel : private db::WithMapper {
public:
    /// The primary key type. Detected structurally by morph; declaring it is
    /// what makes this model keyed.
    using PrimaryKey = std::int64_t;

    dto::AccountInfo   execute(const dto::GetAccount&);
    dto::AccountInfo   execute(const dto::Deposit&);
    dto::AccountInfo   execute(const dto::Withdraw&);
    dto::CommandResult execute(const dto::CloseAccount&);

private:
    void hydrate();                  ///< load `_row` from SQLite on first use
    void writeThrough();             ///< persist `_row` after a mutation

    db::AccountRecord _row{};        ///< the account — in memory, not re-queried
    bool _loaded = false;
};

}  // namespace bank
```

The action DTOs lose the id fields that only existed to say *which* account:

```cpp
// before                                  // after
struct GetAccount { std::int64_t id; };    struct GetAccount {};
struct Deposit { std::int64_t accountId;   struct Deposit { std::int64_t amountMinor; };
                 std::int64_t amountMinor; };
struct CloseAccount { std::int64_t id; };  struct CloseAccount {};
```

and the key is instead declared once per action, naming the field that carries
it — or, for actions that no longer carry one, nothing at all, in which case the
action runs on whichever instance the handler is already attached to:

```cpp
BRIDGE_REGISTER_MODEL(AccountModel, "AccountModel")
BRIDGE_REGISTER_ACTION(AccountModel, GetAccount, "GetAccount", Loggable::No)
BRIDGE_REGISTER_ACTION(AccountModel, Deposit,    "Deposit")
BRIDGE_REGISTER_ACTION(AccountModel, Withdraw,   "Withdraw")
BRIDGE_REGISTER_ACTION(AccountModel, CloseAccount, "CloseAccount")
```

The GUI attaches by key and then stops mentioning ids:

```cpp
BridgeHandler<AccountModel, AllowShared> account{bridge, gui};

account.attach(42);                        // or: any keyed action re-points it
account.execute(Deposit{.amountMinor = 5000})
    .then([](AccountInfo a) { /* a.balanceMinor is authoritative, from memory */ });
```

`Deposit` now reads and writes `_row.balanceMinor` directly. The overdraft check
that today re-selects the row is a field comparison. The strand that morph has
always provided is now load-bearing: it is what makes the unlocked
read-modify-write of `_row` correct.

## `CustomerModel` — the per-user repository

```cpp
class CustomerModel : private db::WithMapper {
public:
    using PrimaryKey = std::int64_t;       // user id

    dto::AccountList execute(const dto::ListAccounts&);   // no `owner` field
    dto::AccountInfo execute(const dto::OpenAccount&);    // no `owner` field
};
```

`OpenAccount` is the *creating* action: it inserts a row and its result carries
the new id. That is the result-sourced key case in
[shared_model_instances.md](shared_model_instances.md) — a handler can adopt the
new account's key straight from the result, exactly as a database insert
returns its generated primary key:

```cpp
BRIDGE_KEY_FROM_RESULT(OpenAccount, &dto::AccountInfo::id)
```

`CustomerModel`'s key comes from the authenticated principal rather than an
action field. The first pass resolves it explicitly at login
(`customer.attach(session.userId)`); making the session principal a first-class
key source is deliberately **not** part of this work — see
[Limitations](#limitations).

## `LedgerModel` — where cross-instance atomicity lives

`Transfer` moves money between two accounts, so with per-account instances it
touches two models. morph has no cross-instance transaction and this proposal
does not add one — consistent with the framework's standing position that
conflict resolution and multi-entity consistency are domain concerns
([ARCHITECTURE.md](../ARCHITECTURE.md), "Conflict Resolution — a domain concern,
not a framework concern").

`Transfer` therefore stays on an unkeyed `LedgerModel` which owns the
`SqlTransaction` that debits one row and credits the other atomically, exactly
as today. The consequence is explicit and must be documented in the example's
README: **after a transfer, any live `AccountModel` instance for either account
holds a stale balance.** The first pass resolves this the blunt way — the ledger
marks both instances dirty and they re-hydrate on their next action.
[instance_subscriptions.md](instance_subscriptions.md) is what would let the GUI
learn about it without asking.

This is the sharpest honest edge of the whole design, and the example should
show it rather than arrange the domain to avoid it.

## Hydration, write-through, and deactivation

- **Hydration is lazy and on-strand.** The first action on an instance loads its
  row, on the strand thread, mirroring how `WithMapper` already defers opening
  the `DataMapper`. A key naming a row that does not exist fails that action
  with `NotFound`; the instance is not retained.
- **Writes are write-through, not write-behind.** A mutating action updates
  `_row` and persists it before returning. This keeps SQLite authoritative, so a
  crash loses nothing and a deactivated instance can always be reconstructed.
  Write-behind would be faster and is explicitly out of scope: it would make the
  in-memory copy authoritative and demand a durability story the example has no
  business inventing.
- **Deactivation just drops memory.** Releasing an instance discards `_row`; the
  database is unchanged. Re-attaching re-hydrates. Nothing in the example
  depends on an instance surviving.

## What the GUI stops doing

The five `BridgeHandler<AccountModel>` become `AllowShared` handlers attached to
the account the user is looking at, so the desktop GUI holds one instance per
*viewed account* rather than one per *controller*. `TransactionController` and
`LoanController` stop constructing their own `AccountModel` purely to re-list
accounts; they attach to `CustomerModel` instead.

`AccountController::refresh()`'s `ListAccounts` round trip after every mutation
(`AccountController.cpp:63`, `TransactionController.cpp:97`) is not removed by
this work — that is the invalidation problem, out of scope here — but it becomes
cheaper, because the balance the GUI re-reads comes from memory.

## The WASM build

`examples/bank/gui_wasm` carries shadow model headers and an in-memory store
(`gui_wasm/include/bank/wasm/store.hpp`) that reimplement every model against a
non-SQLite backing. Those shadows must be reshaped in the same commit, or the
WASM demo silently diverges from the desktop one.

The reshape is *easier* there: a stateful model over an in-memory store is
closer to what the WASM shadows already are. This is a good forcing function —
if the reshaped model is awkward to express against a plain in-memory store, the
model is carrying persistence concerns it should not.

## Migration order

1. `AccountModel` + `CustomerModel` + `LedgerModel`, desktop only. This is the
   whole idea; everything after it is repetition.
2. The `gui_wasm` shadows for the same three.
3. `examples/bank/tests` — the per-model tests become per-instance tests, which
   is where the state actually gets asserted.
4. `LoanModel` and `CardModel` (keyed by loan / card id), same pattern.
5. `PayeeModel`, `PaymentModel`, `StatementModel`, `BudgetModel`,
   `NotificationModel` — keyed by owner, i.e. `CustomerModel`-shaped.
6. `examples/bank/README.md`, whose "Architecture: two type layers" section
   describes the stateless shape and must be rewritten.

Steps 1–3 are the deliverable; 4–6 can follow independently.

## Design decisions

- **Split by entity, not by domain.** The current models are named for domains
  (`AccountModel` handles all accounts). Keying them by the entity they are
  named after is what gives the key something to identify. The `owner` fields
  scattered across today's DTOs are the symptom of the missing split.
- **SQLite stays authoritative.** The model holds a cache with identity, not a
  system of record. This keeps the example honest about what morph does and does
  not own, and keeps `journal`'s replay semantics
  ([journal.md](../spec/journal/journal.md)) unchanged.
- **`Transfer` stays on an unkeyed model.** Making it a cross-instance operation
  would require inventing cross-strand atomicity, which morph does not have and
  which this example must not imply it has.
- **No session-sourced keys in this pass.** `CustomerModel` attaching from an
  explicit user id keeps the key mechanism to one concept.

## Failure modes

- **A key naming a non-existent row.** Hydration fails, the action completes
  through `onError` with `NotFound`, and no instance is retained. It must not
  leave a half-hydrated instance in the directory.
- **A stale `AccountModel` after `Transfer`.** Documented above and visible in
  the example by design. The dirty-and-re-hydrate mitigation must be an
  explicit, commented mechanism, not an accident of timing.
- **Two instances for the same account** — impossible for `AllowShared` handlers
  (the directory guarantees one per key) but expected for plain handlers, which
  keep today's isolated-instance behaviour. Two isolated instances of the same
  account both write through to the same row, so the last writer wins. The
  example should use `AllowShared` throughout and say why.

## Limitations

- **The session principal is not a key source.** `CustomerModel` must be
  attached explicitly after login. Deriving a key from the authenticated
  principal is a plausible follow-up, not part of this work.
- **No cross-instance transaction.** Stated above; a domain concern by design.
- **Write-through only.** No batching, no write-behind, no dirty-flush policy.
- **The first pass leaves six models unreshaped**, so the example is
  temporarily mixed-paradigm. The README must say which models are which rather
  than let a reader infer that the un-migrated ones are the intended pattern.

## Cross-references

- [shared_model_instances.md](shared_model_instances.md) — the keyed-instance
  mechanism this example is the demonstrator for.
- [instance_subscriptions.md](instance_subscriptions.md) — how a GUI learns that
  a shared instance changed.
- [bridge.md](../spec/core/bridge.md) — `BridgeHandler`, `HandlerBinding`, and
  the one-instance-per-handler rule this reshape works within.
- [registry.md](../spec/core/registry.md) — `BRIDGE_REGISTER_*`, model factories,
  and the default-constructibility requirement for remotely instantiated models.
- [journal.md](../spec/journal/journal.md) — `contextKey` as an entity key, which
  a keyed model supplies naturally.
- [ARCHITECTURE.md](../ARCHITECTURE.md) — "Conflict Resolution — a domain
  concern, not a framework concern".
