# GUI localisation (i18n) — translated display text without per-locale schemas (planned)

> **Status: planned — not yet implemented.** This spec is part of the GUI
> enhancement program ([gui_overview.md](gui_overview.md), Tier 1,
> cross-cutting). It supplies the "different mechanism entirely" that
> [forms.md](../spec/forms/forms.md) defers when it rules out localised schemas, and
> that [gui_field_metadata.md](gui_field_metadata.md),
> [gui_computed_fields.md](gui_computed_fields.md), and
> [gui_cross_field_rules.md](gui_cross_field_rules.md) all point at from their
> "no i18n" non-goals. The schema stays one-per-type and un-localised;
> translation is a **renderer-side catalog lookup over stable, derivable
> message keys**. See [todo.md](../todo.md).

## The gap

The GUI program is about to mint user-visible text with no translation story:

- **The schema cannot carry per-locale text.** Each type's schema is memoised
  once, process-wide (`schemaJson<A>()`'s function-local static), which
  "precludes localised / i18n schemas … a translated form would need a
  different mechanism entirely" ([forms.md](../spec/forms/forms.md), "One cached
  schema per type — no localisation"). That design is deliberate and stays.
- **The program's display text is multiplying anyway.** Tier 1 emits `title` /
  `description` / `x-placeholder` per field
  ([gui_field_metadata.md](gui_field_metadata.md)), group titles inside
  `x-layout` (and the group-title string again as each field's `x-group`,
  [gui_layout_grouping.md](gui_layout_grouping.md)); Tier 2 emits `w-title`,
  per-step `title`, `app-title`, and `app-menu` labels
  ([gui_workflows_navigation.md](gui_workflows_navigation.md)). Cross-field
  rules need violation *messages* a renderer must produce from structure
  ([gui_cross_field_rules.md](gui_cross_field_rules.md)). Every one of these
  is baked or renderer-invented, and none is translatable.
- **The locale hook exists but nothing consumes it.**
  `session::Context::locale` is a BCP-47 tag on every call
  ([session.md](../spec/session/session.md)) — plumbed, documented, unused.
- **Locale formatting is unspecified.** Decimal comma vs point for `Rational`
  entry, local-time display of a strictly-UTC `Timestamp`
  ([datetime.md](../spec/util/datetime.md) excludes time-zone conversion) — today
  each renderer improvises with no contract to conform to.

Deferring this past Tier 1 gets expensive: `fieldMetadata` declarations and
renderer catalogs will accrete around whatever key scheme exists, so the key
scheme has to be fixed **before** labels proliferate, not after.

## Design

One principle: **the schema carries structure plus neutral fallback text; a
renderer-side catalog, keyed by stable message keys, supplies translations.**
The locale never reaches `schemaJson<A>()`; the cached-schema design is
untouched.

### 1. Stable message keys, derived — not declared (NEW)

Keys are derived mechanically from identifiers the schema already carries, so
the common case needs zero declaration (the program's infer-by-default rule,
[gui_overview.md](gui_overview.md)):

| Text slot | Derived key |
|---|---|
| field label / help / placeholder | `<actionTypeId>.<wireField>.label` / `.help` / `.placeholder` |
| layout group title | `<actionTypeId>.group.<index>` (index into `x-layout.groups`) |
| cross-field rule message | `<actionTypeId>.rule.<index>` (index into `x-rules`) |
| wizard title / step title | `<wizardId>.title` / `<wizardId>.step.<index>.title` |
| app title / menu label | `<appId>.title` / `<appId>.menu.<index>.label` |

`<actionTypeId>` is the registered `ActionTraits<A>::typeId()` string — already
protocol vocabulary ("append, never rename"), which is exactly the stability a
translation key needs. Wire field names, group/rule indexes, and wizard/app
ids are all present in the emitted schema, so **a renderer can derive every
key from the schema alone**; in the common case this spec adds no bytes to
any schema.

**Declare to override:** hosts with an existing catalog or TMS key scheme can
pin a key explicitly. `FieldMeta` ([gui_field_metadata.md](gui_field_metadata.md))
gains an optional `i18nKey` slot, emitted as **`x-i18nKey`** on the property
node; the same optional slot is added to `FieldGroup`
([gui_layout_grouping.md](gui_layout_grouping.md)) and to wizard steps / menu
entries ([gui_workflows_navigation.md](gui_workflows_navigation.md)), emitted
as an `i18nKey` member of the corresponding descriptor object. These are the
only new schema keys this spec introduces, and all are additive and optional
per the program's versioning stance.

### 2. The catalog seam — renderer-side, host-supplied (NEW)

```cpp
// namespace morph::render — NEW, client-side only; never on the wire.
// Sits beside SlotRegistry in the renderer toolkit (gui_renderer_toolkit.md).
using TranslationProvider =
    std::function<std::optional<std::string>(std::string_view key,
                                             std::string_view bcp47Locale)>;
```

Resolution per display slot, most specific first:

1. explicit `x-i18nKey` (when declared) — looked up in the catalog;
2. the derived key from the table above — looked up in the catalog;
3. **miss ⇒ the schema literal** (the authored `title` / `description` /
   `x-placeholder` / group or step title) — today's behavior, verbatim.

morph ships the seam, the derivation rule, and the fallback — **no
translations and no storage format**. The provider is host-native: the QML
reference renderer wires it to `QTranslator`/`.qm` catalogs, a web renderer to
its JSON catalog, a test to a lambda. An unconfigured renderer (no provider)
skips straight to the schema literal and renders exactly as today.

Two sharp edges the contract pins:

- **Group membership is matched by index, never by translated text.** Each
  field's `x-section` is the stable numeric handle into `x-layout.groups`
  ([gui_layout_grouping.md](gui_layout_grouping.md)); a renderer translates a
  group's *displayed* title but places fields by index — `x-group`'s
  redundant title string is display-only under i18n.
- **Rule messages come from the catalog, not the wire.** For a rule the
  client can evaluate, the renderer shows its catalog message
  (`<action>.rule.<index>`, falling back to a renderer-built neutral message
  from the rule's structure). Server error strings stay canonical protocol
  text ([error_handling.md](../spec/error_handling.md), pinned by
  [drift_guard.md](drift_guard.md)) and are surfaced only for conditions the
  client could not pre-empt.

### 3. Locale data formatting — a contract, not a mechanism

Display formatting is the renderer's duty; the wire stays canonical:

- **Numbers.** A locale may render and accept `1.050,25`; the payload is the
  canonical exact `{num, den, dp}` regardless
  ([rational.md](../spec/util/rational.md)). The renderer converts at the control
  edge; the exact digit routines are locale-free.
- **Timestamps.** The wire value is strict UTC ISO-8601
  ([datetime.md](../spec/util/datetime.md)); displaying and editing in the user's
  zone/format is the control's duty, and a locale-formatted entry must
  round-trip to the identical canonical wire value.
- **Choice option labels are data, not chrome.** Option rows come from
  executing the options action ([choice.md](../spec/forms/choice.md)); the catalog
  never sees them. A model that wants localised rows reads
  `session::current()->locale` server-side ([session.md](../spec/session/session.md))
  — the one place server-side locale participates.

### 4. Conformance fixtures ([gui_renderer_toolkit.md](gui_renderer_toolkit.md))

The renderer conformance kit gains locale fixtures: a catalog hit renders the
translated label; a miss falls back to the schema literal; a decimal-comma
locale entry produces the identical canonical payload; fields stay in their
`x-section` groups under translated titles; a `Timestamp` edited in a
non-UTC display zone round-trips unchanged.

## Non-goals

- **No per-locale schema variants.** `schemaJson<A>()` keeps its one cached,
  un-localised schema; no locale parameter is added anywhere in
  [forms.md](../spec/forms/forms.md)'s surface.
- **No translation storage format.** Qt `.ts`/`.qm`, gettext, JSON, a
  database — the provider signature is the whole contract.
- **No server-side message localisation.** Canonical error strings are
  diagnostic/protocol vocabulary, deliberately stable
  ([drift_guard.md](drift_guard.md) pins them); user-facing wording is the
  renderer's catalog's job.
- **No RTL / layout mirroring engine.** Mirroring is the host toolkit's
  concern (Qt's `LayoutMirroring`, CSS `direction`); the contract only
  requires that `x-order` / `x-section` remain *logical* order.
- **Not machine translation, locale negotiation, or plural rules.** The
  catalog is a lookup; anything richer (ICU MessageFormat, plurals) lives
  inside the host's provider implementation.

## Testing (planned)

- Key derivation: a fixture action's derived keys match the table for every
  slot (field, group, rule, wizard step, menu entry).
- `x-i18nKey` overrides the derived key; absence emits nothing (schema
  byte-identical for actions with no override — regression guard).
- Provider hit / miss: translated text renders on hit; the schema literal
  renders on miss and when no provider is installed.
- Membership under translation: fields land in the correct group by
  `x-section` when every group title is translated.
- Formatting round-trips: decimal-comma entry → canonical `{num, den, dp}`;
  zoned `Timestamp` edit → identical UTC ISO-8601 wire value.
- A model reading `Context::locale` returns localised option rows over the
  wire (server-side data localisation path).

## Cross-references

- [forms.md](../spec/forms/forms.md) — the cached, un-localised schema this spec
  works with rather than against; the deferred "different mechanism" this is.
- [gui_field_metadata.md](gui_field_metadata.md) /
  [gui_layout_grouping.md](gui_layout_grouping.md) /
  [gui_workflows_navigation.md](gui_workflows_navigation.md) — the
  text-bearing keys the catalog translates; the descriptor surfaces gaining
  optional `i18nKey` slots.
- [gui_cross_field_rules.md](gui_cross_field_rules.md) — rule structure the
  renderer turns into localised violation messages.
- [gui_renderer_toolkit.md](gui_renderer_toolkit.md) — where the
  `TranslationProvider` seam lives and the conformance fixtures run.
- [session.md](../spec/session/session.md) — `Context::locale`, the server-side hook
  for data (not chrome) localisation.
- [datetime.md](../spec/util/datetime.md) / [rational.md](../spec/util/rational.md) —
  the canonical wire forms display formatting must round-trip to.
- [choice.md](../spec/forms/choice.md) — option labels as data, out of catalog
  scope.
- [drift_guard.md](drift_guard.md) — why canonical server strings stay
  untranslated.
- [gui_overview.md](gui_overview.md) — the umbrella program and the
  infer-by-default / declare-to-override discipline the key scheme follows.
- [todo.md](../todo.md) — roadmap placement (Tier 1, alongside E-G1).
