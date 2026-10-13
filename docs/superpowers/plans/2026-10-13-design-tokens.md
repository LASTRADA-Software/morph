# Design tokens: implementation plan (part 8, slice c1, #893)

**Goal.** An application's look is data: a theme is a set of DTCG 2025.10 token files a design tool exports, served
in the bundle, checked when the bundle is registered, resolved on the client into the closed `ui-theme/1` token set,
and read by every `MorphUi` component through the `Theme` singleton. Part of #893.

**Specs.** Spec 6 (`docs/superpowers/specs/2026-10-08-app-hosting-design.md`) §6 "Theme tokens" and the §12 "Tokens"
and "Design package" token cases; the 2026-10-08 addendum on #885 (DTCG 2025.10). In-tree:
`include/morph/ui/service.hpp` (`UiRegistry::addFile`, `build`), `include/morph/ui/bundle.hpp` (`kThemeFile`),
`src/qt_quick/qml/Theme.qml`, `docs/spec/qt_quick/backend.md` ("Layout and theme", "Not yet").

## What planning found

1. **Themes travel already, unread.** `theme` bundle files are served byte for byte; nothing parses them, and the
   test fixture's theme (`{"vocab":"ui-theme/1","accent":"#0a6"}`) is not a DTCG document.
2. **The bundle-level theme description has no home.** Spec 6 §6 shows a `"theme": {"vocab": "ui-theme/1", "base":
   …, "light": …, "density": {…}, "surfaces": {…}}` object listing the files of each variant, but no document or
   file carries it. It becomes a `theme` file of its own, the **theme index**, told apart from a token file by its
   `"vocab": "ui-theme/1"`.
3. **The closed token set is not written down anywhere.** Spec 6 §6 names `color.surface`, `space.2`,
   `control.height` and the semantic sets as examples. The `MorphUi` components read eleven colour roles and one
   opacity from `Theme.qml`, all fixed. The set this slice defines is what something reads: those roles, `accent`
   and `focus`, and the semantic sets. `space.*`, `control.*`, `font.*` and `elevation.*` join it with the
   components that read them (c2); until then they are unknown paths, resolved and ignored.
4. **The Qt Quick seam is small.** `Theme` is a QML singleton of `readonly` constants in a module with no C++ types.
   Making its properties writable, with today's values as their defaults, and setting them on the engine's singleton
   instance from C++ reaches every generated screen and every host component in that engine, with no new type
   registration.

## Design

### The token model (`ui/theme.hpp`, no toolkit)

- **`parseTokens(text)`**: a DTCG 2025.10 file. Groups nest; a token is an object with `$value`; `$type` is
  inherited from the nearest group that declares one. Refused: not JSON or deeper than `kMaxJsonDepth`, a
  must-understand (`!`) key, a name with `.`, `{` or `}`, a token with no type, a value that is not of its known
  type (`color`, `dimension`, `number`, `fontFamily`, `fontWeight`, `duration`), `$extends` (group inheritance is not
  supported), more than 16384 tokens. Composite types (`shadow`, `typography`, `border`, `strokeStyle`, `gradient`,
  `transition`, `cubicBezier`) are accepted unchecked; a token of a `$type` the client does not know is dropped.
- **Values.** A colour is `"#rgb"`, `"#rrggbb"`, `"#rrggbbaa"`, or the 2025.10 object (`hex`, or `srgb`
  `components` 0–1 with `alpha`); another colour space without `hex` is refused. A dimension is
  `{"value", "unit": "px"|"rem"}`.
- **Aliases.** A `$value` that is exactly `"{a.b}"`, or such a string anywhere inside a composite value, refers to
  another token. `resolveTokens(layers)` layers files in order, later winning per path, then resolves aliases;
  a cycle or a reference to a missing token refuses, naming the chain.
- **The theme index** (`parseThemeIndex`): `vocab` `ui-theme/1`, `base` (required), named variants (`light`,
  `dark`, …) each a file id, `density` and `surfaces` each a map of name to file id, `icons` accepted for later.
- **Checking a theme** (`checkTheme(index, files)`): every file the index names is a token file of the bundle, and
  every combination of a variant (or none), a density (or none) and a surface (or none) layered over `base`
  resolves. Each list is bounded at 16, so a check is at most 17³ resolutions.
- **The closed set** (`themeTokens()`): path, type and the `MorphUi` default. `themeValues(resolved)` gives every
  path of the set its resolved value, or its default when the theme lacks it or has it with another type; a path
  outside the set is ignored.

### Registration (`UiRegistry`)

- `addFile` of a `theme`: an index is parsed with `parseThemeIndex`, a token file with `parseTokens`; either refused
  as such. The fixture's theme becomes a real index and base file.
- `build`: each index is checked with `checkTheme`; a cycle, a missing reference or a missing file refuses the
  bundle.

### Qt Quick (`qt_quick::applyTheme`)

- `Theme.qml`'s colour and opacity properties become writable, with their current values as defaults, and gain
  `accent` and `focus`.
- `qt_quick::applyTheme(QQmlEngine&, ThemeValues const&)` sets them on the engine's `Theme` instance. Every
  `MorphUi` component and every host component that imports `MorphUi` reads the same instance, so a token reaches
  both; a token the theme lacks keeps its default.

## Left for (c2)

- **Surfaces and density at runtime:** a node's `surface` resolving its subtree through a named token set needs a
  per-subtree theme object, not the one singleton; `{"pref": "density"}` selecting a variant needs the preferences
  glue (#939) and a re-apply on change.
- **The scale tokens** (`space.*`, `control.*`, `font.*`, `elevation.*`), joining the closed set with the
  components that read them; today the units come from the default font's metrics.
- **Icons** (`asset:sha256:…` SVGs), **host components and restyling from the bundle** (spec 6 §6), **fallbacks**,
  **`a11y` and `testId`** mapping, the **development tools** (§21), and the **renderer as a `ScreenHost`** (needs
  #933's host contract).
- The terminal renderer's palette mapping.

## Tasks (one commit each)

1. This plan.
2. `ui/theme.hpp` and its tests: a design-tool export loads; variants layer over `base`; an alias resolves, inside a
   composite too; a cycle and a missing reference refuse; a missing token takes its default; an unknown `$type` and
   an unknown path are ignored.
3. Registration in `UiRegistry` and its tests; the fixture's theme becomes a real one.
4. `Theme.qml` and `qt_quick::applyTheme`, with Qt Quick tests: a token reaches a generated screen's widget and a
   host component; a missing token takes the default.
5. Docs: `docs/spec/ui/theme.md` (new), `docs/spec/ui/service.md`, `docs/spec/qt_quick/backend.md`, spec 6 §6 (the
   index file, the closed set), CHANGELOG.

## Verification

- Mutations, each against a freshly linked binary: no cycle check (the test must fail, not hang: the resolver
  bounds its depth); no layering (base wins); `applyTheme` setting nothing.
- GCC Debug `morph_tests` in full; a Qt Quick build (`MORPH_BUILD_QT_QUICK=ON`) and `morph_qt_quick_tests` offscreen;
  clang `-Weverything -Werror` on the new test files; clang-tidy on the changed lines; Doxygen, re-run after any
  comment edit; qmllint through the module's lint target; clang-format.
