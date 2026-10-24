// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "layout.hpp"
#include "lims_gallery/lims_gallery.hpp"
#include "lims_gallery/screens.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;

// One design's entry: its number, name and summary, and a button that opens it, or a tag when it is not built.
ui::Node entry(Design const& design, ui::AnyHandle const& width) {
    std::string const number{design.number};
    ui::Node const action =
        design.make != nullptr
            ? ui::button({.label = "Open",
                          .onClick = ui::navigate(std::string{design.id},
                                                  {.params = ui::obj({{std::string{kWidthParam}, width}})})})
                  .a11y(std::string{"Open "} + std::string{design.title})
                  .testId("open-" + number)
            : layout::tag("Not built yet", "neutral").testId("unavailable-" + number);
    return ui::row(
               {layout::muted(number),
                ui::column({layout::section(std::string{design.title}), layout::muted(std::string{design.summary})},
                           {.gap = 0})
                    .sizing(ui::stretch()),
                action},
               {.gap = 2, .align = layout::Align::Center})
        .testId("design-" + number);
}

}  // namespace

ui::Screen indexScreen() {
    return ui::screen(std::string{kIndexScreen}, [](ui::Scope& screen) {
        screen.title("LIMS gallery");
        // The width a test asked for is passed on to the design it opens.
        auto const width = screen.param<std::optional<std::string>>(std::string{kWidthParam});
        std::vector<ui::Node> entries;
        for (Design const& design : designs()) {
            entries.push_back(entry(design, width));
        }
        return ui::column({layout::muted("Ten laboratory screens written as ui/1 documents. Each keeps its sample "
                                         "records in its own state; there is no backend."),
                           ui::column(entries, {.gap = 2}).testId("designs")},
                          {.gap = 3});
    });
}

}  // namespace lims_gallery
