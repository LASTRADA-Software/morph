# kanban and ledger on server-defined screens (part 14, #899)

**Goal.** Spec 4 §5's kanban and ledger as UI documents, each with screen tests on the harness, a render smoke over
every screen, and an entry in the generic client's local mode.

**Specs.** Spec 4 §5 (kanban, ledger), §7.

## What building found

- **A `serialKey` cannot read the run it keys**, so kanban's moves cannot be serialized per task: load-time
  validation refuses `event` in it. Filed as #956. The board uses one serial queue meanwhile.
- **A form's submit does not trigger `refreshOn`** (#950), so kanban's column, swimlane and task forms refetch the
  board themselves.
- **A gated-off query drops its value.** ledger's status poll closes its gate once the job is done or failed, and
  the status went with it. The last status is kept in a state of its own.
- **The harness's clock is manual.** A query's `refreshEvery` fires only when the scheduler is advanced, so the
  harness gains `advance(delta)`.
- **`RunReportJob` belongs to the server's report runner,** a system principal, so the screen requests a statement
  and polls it. The tests run the job as the runner does. Local mode has no runner, so a requested statement stays
  pending there.

## Not in this part

- kanban's attachments over the file side channel, and the offline queue and network monitor.
- ledger's transactions, budgets and rules beyond the ledger, its accounts and the statement job.
- Each rung's `gui/`, `gui_lib/` and `gui_wasm/`, which #900 retires.

## Verification

Per rung:
- the screen tests and the render smoke;
- each test mutated against a fresh binary;
- clang-tidy on the changed lines, headers included.
