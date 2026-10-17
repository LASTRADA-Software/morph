# Theme tokens — design

An application's look is data: a theme is a set of **DTCG 2025.10** token files, the format a design tool exports,
served in the bundle, checked when the bundle is registered, resolved on the client into the closed `ui-theme/1`
token set, and applied to the renderer. This implements the token parts of spec 6 §6
(`docs/superpowers/specs/2026-10-08-app-hosting-design.md`).

| Header | What it holds |
|---|---|
| `include/morph/ui/theme.hpp` | `parseTokens`, `resolveTokens`, `parseThemeIndex`, `resolveTheme`, `checkTheme`, `colorOf`, `kThemeTokens`, `themeValues` |
| `include/morph/ui/service.hpp` | `UiRegistry` checks themes at `addFile` and `build` |
| `include/morph/qt_quick/theme.hpp` | `qt_quick::applyTheme` |

## Contents

- [Token files](#token-files)
- [Aliases and layering](#aliases-and-layering)
- [The theme index](#the-theme-index)
- [At registration](#at-registration)
- [The closed set](#the-closed-set)
- [On Qt Quick](#on-qt-quick)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## Token files

A token file is a JSON object of groups and tokens. A **token** is an object with `$value`; a **group** is any other
object, and groups nest. A token's `$type` is its own or the nearest enclosing group's. `$description`,
`$extensions`, `$deprecated` and other `$` keys are ignored.

```json
{ "color": { "$type": "color",
             "surface": { "$value": { "colorSpace": "srgb", "components": [1, 1, 1], "hex": "#ffffff" } },
             "accent":  { "$value": "#2563EB" }, "focus": { "$value": "{color.accent}" } },
  "space": { "$type": "dimension", "2": { "$value": { "value": 8, "unit": "px" } } } }
```

`parseTokens` checks the shape of the simple types and accepts the composite ones unchecked:

| `$type` | Value |
|---|---|
| `color` | `"#rgb"`, `"#rrggbb"`, `"#rrggbbaa"`, or the 2025.10 object: its `hex`, or `srgb` `components` from 0 to 1 with an optional `alpha` |
| `dimension` | `{"value": n, "unit": "px" \| "rem"}` |
| `duration` | `{"value": n, "unit": "ms" \| "s"}` |
| `number` | a number |
| `fontFamily` | a name, or a non-empty list of names |
| `fontWeight` | 1 to 1000, or a DTCG weight name (`semi-bold`, …) |
| `shadow`, `typography`, `border`, `strokeStyle`, `gradient`, `transition`, `cubicBezier` | accepted unchecked |

A token of another `$type` is dropped, as spec 6 §6 says. Refused: text that is not a JSON object or nests deeper
than `kMaxJsonDepth`, a must-understand (`!`) name, a name with `.`, `{` or `}`, a member that is not an object, a
token with no type that is not a plain alias, a value that does not have its type's shape, `$extends`, and more than
16384 tokens. A colour in another colour space with no `hex` is refused rather than converted.

## Aliases and layering

A `$value` that is exactly `"{a.b}"` is an alias of the token at `a.b`; the same string inside a composite value
(a shadow's `color`) is replaced by its target's value. `resolveTokens(layers)` layers the files in order, a later
file replacing a token of the same path, then resolves every alias against the layered set. An alias with no
`$type` takes its target's; one with a `$type` must match it. Refused, naming the path or the chain:

- an alias cycle, including one that only the layering closes;
- an alias to a token no layer has;
- a chain of more than 64 aliases, so resolution is bounded whatever the input;
- a typed alias to a token of another type;
- a resolved value that does not have its type's shape.

## The theme index

A theme index is a `theme` bundle file whose top level has a string `vocab`:

```json
{ "vocab": "ui-theme/1",
  "base": "theme/base.tokens.json", "light": "theme/light.tokens.json", "dark": "theme/dark.tokens.json",
  "density": { "comfortable": "theme/comfortable.tokens.json", "compact": "theme/compact.tokens.json" },
  "surfaces": { "sidebar": "theme/sidebar.tokens.json" } }
```

`base` is required; every other string member is a named variant; `density` and `surfaces` map names to files;
`icons` is accepted and not read. Each list holds at most 16 names. `resolveTheme(index, files, variant, density,
surface)` layers `base`, then the variant, the density and the surface, each optional, and resolves.

## At registration

`UiRegistry::addFile` parses a `theme` file as an index when it has a string `vocab` and as a token file otherwise,
and refuses one that does not parse. `build` checks each index with `checkTheme`: every combination of a variant (or
none), a density (or none) and a surface (or none) over `base` must resolve from the bundle's token files, so a
cycle, a missing reference, or a file the index names that the bundle lacks refuses the bundle, naming the index and
the combination (`base + dark + density compact: …`). At most 17³ combinations are resolved.

## The closed set

`kThemeTokens` is the closed `ui-theme/1` set, each path with its kind and the `MorphUi` default:

| Paths | Kind |
|---|---|
| `color.surface`, `color.text`, `color.muted`, `color.accent`, `color.focus`, `color.border`, `color.neutral`, `color.selection` | colour |
| `color.ok`, `color.warn`, `color.err`, `color.info`, each with `.bg`, `.border` and `.dot` | colour |
| `opacity.stale` | number |
| `space.0` … `space.5`, defaults 0, 4, 8, 12, 16, 24 | dimension, in px |

`themeValues(resolved)` gives every path of the set the theme's value when the theme has it with the set's type, and
its default otherwise; a token outside the set is ignored. A dimension is read in px, and a `rem` is 16 px
(`kPixelsPerRem`): the renderers do not change the root font size. `ThemeValues` keeps colours, numbers and
dimensions in three maps.

The six `space` tokens are the spacing scale: `space.<step>` is how far apart a `gap` or `padding` of that step puts
things (`docs/spec/ui/view_tree.md`, the spacing step). Other `space.*` paths, `control.*`, `font.*` and
`elevation.*` are resolved and checked like any token, and join the set with the components that read them.

## On Qt Quick

The `MorphUi` `Theme` singleton's colour roles and `staleOpacity` are writable properties whose values are the
`ui-theme/1` defaults. `qt_quick::applyTheme(engine, values)` sets them on the engine's `Theme` instance:

| `Theme` | Token |
|---|---|
| `text`, `muted`, `surface`, `border`, `neutral`, `selection`, `accent`, `focus` | `color.<same name>` |
| `error`, `success`, `info`, `warn` | `color.err`, `color.ok`, `color.info`, `color.warn` |
| `staleOpacity` | `opacity.stale` |
| `space` (six px lengths) | `space.0` … `space.5` |

Every `MorphUi` component binds to `Theme`, and a host component that imports `MorphUi` reads the same instance in
the same engine, so a theme reaches both, and applying another theme later updates what is mounted. It returns the
properties it could not set. The semantic `bg`, `border` and `dot` colours have no `Theme` property yet: no built-in
component draws them.

## Design decisions

| Decision | Chosen | Why |
|---|---|---|
| Where the variant list lives | A `theme` file of its own, the index | The bundle has no other place for it, and the manifest lists it with the token files. |
| Index or token file | A string `vocab` at the top | A DTCG file's top-level members are groups and tokens, objects. |
| When a theme is checked | Shape at `addFile`, resolution at `build` | Aliases cross files, and files arrive in any order. |
| Every combination checked | Up to 17³ resolutions | A density or a surface may alias a token only some variants have. |
| An alias chain bound | 64 | Resolution recurses; a hostile file must not exhaust the stack. |
| The token model | No toolkit | The terminal renderer and the server use it as well. |
| How Qt Quick gets the values | Properties set on the existing `Theme` singleton | Every component already binds to it; no new QML type is registered. |

## Not yet

- Surfaces resolved per subtree, and `{"pref": "density"}` choosing a density at runtime.
- Units from the scale tokens rather than from the default font; `control.*`, `font.*` and `elevation.*` in the closed
  set.
- Icons, the terminal renderer's palette mapping.
