# morph's declarative UI vs. web frameworks — what to keep, what to learn

Date: 2026-10-08
Compared: specs 0–7 of the declarative UI program (`docs/superpowers/specs/2026-10-0[478]-*.md`, branch
`docs/declarative-ui-specs` @ `822a1eef`) and the plans for #886/#887, against the web frameworks that solve the
same problems.

## Verification status

- **Read, not run.** Every statement about morph comes from reading the specs. No prototype was built and nothing
  was measured.
- **Web facts.** Facts marked **(checked)** were looked up on 2026-10-08; the sources are listed at the end. The
  rest is general knowledge of each framework's public API and design, not re-checked for this note. Treat a
  version number or option name without **(checked)** as something to confirm before citing it in a spec.
- **What would change a verdict.** A lesson below is a proposal, not a defect. One is wrong if the spec already
  covers it in a section this note missed, or if the use case it serves is not one morph has. The second
  happens to L8 if measurement shows per-row scopes are cheap at 10,000 rows.

## 1. Summary

The program already arrived, independently, at most of what the web has converged on: fine-grained signals,
declarative server state (`Query`/`Mutation`), keyed serial mutations, a headless table engine with a server
mode, and a JSON UI over a catalogue of trusted native components with fallbacks. Where morph differs on
purpose (no code shipped to the client, no server-held UI state, exact numbers, a total expression language),
the web's own failures support morph's choice.

What the web does that the specs do not, ranked by value and by how much harder it gets once `ui/1` is
frozen:

| # | Lesson | Web precedent | Lands in | Do it |
|---|---|---|---|---|
| L1 | A client-wide query cache: dedup by key, invalidation by action across screens, `staleTime` | TanStack Query `QueryClient` | #887, #890 | **Before #890** — it shapes the interpreter |
| L4 | Plural and select messages | Unicode MessageFormat 2 (stable, CLDR 47) | #890, #893 | **Before `ui/1`** — a later change is `ui/2` |
| L5 | DTCG token format for `ui-theme/1` | W3C DTCG 2025.10 (stable) | #893 | **Before `ui-theme/1`** |
| L10 | `touched` and an error-display policy | TanStack Form, react-hook-form | #890, #892 | Before `ui/1` |
| L13 | Accessibility lint at screen registration | eslint-plugin-jsx-a11y, axe-core | #890 | Cheap, any time |
| L2 | Optimistic updates with rollback | TanStack `onMutate`, React `useOptimistic` | #890 | Before kanban (#899) |
| L3 | Routes: a screen's identity and params as a URL | every web router | #891, #893 | Before the WebAssembly client ships |
| L9 | Loading and error boundaries over a subtree | React Suspense and error boundaries | #890 | Optional; saves document boilerplate |
| L6 | An inspector for documents, queries and the graph | TanStack Query Devtools, React DevTools | after #895 | Later |
| L7 | Live reload of server documents in development | Vite HMR | after #894 | Later; nearly free |
| L8 | Windowed row scopes for long lists | TanStack Virtual, react-window | #888, #889, #891 | **Measure first** |
| L12 | Prefetch on intent | router loaders, link prefetch | after #893 | Later |

## 2. Where morph already matches the web

