# Theme tokens — design

An application's look is data: a theme is a set of **DTCG 2025.10** token files, the format a design tool exports,
served in the bundle, checked when the bundle is registered, resolved on the client into the closed `ui-theme/1`
token set, and applied to the renderer. This implements the token parts of spec 6 §6
(`docs/superpowers/specs/2026-10-08-app-hosting-design.md`).

| Header | What it holds |
|---|---|
| `include/morph/ui/theme.hpp` | `parseTokens`, `resolveTokens`, `parseThemeIndex`, `resolveTheme`, `checkTheme`, `colorOf`, `kThemeTokens`, `themeValues`, `unreadThemeTokens`, `themeProblems` |
| `include/morph/ui/service.hpp` | `UiRegistry` checks themes at `addFile` and `build` |
| `include/morph/qt_quick/theme.hpp` | `qt_quick::applyTheme` |

## Contents

- [Token files](#token-files)
- [Aliases and layering](#aliases-and-layering)
- [The theme index](#the-theme-index)
- [At registration](#at-registration)
- [What the client reads](#what-the-client-reads)
- [The closed set](#the-closed-set)
- [On Qt Quick](#on-qt-quick)
- [Design decisions](#design-decisions)
- [Not yet](#not-yet)

## Token files

A token file is a JSON object of groups and tokens. A **token** is an object with `$value`; a **group** is any other
object, and groups nest. A token's `$type` is its own or the nearest enclosing group's. `$description`,
`$extensions`, `$deprecated` and other `$` keys are ignored.

A token holds no tokens, as DTCG requires: an object with `$value` and a member that does not start with `$` is
refused, naming the token and the member. A name cannot be a token and a group at once in nested JSON, so a token
that has parts is written as a group whose **`$root`** member is the token. `$root` is the token at the group's own
path, so `color.ok.$root` is `color.ok`:

```json
{ "color": { "$type": "color",
             "ok": { "$root": { "$value": "#17602F" }, "bg": { "$value": "#E2F4E8" } } } }
```

gives `color.ok` and `color.ok.bg`, and a file that writes `"ok": { "$value": "#17602F" }` gives the same `color.ok`.
An alias names either spelling: `{color.ok.$root}` is read as `{color.ok}`. Across layers the later file wins, as for
any path. `$root` at the top of a file is refused, since there is no group path to name.

```json
{ "color": { "$type": "color",
             "surface": { "$value": { "colorSpace": "srgb", "components": [1, 1, 1], "hex": "#ffffff" } },
             "accent":  { "$value": "#2563EB" }, "focus": { "$value": "{color.accent}" } },
  "space": { "$type": "dimension", "2": { "$value": { "value": 8, "unit": "px" } } } }
```

`parseTokens` checks the shape of the simple types and of `shadow`, and accepts the other composite ones unchecked:

| `$type` | Value |
|---|---|
| `color` | `"#rgb"`, `"#rrggbb"`, `"#rrggbbaa"`, or the 2025.10 object: its `hex`, or `srgb` `components` from 0 to 1 with an optional `alpha` |
| `dimension` | `{"value": n, "unit": "px" \| "rem"}` |
| `duration` | `{"value": n, "unit": "ms" \| "s"}` |
| `number` | a number |
| `fontFamily` | a name, or a non-empty list of names |
| `fontWeight` | 1 to 1000, or a DTCG weight name (`semi-bold`, …) |
| `shadow` | `{"color", "offsetX", "offsetY", "blur", "spread"}` with a colour and four dimensions, or a non-empty list of them |
| `strokeStyle` | `solid` or `dashed` where the set reads it (`border.style`), refused at `checkTheme` otherwise; accepted unchecked elsewhere |
| `keyword` | one of the names the token's spec lists (`shape.iconOnly`, `font.overline.case`, `collapsible.marker`), refused at `checkTheme` otherwise; DTCG has no type for a switch, and `strokeStyle` names stroke dashes only, so this is a `ui-theme/1` type; accepted unchecked elsewhere |
| `typography`, `border`, `gradient`, `transition`, `cubicBezier` | accepted unchecked |

A token of another `$type` stays in the parsed set with its type and no value check; `themeValues` ignores a path
whose type is not the set's, and the report in [What the client reads](#what-the-client-reads) names it. Refused: text that is not a JSON object or nests deeper
than `kMaxJsonDepth`, a must-understand (`!`) name, a name with `.`, `{` or `}`, a member that is not an object, a
token with no type that is not a plain alias, a value that does not have its type's shape, `$extends`, and more than
16384 tokens, and a token that holds members. A colour in another colour space with no `hex` is refused rather than
converted.

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
`icons` maps icon names to `icon` files ([Icons](#icons)). Each list holds at most 16 names, and the icon set at most
256. `resolveTheme(index, files, variant, density,
surface)` layers `base`, then the variant, the density and the surface, each optional, and resolves.

## At registration

`UiRegistry::addFile` parses a `theme` file as an index when it has a string `vocab` and as a token file otherwise,
and refuses one that does not parse. `build` checks each index with `checkTheme`: every combination of a variant (or
none), a density (or none) and a surface (or none) over `base` must resolve from the bundle's token files, so a
cycle, a missing reference, or a file the index names that the bundle lacks refuses the bundle, naming the index and
the combination (`base + dark + density compact: …`). At most 17³ combinations are resolved.

## What the client reads

The closed set below is all that draws. A token file may hold more: a design tool's export carries tokens for other
platforms, and a palette that the set's tokens alias. `unreadThemeTokens(index, files)` names, over every file the
index lists, each token that is neither a path of the set nor reachable from one through aliases (a plain alias, or one
inside a composite value, followed transitively through every file that has the path), and each token of a `$type`
ui-theme/1 reads no token of. A palette token that a set token aliases is read; `color.sucess` is not. A path is
reported once, with the first file that has it.

The report is not a refusal: the application still starts. `UiRegistry::build` logs each at warn
(`[ui-serve] theme <index>: <file>: the token <path> is not a ui-theme/1 token and no token of the set refers to it,
so nothing draws it`, or `… is of type <t>, which ui-theme/1 does not read`), and `applyBundleTheme` returns them among
its problems, which the Qt Quick frontend logs. `themeProblems` gives both channels the same sentences.

A theme whose text cannot be read on its fills is reported the same way. `themeContrastProblems(values)` checks
`color.text` at 4.5:1 and `color.muted` at 3:1 on `color.hover`, `color.selection`, `color.track` and
`color.controlSurface`, each composited
over `color.surface` when it is translucent (for example `color.hover: color.text is 1.50:1 on it, below 4.5:1`), and
`themeProblems` runs it for the base and for each surface's resolved values. Muted gets 3:1 because the framework's
own defaults hold that (4.04:1 on `#e5e5ea`) and not 4.5:1; the check exists to catch a fill that makes text
disappear, not to restyle the defaults. The theme is still applied.

## Surfaces

A surface is a named set of colour roles a node and everything inside it draw with (`Common::surface`). Five are
built in, so an application gets the shell's look with no theme file; a theme index's surface of the same name
replaces one, and declares any other:

| Surface | Background | Colour roles |
|---|---|---|
| `page` | `color.background` | the theme's |
| `card` | `color.surface`, with `radius.card`, a border and `elevation.card` | the theme's |
| `sidebar` | `#1f2430` | text `#c9ccd6`, muted `#7d8396`, accent `#4c8dff`, selection, border, track and divider `#2f3646`, control fill `#2a3040`, control outline `#3a4152`, hover `#ffffff14`, the neutral tone `#7d8396` (its fill that at 15 %); the rest the theme's |
| `toolbar` | `color.surface`, with a border `#e3e5ea` | the theme's |
| `bar` | `color.surface`, with no border, radius or shadow; the bars `pinned` writes | the theme's |

A surface's `border.style` is read like any token of it. While it is `dashed`, the outline of a button, a badge and an input on the surface is stroked in dashes (a `MorphDashedOutline`) in place of the solid border; the surface's own background has no border to dash, except a card's and a toolbar's, which stay solid. A round button or badge (`shape: round`) draws the surface's background with its own corners, half its height, so no square shows behind it.

**In the client.** A bundle carries its theme files by id (`Bundle::themes`). Once a bundle is open and before the
shell mounts, the Qt Quick frontend applies it with `applyBundleTheme`: the index's `base`, and each of its surfaces
resolved with its layer; a bundle without a theme index restores the defaults. It does so unless the frontend is told
to ignore the bundle's theme (`FrontendConfig::bundleTheme`), for an application that applies its own. No variant or
density is chosen yet.

Every built-in surface sets every role `MorphScheme` declares, either bound to the theme's value or as a literal of
its own palette, so the dark sidebar, which holds the navigator's search field and pin buttons, is not drawn with the
light defaults (white controls under light text, a light outline, an invisible hover). `toolbar`'s border is its own
`#e3e5ea`.

A theme's surface is resolved with its layer over the theme (`resolveTheme(…, surface)`), so a role it does not set
is the theme's. Surfaces do not nest their colours: a card inside a sidebar is a card. A dialog and a drawer draw on
`card`, whatever surface encloses the node that declares them. A panel draws on `card` unless it names a surface; an
empty surface is the default; a panel on a card (a panel in a panel, a dialog or a drawer) draws its border and fill but
no shadow, since a card on a card has no elevation; a flat panel names `page`. A surface name that is neither
built in nor in the theme draws nothing and changes no colour.

## The closed set

`kThemeTokens` is the closed `ui-theme/1` set, each path with its kind and the `MorphUi` default:

| Paths | Kind |
|---|---|
| `color.surface`, `color.text`, `color.muted`, `color.accent`, `color.focus`, `color.border`, `color.selection` | colour |
| `color.ok`, `color.warn`, `color.err`, `color.info`, `color.neutral`, each with `.bg`, `.border` and `.dot`; `color.neutral` and its parts default to `#6e6e73`, `#6e6e7326`, `#6e6e73`, `#6e6e73`, so the neutral tone is drawn like the others, and no longer follows `color.muted` | colour |
| `color.hover` (`#1d1d1f14`): the fill under a hovered or pressed list item, ghost button, segmented option and menu entry; translucent, so it darkens whatever light surface it lies on by one step and keeps its text readable | colour |
| `color.controlSurface` (`#ffffff`): the fill of a text input, a text area, a select's field, a date input, a check box's box and a normal or danger-outline button; a scheme role, so a surface that sets only `color.surface` leaves its controls on it. A read-only input keeps its own fill | colour |
| `color.controlBorder` (`#c7c7cc`): the outline of those controls at rest; a theme that sets `color.border` alone no longer moves them. A focused control's outline stays `color.focus`, a toned one's its tone's border | colour |
| `color.divider` (`#dcdce0`): a table's row rules and the rules of a divided list; `color.border` stays the outline of a card, a panel, a popup and a table | colour |
| `color.scrim` (`#0000004d`, black at 30 %): the dim over the window behind a modal dialog or drawer; a sheet draws none | colour |
| `color.track` (`#e5e5ea`): the unfilled part of a progress bar, and the segmented select's track | colour |
| `color.table.header` (transparent) | colour |
| `table.cellPaddingX` (8), `table.cellPaddingY` (4): the inset of a table cell and of a header label on each axis; a row is as tall as its tallest cell plus twice the vertical inset, and at least `Theme.targetHeight − 8`. The terminal keeps its one-column gap between cells and no vertical padding | dimension, in px |
| `color.chart.1` … `color.chart.4` (`#0072b2`, `#c25400`, `#00805e`, `#a8508a`): a chart's series colours, in drawing order | colour |
| `opacity.stale` | number |
| `space.0` … `space.5`, defaults 0, 4, 8, 12, 16, 24 | dimension, in px |
| `color.background` (`#f2f3f7`) | colour |
| `radius.control` (6), `radius.card` (10), `radius.badge` (4: a tag, a chip and a select option), `control.height` (32) | dimension, in px |
| `control.touchHeight` (44, the common minimum touch target): a sibling of `control.height` | dimension, in px |
| `font.size.body`, `font.size.heading` (0: the platform font's size, and the body size ×1.3) | dimension, in px |
| `font.size.small` (0: the body size ×0.85, rounded): a table header's label, the `small` text role; `font.size.caption` (0: ×0.92, rounded): a field's caption above its control; `font.size.mono` (0: the body size): the `mono` role; `font.size.display` (0: ×1.7, rounded): the `display` role. Each derives from the body size, none from another new token, so what a theme leaves unset never depends on what else it sets | dimension, in px |
| `font.weight.strong` (700): the weight of every strong font (`strongFont`, `headingFont`, the table header, a section, a strong run) | font weight: a number from 1 to 1000 or a DTCG weight name (`semi-bold` is 600), read into `ThemeValues::numbers` |
| `font.family` (none: the platform font) | font family: a name, or a list of names in order of preference |
| `font.mono` (none: the platform's fixed-pitch font), the family of the `mono` text role: a sibling of `font.family`, as the set keeps one spelling per path | font family, as `font.family` |
| `border.style` (`solid`): how the outline of a button, a badge and an input is stroked on a surface, `solid` or `dashed`; read per surface, so one surface (an empty plate well) can draw dashed controls while the rest draw solid | stroke style: `solid` or `dashed`, read into `ThemeValues::strokes`; any other value is refused by `checkTheme` as `border.style: only solid and dashed are drawn` |
| `shape.iconOnly` (`default`): whether a push button or a button-style menu whose caption is an icon alone, and which gives no `shape` of its own, is drawn square (`square`, as `shape: square` on a button). It covers the head's `⋯`, the compact shell's ☰ and a screen's `⋯` menus. Read once for the whole theme, not per surface. The terminal reads no theme token and draws `[ ⋯ ]` either way | keyword: `default` or `square`, read into `ThemeValues::keywords`; any other value is refused by `checkTheme` as `shape.iconOnly: only default and square are drawn` |
| `font.overline.case` (`none`): how the `overline` text role draws. `none` draws it as `section` (body size, strong, muted); `uppercase` draws it at the small size, strong, muted, in capitals, letter-spaced by `0.05 × Theme.smallPixels`. The text is written as it reads and the frontend draws the capitals, so the accessible name stays as written. The terminal draws it bold and muted, as written, whatever the token | keyword: `none` or `uppercase`; any other value is refused as `font.overline.case: only none and uppercase are drawn` |
| `collapsible.marker` (`shown`): whether a collapsible section's header and a collapsible panel's header draw their ▸/▾ chevron (`shown`) or not (`none`). With `none` the header keeps everything else: it is still the whole row's toggle, at least a touch target tall, with its role, its name and its expanded state, so the state stays where assistive technology reads it. Read once for the whole theme. The terminal reads no theme token and keeps its `▸ `/`▾ ` heading | keyword: `shown` or `none`; any other value is refused as `collapsible.marker: only shown and none are drawn` |
| `elevation.card` (`#00000014`, offsets 0 and 2, blur 10, spread 0) | shadow; of a list of layers, the first |
| `elevation.popup` (`#00000024`, offsets 0 and 8, blur 24, spread 0): a menu's, a dropdown's and a date picker's shadow | shadow, as `elevation.card` |

`themeValues(resolved)` gives every path of the set the theme's value when the theme has it with the set's type, and
its default otherwise; a token outside the set is ignored. A dimension is read in px, and a `rem` is 16 px
(`kPixelsPerRem`): the renderers do not change the root font size. `ThemeValues` keeps colours, numbers and
dimensions in three maps, shadows in a fourth, font family lists (`families`) in a fifth, stroke styles (`strokes`) in a sixth, and keywords (`keywords`) in a seventh.

The six `space` tokens are the spacing scale: `space.<step>` is how far apart a `gap` or `padding` of that step puts
things (`docs/spec/ui/view_tree.md`, the spacing step). Other `space.*` paths, `control.*`, `font.*` and
`elevation.*` are resolved and checked like any token, and join the set with the components that read them.

## On Qt Quick

The `MorphUi` `Theme` singleton's colour roles and `staleOpacity` are writable properties whose values are the
`ui-theme/1` defaults. `qt_quick::applyTheme(engine, values)` sets them on the engine's `Theme` instance:

| `Theme` | Token |
|---|---|
| `text`, `muted`, `surface`, `border`, `neutral`, `selection`, `accent`, `focus`, `hover`, `track`, `controlSurface`, `controlBorder`, `divider` | `color.<same name>` |
| `neutralBg`, `neutralBorder`, `neutralDot` | `color.neutral.bg`, `color.neutral.border`, `color.neutral.dot` |
| `error`, `success`, `info`, `warn` | `color.err`, `color.ok`, `color.info`, `color.warn` |
| `successBg`, `successBorder`, `successDot` | `color.ok.bg`, `color.ok.border`, `color.ok.dot` |
| `warnBg`, `warnBorder`, `warnDot` | `color.warn.bg`, `color.warn.border`, `color.warn.dot` |
| `errorBg`, `errorBorder`, `errorDot` | `color.err.bg`, `color.err.border`, `color.err.dot` |
| `infoBg`, `infoBorder`, `infoDot` | `color.info.bg`, `color.info.border`, `color.info.dot` |
| `tableHeader` | `color.table.header` |
| `chart1` … `chart4` | `color.chart.1` … `color.chart.4` |
| `scrim` | `color.scrim` |
| `staleOpacity` | `opacity.stale` |
| `space` (six px lengths) | `space.0` … `space.5` |
| `background` | `color.background` |
| `radiusControl`, `radiusCard`, `radiusBadge`, `controlHeight`, `touchHeight`, `bodySize`, `headingSize` | `radius.control`, `radius.card`, `radius.badge`, `control.height`, `control.touchHeight`, `font.size.body`, `font.size.heading` |
| `tableCellPaddingX`, `tableCellPaddingY` | `table.cellPaddingX`, `table.cellPaddingY` |
| `smallSize`, `captionSize`, `monoSize`, `displaySize` | `font.size.small`, `font.size.caption`, `font.size.mono`, `font.size.display` |
| `strongWeight` | `font.weight.strong` |
| `cardShadowColor`, `cardShadowOffsetX`, `cardShadowOffsetY`, `cardShadowBlur`, `cardShadowSpread` | `elevation.card` |
| `popupShadowColor`, `popupShadowOffsetX`, `popupShadowOffsetY`, `popupShadowBlur`, `popupShadowSpread` | `elevation.popup` |
| `fontFamilies` | `font.family` |
| `monoFontFamilies` | `font.mono` |
| `iconOnlySquare` (true for `square`) | `shape.iconOnly` |
| `overlineCaps` (true for `uppercase`) | `font.overline.case` |
| `collapsibleMarker` (true for `shown`) | `collapsible.marker` |

**A chart's colours.** A series reads `chart1` to `chart4` of its widget's scheme in order; a reference line and a
marked point read their tone's colour (`muted` for neutral). Each default holds 3:1 against `color.surface` and
`color.background` (WCAG 1.4.11): 5.19, 4.60, 4.94 and 5.02:1 on white. No four colours stay apart from the four
tone colours under colour-vision deficiency, so a chart never tells series apart by colour alone: each also has its
own marker shape, a marked point a cross no series uses, and a clipped point an open triangle. A dark theme sets its
own four, as it sets every colour.

**Touch targets follow the width class.** While the window's width class is `compact`, the backend sets
`Theme.touchTargets`, and every control is drawn at `Theme.targetHeight`: at least `control.touchHeight`, else
`control.height`. A button's and an input's background, a text area (twice it) and a table row (8 px less) read it;
a check box and a tab button take it as their least height while `touchTargets` holds, their indicator and label
keeping their size. The width class stands in for touch input, and is not a density layer: it is a fact about the
window, not a choice. The terminal is unchanged: a cell is the target.

`Theme.bodyPixels`, `headingPixels`, `smallPixels`, `captionPixels`, `monoPixels` and `displayPixels` are the font
sizes the components use: a size of 0 derives from the platform font (`Theme.platformMetrics`), the body its size, a
heading 1.3 times the body, and the others as the closed set lists. Every `MorphUi` text and control draws in
`Theme.bodyFont` (`strongFont` where it is emphasised and for a `section` text, which is also muted, `headingFont`
for a heading, `smallFont` and `smallStrongFont` for a table header's label and a `small` text, `captionFont` for a
field's caption, `displayFont` for a `display` text): the theme's families at the font's size, or the platform font
untouched while `fontFamilies` is empty. Every strong font draws at `Theme.strongWeight`. A family Qt cannot resolve
falls back to Qt's own substitute, and `applyTheme` reports it (`font.family: none of <names> is installed`),
comparing `QFontInfo`'s family against the list.

A `mono` text draws in `Theme.monoFont`: `Theme.monoFamilies` at `monoPixels`, regular weight. `monoFamilies` is the
theme's `font.mono` (`monoFontFamilies`), or, while that is empty, the family of the platform's fixed-pitch font
(`QFontDatabase::systemFont(QFontDatabase::FixedFont)`), which the backend writes as `Theme.platformMonoFamily` when
it starts and on every `applyTheme`. A `font.mono` Qt cannot resolve is reported as `font.family`'s is
(`font.mono: none of <names> is installed`). The size is `font.size.mono`, the body size while it is unset.

**Units follow the theme's font, opt-in.** `Theme.metrics`, which the units read, is the theme's families at
`bodyPixels` while `fontFamilies` is set and the platform font otherwise, so the units and every `fixed(n)` are
unchanged until a theme names a family. `bodyPixels` derives from the platform metrics rather than from `metrics`, so
the size never depends on the metrics it feeds.

Every `MorphUi` component binds to `Theme`, and a host component that imports `MorphUi` reads the same instance in
the same engine, so a theme reaches both, and applying another theme later updates what is mounted. It returns the
properties it could not set. Every colour row is also a role of each surface's `MorphScheme`, so a surface's own
tokens set it there. A progress bar reads two of them: its fill is `color.accent` on a `color.track` track. `MorphWidget`'s `toneBgOf`,
`toneBorderOf` and `toneDotOf` answer a tone's fill, outline and marker in a widget's scheme (the neutral tone's are
`color.neutral.bg`, `.border` and `.dot`); a filled badge fills with them, and an outline badge, a chip and a banner outline with them.

## Icons

A theme index's `icons` is the bundle's icon set: icon name (`isIconName`: `[a-z][a-z0-9.-]{0,63}`) to the id of an
`icon` file, SVG text of at most 64 KiB (`kMaxIconBytes`). `UiRegistry::build` refuses an index whose set names a
file the bundle lacks or one that is not an `icon` file, naming the index and the icon. The client loader fills
`ui::Bundle::icons` with each named icon's SVG.

- **Framework names.** The built-in components draw `empty` (an empty state without an icon of its own),
  `tone.info`, `tone.ok`, `tone.warn`, `tone.err` (a banner's tone), `nav.expand` and `nav.collapse` (a collapsible's
  and a panel's chevron, and a select's dropdown indicator), `close` (a dismissible banner's close button) and
  `date.pick` (a date input's calendar button), `more` (an overflow menu's trigger, `⋯`) and `nav.menu` (the compact
  shell frame's navigation menu, `☰`), from the set when it names them, and as today's glyph or nothing
  otherwise, so a bundle without a set keeps today's look. `nav.back` and `nav.forward` are framework names no
  built-in component draws yet (`kFrameworkIcons`).
  Qt Quick draws them as a vector chevron when the set does not name them: a push button with no label whose icon
  is `nav.back` or `nav.forward` (a pager's buttons) shows a `MorphChevron`, in the button's caption colour.
- **On Qt Quick.** `applyBundleTheme(engine, themes, icons)` hands the set to `Theme.icons` (name to a
  `data:image/svg+xml` URL) and `Theme.iconSvgs` (name to SVG text) and sets `Theme.iconSet`;
  `Theme.iconSource(name)` is the URL, or empty. It also writes `Theme.iconGlyphs`, name to glyph for every
  framework name that has one (`iconGlyph`, the glyphs the terminal draws), whether or not the bundle has a set. A button, a
  badge, an empty state, a menu entry and a single-line or password text input's leading `icon` draw a node's icon
  from the set when it names it, and today's text otherwise (a badge's and a menu entry's name before the label, a
  button and a text input nothing — a text input with no themed icon for its name shows no leading space at all).
  An icon-only push button the set does not name is never blank: with no label it draws, in order, the set's icon, a
  built-in chevron (`nav.back`, `nav.forward`), `Theme.iconGlyphs[name]`, and its accessible name.
- **`currentColor`.** A themed SVG that paints with `currentColor` is drawn in the colour of the control that shows it:
  `Theme.iconSourceIn(name, color)` answers the URL with every `currentColor` replaced by the colour's `#rrggbb`,
  cached by name and colour. A push button and a menu trigger draw it in their caption colour (white on a dark
  head through a ghost button's accent), a menu entry in its text colour, a badge in its label's colour and a text
  input's leading icon in the accent. An SVG without `currentColor` is drawn as it is, so an icon set that never
  uses it keeps its look.
  While the bundle declares a set, a node icon it does not name is reported once per name through the `Components`
  callback (`icon <name> is not in the theme's icon set`); without a set nothing is reported.
- **On the terminal.** Icons stay text: a framework name is drawn as its glyph (`iconGlyph`: `tone.ok` ✓, `tone.warn`
  !, `tone.err` ✗, `tone.info` i, `nav.expand` ▸, `nav.collapse` ▾, `close` ×, `date.pick` ▦, `more` ⋯, `nav.menu`
  ☰) before a badge's or a
  button's text, or a single-line or password text input's own text, and any other name is not drawn.

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

- `{"pref": "density"}` choosing a density at runtime.
- Units from the scale tokens rather than from the body font; `control.*` and `elevation.*` beyond those above in the
  closed set.
- The terminal renderer's palette mapping.
