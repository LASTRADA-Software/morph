# forge — rung 8 of the [application ladder](../LADDER.md)

**Status: design annex** ([round-7 program decision](../LADDER.md)) — this
README is the deliverable; the rung's *framework* content (polling at
500–2,000 sockets, unbounded notification instances, epoch resync,
hardened-config latency) ships earlier as the **forge load script against
synthetic models**; building the product phases is a post-rung-4 decision.
A software forge — the GitLab class: organizations,
teams, repositories, issues, labels, milestones, notifications, wiki, pull
requests with reviews, webhooks, CI status. The ladder's ceiling: every
subsystem and every known framework limit at once, at multi-client scale.

## Reference implementations

- **[Gitea](https://github.com/go-gitea/gitea) /
  [Forgejo](https://codeberg.org/forgejo/forgejo)** (Go, MIT) — the anchor.
  Decisive facts, verified:
  - **SQLite is a first-class supported database** — a full forge runs on
    morph's persistence tier.
  - Even Gitea's own UI **treats push as an optional enhancement over
    polling**: notification counts poll (SSE optional and distrusted, see
    [gitea#25661](https://github.com/go-gitea/gitea/issues/25661)), CI
    runners **poll** `FetchTask`
    ([#24543](https://github.com/go-gitea/gitea/issues/24543) to change that
    is still open), and the CI log view polls a JSON endpoint
    ([#33606](https://github.com/go-gitea/gitea/issues/33606)). A
    request/response-only forge is therefore *precedented*, not a
    compromise.
  - Architecture to study: layered monolith `routers → services → models
    (XORM) → modules`; background work behind a unified queue abstraction
    (persistable-channel/LevelDB — analogous to morph's SQLite offline
    queue); `hook_tasks` table for webhook delivery + retry.
    Overview: <https://docs.gitea.com/contributing/guidelines-backend>
   Note also: a `git push` over SSH **bypasses morph entirely**, yet repo
   viewers must see the new branch on their next poll — the post-receive
   hook needs the server-side internal-dispatch seam established in
   [`bookmarks`](../bookmarks); the drift test is "push via sidecar, assert
   a polling client converges."
- **[Gogs](https://github.com/gogs/gogs)** (Go, MIT) — Gitea's ancestor,
  deliberately minimal, single binary + SQLite: the best small-codebase read
  for "what is the true minimum forge".
- **[Zulip's events system](https://zulip.readthedocs.io/en/stable/subsystems/events-system.html)**
  — the notification transport blueprint: per-client server-side event
  queues, register-with-snapshot then incremental `getEventsSince`, queue
  GC + full-state resync on expiry. Proves an entire real-time product
  ships on request/response alone. This rung scales the pattern introduced
  in [`polls`](../polls) to many clients per user across many entities.
- **GitLab itself** — the architecture *lesson*, not a code reference: Rails
  keeps typed app logic; **Workhorse** (large/slow transfers) and **Gitaly**
  (all git object access, gRPC) bypass it. The shape to copy: typed actions
  in morph; bytes in sidecars. <https://docs.gitlab.com/development/architecture>
- [Pagure](https://github.com/Pagure/pagure) — curiosity worth knowing:
  issue/PR metadata stored as JSON *in git*, i.e. metadata history = git
  history — a cousin of morph's replayable journal.

## What to implement

Build order follows verified complexity ranking; each phase ships usable.

**Phase 1 — the tracker (morph sweet spot).**
Models: `OrgModel`, `RepoModel` (shared instance per repo), `IssueModel`
(shared instance per issue), `NotificationModel` (per user).

Two review-mandated design rules up front: **key models by immutable ids,
never by mutable attributes** — "instances never change key" is load-bearing
in the shared-instance design, and repo rename/transfer (a table-stakes
forge feature this rung must include) collides head-on with a name-keyed
`RepoModel`; and **per-user notification instances are unbounded** — N users
each pinning a live shared instance forever collides with
`LimitPolicy::maxLiveModels` and the absence of idle eviction; the load
script measures instances/memory vs. connected users deliberately, to
motivate an eviction policy [framework gap to expose].

1. Users, orgs, teams; repo create/settings; permission matrix
   (owner/admin/write/read) via `IAuthorizer` — Gitea's permission checks
   transliterated.
2. Issues: CRUD, comments, labels, milestones, assignees, state machine.
   **Issue history comes free from the journal** — Gitea maintains a
   `comment` row type per event; here the journal *is* that table.
3. Notifications: fan-out-on-write to per-user rows; clients poll unread
   counts (exactly what Gitea does); Zulip-pattern event queues for list
   deltas — including the Zulip design's *expiry half*: **event-queue GC
   and server-restart epochs**. A client holding `lastEventId` across a
   restart must detect the epoch change and full-resync; without it the
   load test silently measures the wrong thing after the first restart.
4. Search: SQL `LIKE`/FTS5 fallback (Gitea ships a DB fallback too);
   indexing pipelines are out of scope.

**Phase 2 — git enters (the sidecar).**

5. Repo browsing: tree/blob/commit/branch/log/README rendering. Git object
   access lives in a **sidecar module shelling out to git** (Gitea's
   `modules/git` approach) exposed as read-only actions; large blobs and
   raw-file/archive downloads go over a plain HTTP endpoint next to the
   WebSocket server — **the Gitaly/Workhorse lesson: bytes never travel the
   JSON action protocol.** Clone/push (smart HTTP/SSH) is served by that
   sidecar entirely outside morph.
6. Wiki: a git repo of markdown reusing the same sidecar.

**Phase 3 — collaboration machinery (the hard 20%).**

7. Webhooks: config as CRUD actions; delivery as a **durable outbound job
   queue in SQLite** (Gitea's `hook_tasks`) with retry + dead-letter —
   the background-job pattern from [`bookmarks`](../bookmarks) at
   production shape.
8. Pull requests + reviews: diff computation in the sidecar, paginated diff
   actions (response-size bounds get measured here), review threads
   anchored to diff positions, approve/request-changes state machine.
   **Merge is the submit→poll job idiom** from [`ledger`](../ledger):
   `SubmitMerge` → job id → poll status (no `Completion` chaining, no
   cancellation — this is where those limits show).
9. CI status: an external runner **polls** `FetchTask` (Gitea's actual
   protocol), posts status/logs up; the UI polls `GetLogsSince(offset)` for
   log tailing — incremental delivery within request/response, the honest
   stress test of one-callback-per-outcome.

## morph subsystems exercised

All of them, at scale: authorization at real granularity, shared instances
(repo/issue) with many concurrent viewers, journal as product feature
(issue history, audit), event-queue polling under N clients × M
subscriptions (the scale test for no-push), durable background queues, the
sidecar boundary for everything binary.

## Expected strain points (the point of the rung)

- **Polling at scale**: notification freshness vs. server load. Review
  quantified the meaningful load: **500–2,000 concurrent sockets at
  ~1 poll/s** — the ceiling is the single Qt thread that receives every
  frame and marshals every reply, not the worker pool; "dozens of clients"
  finds nothing. Measure p99 poll latency vs. N, including during a
  `closeGracefully` drain, plus the rate-limiter interaction (dropped
  frames hang unwrapped completions — the rung-3 helper's timeout is
  load-bearing here).
- **Payload bounds**: large diffs/file lists through JSON actions;
  pagination as a first-class action idiom — including **diff-cursor
  staleness under force-push** (cursors and review comments anchored to
  positions that no longer exist; put a diff id/epoch in the cursor).
- **Long operations**: merge/CI without composable completions or
  cancellation — the submit→poll idiom's limits. Test **duplicate
  `SubmitMerge`** (double-click → two jobs racing on one repo's git lock)
  and **client disconnect mid-poll** (the job registry must be
  server-scoped, not connection-scoped: the job completes and is
  re-pollable from a new connection).
- **Permission revocation mid-session**: a demoted user's attached
  `IssueModel`/`RepoModel` handlers must go fully inert — reads included —
  not just fail new registrations (kanban's revocation answer at forge
  scale).
- **The protocol boundary**: keeping git bytes, archives, and log streams
  cleanly outside the action model without the two worlds drifting; webhook
  deliveries signed via the [`vetted_hmac`](../vetted_hmac) pattern.
- **Right-to-erasure vs. permanent journal** (written deliverable): the
  journal never prunes; GDPR-class user deletion against an immutable audit
  trail is an unresolved framework question (rotation exists, redaction
  does not). Document the position.

## Security posture — the hardened-configuration demonstration

Delivery review found the ladder tested security features piecemeal but
never *composed* them; this rung closes that. The forge server binary's
default configuration is the full `docs/spec/security.md` checklist: TLS
(`tlsVerifyingConfig`/`tlsPinnedConfig`), `MORPH_REQUIRE_VETTED_HMAC=ON`
with a `vetted_hmac` adapter, a `SigningAuthorizer` subclass overriding
**both** `authorizeRegister` and `authorizeInstance`, full `LimitPolicy`,
full server bounds, and `hello` version negotiation — and the **load script
runs against this hardened config** (the limiter, in-flight caps, and TLS
change the latency curve; measuring only the unbounded server measures a
configuration the spec says never to deploy).

## Phase gating (delivery review)

Phases 1–2 constitute a shippable forge-lite. Phase 3's items (webhooks,
PRs/reviews, CI protocol) each get an individual go/no-go, like crm's 7b —
phase 3 is effectively a second product and must not be entered as a block.

## Explicit non-goals

Sub-second collaborative editing (Etherpad-class OT — genuinely requires
push), federation, code search indexing, and **public-internet exposure /
red-teaming** — but note the hardened *configuration* is in scope, per the
security section above.

## Definition of done

- Two orgs, several repos, issues + PRs + reviews end-to-end from Qt
  desktop and WASM clients against the remote backend, SQLite storage.
- A demo runner executes a job and the UI tails its log by polling.
- Webhook deliveries survive a server restart (durable queue) and retry.
- A load script sweeping to 500–2,000 polling connections (process-pool
  clients per [`../TESTING.md`](../TESTING.md)), with p99 latency and
  live-instance/memory measurements written up in this folder — including
  a run across a server restart (epoch resync) and a graceful drain.