| morph | Web counterpart | Note |
|---|---|---|
| `morph::reactive`: push-dirty/pull-value, glitch-free, lazy `Computed`, batched effects (spec 1 §3) | Preact Signals, SolidJS, Vue's reactivity, Angular signals, Svelte 5 runes; the TC39 Signals proposal | The web's consensus. Spec 1 already cites Reactively and Preact. The web moved away from virtual-DOM diffing toward exactly this. |
| `Query`: key-tracked, latest wins, last value kept while refetching, `refreshEvery` (spec 1 §4b) | TanStack Query (`queryKey`, `placeholderData: keepPreviousData`, `refetchInterval`); Angular `resource()`/`httpResource()` **(checked**: stable in Angular 22) | Same model. morph's `debounce` is a built-in that TanStack users add by hand. |
| `Mutation` with `serial` + `serialKey` (spec 5 §5) | TanStack Query v5 mutation `scope: { id }` — "mutations with the same scope id run in serial" **(checked)** | Near-identical. Confirms the design. |
| `Mutation` `invalidates`, `refreshOn` | TanStack `invalidateQueries({ queryKey })` | Same intent, narrower scope. See L1. |
| `Store<ViewState, Msg>` with `ExhaustiveUpdate` (spec 1 §4) | The Elm architecture; Redux reducers | Elm checks exhaustiveness at compile time, as morph does. Redux cannot. |
| JSON documents rendered by native components from a client-side catalogue, with fallbacks (spec 5 §1, §10) | Adaptive Cards; Google A2UI — "the client maintains a catalog of trusted UI components and the agent can only request components in that catalog" **(checked)**; server-driven UI at Airbnb and Lyft | morph is squarely in this family. |
| `!`-prefixed must-understand keys, `vocab` versions, per-node fallback (spec 5 §12) | Adaptive Cards `requires` (feature → minimum version) and `fallback` (an alternative element, or `drop`) **(checked)** | Same mechanism. Adaptive Cards adds `drop` as an explicit fallback; morph's fallback subtree covers it (an empty subtree). |
| Custom components see only their props, leave only through declared events (spec 5 §10) | Shopify Remote DOM — sandboxed code renders a controlled set of elements on the host **(checked)** | Remote DOM sandboxes the *component*. morph instead trusts the bundle by hash and contains the component by its interface. Both draw the line at "no ambient access". |
| `expr/1`: pure, total, bounded, no user functions (spec 5 §4) | Google CEL (Common Expression Language), JSONLogic | CEL's design rationale — non-Turing-complete, linear-time, safe for untrusted input — is morph's, and it has held up. |
| Forms' `x-rules`, schema forms (spec 2) | JSON Forms (UI schema + rules), react-jsonschema-form | Same split of a data schema from a UI schema. |
| Table engine: headless, client mode and server mode (spec 7) | TanStack Table (headless; `manualSorting`/`manualFiltering`/`manualPagination` hand the work to the server); AG Grid server-side row model | Same architecture. morph's "the engine owns the view, not the renderer's model" is TanStack Table's core idea. |
| `RecordingBackend` helpers act only on what a user could reach; `a11y` names and `testId` (spec 1 §5, spec 6 §6) | Testing Library — "query the way a user would" (`getByRole`, `getByLabelText`, `data-testid` as the escape hatch) | Same philosophy, and the port check in spec 6 §12 is Testing Library's migration story. |
| Server-held UI state rejected (spec 5 §1) | Phoenix LiveView, Blazor Server | Right for morph: both need a live connection per user, and morph has an offline queue. |

## 3. Lessons

### L1. One query cache per client, keyed, shared across screens

**What the web does.** TanStack Query keeps every query in one `QueryClient`, keyed by `queryKey`. Two components
asking for the same key share one request and one cached value. A mutation invalidates by key prefix, and every
screen showing that data refetches. `staleTime` says how long a value counts as fresh, so remounting a screen
does not refetch. `gcTime` says how long an unused value is kept.

**What morph does.** A query belongs to its scope (spec 5 §5). `invalidates` names queries of the same document.
`refreshOn` fires only for calls "made through that alias by this client" (spec 5 §5). The app root scope shares
a query between screens (spec 6 §5), but only one the app document declares.

**Why it matters.** The workspace shell (spec 6 §7) keeps several tabs mounted. If tab A captures a result, tab
B's list of the same sample's results is not refreshed: it is another document, so neither `invalidates` nor
`refreshOn` reaches it. Two tabs showing the same sample also send the same query twice. A remount after a tab
switch refetches everything.

