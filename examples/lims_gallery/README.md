# LIMS gallery

Ten laboratory screens from a set of design mockups (an e-signature, sample receipt, an instrument record, an audit
trail, …), written as ui/1 documents with the C++ authoring API. The point is to find what morph's declarative UI can
and cannot express: where a design asks for something ui/1 has no word for, the screen uses the closest honest
workaround and the gap is recorded.

There is no backend. Each screen keeps its sample records in its own `state`, and its actions only change that state.

## Running it

```sh
cmake --build build/<preset> --target morph_examples_client
morph_examples_client --app lims-gallery --ui=qt
```

The first screen is the index, which lists every design and opens the ones that are built. `--screen <id>` opens one
directly, such as `--screen lims-gallery.esignature`.

Each design has an expanded layout (a window of 1200 px or more) and a compact one (under 600 px). A screen reads its
width class from an optional `width` param before `{"env": "widthClass"}`, so the tests and the render smoke mount it
at either width.

## Layout

- `src/screens/` holds one file per screen; `designs.cpp` is the one list of designs, which the bundle registers and
  the index shows. A design is built by giving its entry the screen's builder.
- `tests/` holds one test file per design, over `GalleryScreen` (`tests/gallery_harness.hpp`), which mounts a screen
  by id at a width class on the recording backend. `lims_gallery_render_smoke` mounts every screen at both widths in
  Qt Quick, offscreen.

## The gap log

The findings are kept in a gap log beside this work rather than here: one entry per need a design has that ui/1
cannot meet, with the evidence, the workaround the screen uses, and how much it matters to a LIMS. It is the input
for framework changes, which are made in batches from it, not in this example.