**Proposal.** A client-wide query cache in the interpreter:
- key = `(model type, instance, action, canonical body JSON)`;
- identical in-flight requests are deduplicated;
- `refreshOn` and `invalidates` may name an **action id client-wide**, so a successful call to it refetches
  every live query on that model type and instance, in every screen;
- an optional `staleTime` on a query.

`Query` (#887) need not change. The cache sits in the interpreter (#890) and hands queries a shared fetcher. It
must be decided before #890 is planned, because it decides whether a query is owned by a scope or by the client.

**Caveat.** A shared value must not leak between principals; the cache is per session and cleared on `signOut`.

### L2. Optimistic updates with rollback

**What the web does.** TanStack Query's `onMutate` writes the expected result into the cache before the request
and rolls it back in `onError`. React 19's `useOptimistic` shows an optimistic state for the duration of an
action.

**What morph does.** `patch` replaces a cached row **after** the reply (spec 5 §5). The calculated-values rules
(spec 5 §7) keep the last value and mark it `stale`, which is right for values the server computes.

**Why it matters.** Kanban's drag-to-move (spec 4 §1) and any toggle (archive, pin, check off) will show
round-trip latency over a remote connection, and worse over a slow WebAssembly link.

**Proposal.** `"optimistic": {"query": "q", "key": e, "with": e}` on a mutation, the same shape as `patch`: applied
when the run starts, replaced by the reply, rolled back on failure. Restrict it to client-owned fields so it
cannot contradict spec 5 §7's ownership rule. Server-calculated values never travel optimistically.

### L3. A screen is addressable: routes and history

**What the web does.** Every router makes a screen and its parameters a URL. The back button, reload, bookmarks,
shared links and "open in new tab" all follow from that.

**What morph does.** `navigate` takes a screen id and params. A screen has an `identity` (spec 6 §3), and the
workspace shell can `resume` (spec 6 §7). Nothing serialises a screen to a string.

**Why it matters.** The WebAssembly client (spec 3 §10) runs in a browser. Without a route, its back button leaves
the application and a reload loses the user's place. On the desktop, the same serialisation is what `resume` and
"copy link to this sample" need.

**Proposal.** A canonical route `screen-id?param=value…` derived from `identity` and `params`. On WebAssembly the
Qt Quick frontend maps it to `history.pushState`/`popstate`; on the desktop it is the `resume` record and a
`morph://` link. Params are already typed (spec 5 §3), so parsing a route back is validated like any document
input.

### L4. Plural and select messages

**What the web does.** ICU MessageFormat, now Unicode MessageFormat 2. MessageFormat 2 became **stable in CLDR
47 (March 2025)** **(checked)**, with ICU and JavaScript implementations. `{count, plural, one {# sample} other
{# samples}}` is how every localised web app writes counts.

**What morph does.** `t(key, fallback, args...)` and `format(template, args...)` (spec 5 §4). There is no plural
or select form anywhere in the specs (grep for "plural": no match).

**Why it matters.** "1 samples selected" is wrong in English, and wrong in a different way in Polish, Russian and
Arabic, where the plural rules have three to six forms. Adding it after `ui/1` and `expr/1` freeze is a new
vocabulary version (spec 5 §12).

**Proposal.** Message catalogues (spec 6 §19) hold MessageFormat 2 patterns, evaluated in C++ with CLDR plural
rules. `t`'s `args` become named. Short of that, add a `plural(n, {one, few, many, other})` function to `expr/1`
now, so the vocabulary has the hook.

### L5. Design tokens in the DTCG format

**What the web does.** The W3C Design Tokens Community Group format reached its **first stable version, 2025.10,
on 28 October 2025** **(checked)**. Style Dictionary, Tokens Studio and Figma support it.

**What morph does.** Spec 6 §6 defines its own token map (`"color.accent": "#2563EB"`, …) under `ui-theme/1`.

**Proposal.** Make `ui-theme/1`'s token file a DTCG document, or a strict subset of one. A designer's Figma tokens
then become a morph theme with no converter, which serves spec 6's goal that an application's own code be "all
design or integration". Cheap now; a format change after `ui-theme/1` ships is a new vocabulary version.

### L6. An inspector

**What the web does.** TanStack Query Devtools (every query's key, state, age, and a refetch button), React
DevTools (the component tree and its props), Redux DevTools (every action, with time travel).

**What morph does.** A diagnostic channel for expression errors (spec 5 §4). Nothing shows a running screen's
state.

**Proposal.** A development-only inspector over the interpreter: the mounted document tree with each node's
evaluated properties, every query and mutation with key, state and age, the command log, and the reactive
graph's flush counts. It can be a host component or a second window. It is the tool that makes server-defined
screens debuggable by someone who did not write the C++ builder.

### L7. Live reload of documents in development

**What the web does.** Vite's hot module replacement swaps changed code and keeps component state.

**Proposal.** In development, the client polls `ui-hello` for the manifest digest (spec 5 §12) and, when it
changes, re-fetches and remounts changed screens, keeping state by `identity` where the new document still
declares it. Documents are already content-addressed and validated, so this needs no server push, which spec 5
§15 leaves out.

### L8. Windowed row scopes — measure first

**What the web does.** TanStack Virtual and react-window render only visible rows.

**What morph does.** A `forEach` or `table` row is a scope with its own state, queries and effects (spec 5 §3,
§7). Qt Quick's `ListView` windows the *items*. Whether the interpreter also creates row *scopes* only for
visible rows is not stated.

**Why it matters.** A 10,000-row table with a per-row `calc` query and draft state could mean tens of thousands
of reactive nodes. Spec 7 §15 measures sort and filter, not mounting.

**Proposal.** Add a mount benchmark to spec 7 §15 (1,000, 10,000 and 100,000 rows with a row scope), and state
whether row scopes are windowed. **Verdict condition:** if the mount cost at 10,000 rows is within one frame
budget, drop this lesson.

### L9. Loading and error boundaries

**What the web does.** React Suspense shows one fallback while anything inside a subtree is first loading. An
error boundary shows one error view, with retry, for any failure inside it.

**What morph does.** Each element binds its own `pending`, `stale` and `errorKind` (spec 5 §4, spec 6 §10). That
is precise but repetitive: a panel over three queries needs three bindings.

**Proposal.** A `boundary` node: `{"kind": "boundary", "loading": [...], "error": [...], "children": [...]}`,
which shows `loading` until every query in the subtree has a first value, and `error` (with a `retry` command
that refetches them) when one fails with no value to keep. Per-element `stale` stays for refetches.

### L10. `touched`, and when to show errors

**What the web does.** TanStack Form and react-hook-form track `touched`/`isBlurred` per field and let a form
choose when errors appear: on change, on blur, or on submit. Showing "required" before the user has visited a
field is a known anti-pattern.

**What morph does.** Element values include `dirty` and `focused` (spec 5 §6), not `touched`, and no rule says
when `errors` become visible.

**Proposal.** Add `touched` (the element has lost focus at least once). Add a form or dialog policy,
`showErrors: "touched" | "submit" | "always"`, defaulting to `touched`, with `submit` revealing every error at
once. Cheap, and part of `ui/1`.

### L11. Matched already: keeping the old screen while the new one loads

React's `useTransition` keeps the current UI interactive while the next one loads. morph's query keeps its last
value during a refetch (spec 1 §4b), which is the same idea at a smaller grain. No action.

### L12. Prefetch on intent

**What the web does.** Router loaders and link prefetch start a screen's data on hover, or as soon as navigation
starts, rather than after the screen mounts.

**Proposal.** With L1's cache in place, `navigate`'s target may declare `prefetch` queries the shell starts on
hover or keyboard focus of a menu entry or row. Later; it needs L1.

### L13. Accessibility lint at screen registration

**What the web does.** eslint-plugin-jsx-a11y and axe-core catch an input without a label or an icon button
without a name before a user does.

**Proposal.** Screen registration already validates the document (spec 5 §11). Add rules: every input has a
label or `a11y.name`; every icon-only button and every `custom` node has `a11y.name`; every `table` has a
caption or name. A failure is a failing server test, which is how spec 5 §11 treats every other document defect.

## 4. What the web does that morph should keep refusing

- **Shipping code as the UI.** React Server Components and htmx send renderable output. morph sends data that
  is validated before anything runs. A2UI's stated reason for being declarative — "the client is not getting
  executable code" **(checked)** — is spec 5 §1's.
- **Server-held view state** (LiveView, Blazor Server): it needs a live connection per user and breaks the offline
  queue.
- **An open-ended expression language**, or JavaScript in templates (Vue and Angular template expressions,
  JSONata). `expr/1`'s totality and size bound are what make a document safe to load from a server.
- **Floating-point numbers by default.** JavaScript's `number` is the web's long-running source of money and
  quantity bugs. morph's exact `int`/`decimal`/`quantity` is an advantage to keep, not a gap.

## 5. Where each lesson is specified

Every lesson except L11 (already matched) is part of the specs:

| Lesson | Specified in |
|---|---|
| L1 Query cache, `refreshOn` across screens, `staleTime` | Spec 5 §5 "The query cache" and `refreshOn`; spec 1 §4b |
| L2 Optimistic updates | Spec 5 §5 Mutations, `optimistic` |
| L3 Routes | Spec 6 §20; spec 5 §8 (`navigate` by route); spec 3 §10 (browser history); spec 6 §7 (`resume` records) |
| L4 Plurals | Spec 5 §4 (`t` with named arguments, the MessageFormat 2 subset); spec 6 §19 (catalogues) |
| L5 DTCG tokens | Spec 6 §6 |
| L6 Inspector | Spec 6 §21 |
| L7 Development reload | Spec 5 §12; spec 6 §21 |
| L8 Windowed row scopes | Spec 7 §15 (the mount measurement decides); spec 3 §5 |
| L9 Boundaries | Spec 5 §9 (`boundary`), §8 (`retry`) |
| L10 `touched` and `showErrors` | Spec 5 §6 |
| L12 Prefetch | Spec 5 §5 (`prefetch`); spec 6 §7 |
| L13 Accessibility rules | Spec 5 §11 |

Tests for each are in spec 5 §14 and spec 6 §12.

## Sources

- TanStack Query, mutations guide (v5 mutation scopes): <https://tanstack.com/query/v5/docs/framework/vue/guides/mutations>
- Design Tokens Community Group, first stable version (2025.10): <https://www.w3.org/community/design-tokens/2025/10/28/design-tokens-specification-reaches-first-stable-version/>, <https://designtokens.org/>
- Unicode CLDR 47 (MessageFormat 2 stable): <https://cldr.unicode.org/index/downloads/cldr-47>, <https://blog.unicode.org/2025/03/>
- A2UI: <https://a2ui.org/>, <https://sdtimes.com/ai/google-launches-a2ui-project-to-enable-agents-to-build-contextually-relevant-uis/>
- Angular resource APIs: <https://dev.to/codewithrajat/angulars-resource-apis-are-finally-stable-say-goodbye-to-manual-loading-states-what-changes-in-2594>, <https://v19.angular.dev/api/common/http/httpResource>
- Shopify Remote DOM: <https://github.com/Shopify/remote-dom>, <https://shopify.engineering/remote-rendering-ui-extensibility>
- Adaptive Cards `requires` and `fallback`: <https://adaptivecards.io/explorer/ActionSet.html>, <https://adaptivecards.io/schemas/1.2.0/adaptive-card.json>
