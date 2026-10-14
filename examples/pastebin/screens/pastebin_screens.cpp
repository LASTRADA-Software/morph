// SPDX-License-Identifier: Apache-2.0

#include "pastebin_screens.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>

#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"

namespace pastebin::screens {

namespace ui = morph::ui::author;

namespace {

// Pastes: creating one refetches the list. Opening one consumes one of its allowed reads, so it is a mutation, never
// a query a refetch could repeat, and an `exclusive` one: a second open while one is in flight is refused.
ui::Screen pastesScreen() {
    return ui::screen(std::string{kPastes}, [](ui::Scope& screen) {
        screen.title("Pastes");
        auto const pastes = screen.model<PasteModel>("pastes", {.instance = ui::Instance::Private});
        auto const list = screen.query<ListPastes>("list", pastes);
        auto const open = screen.mutation<GetPaste>("open", pastes, {.concurrency = ui::Mode::Exclusive});
        auto const opened = screen.let("opened", open.result());
        auto const remove = screen.mutation<DeletePaste>("remove", pastes, {.invalidates = {list}});
        auto const rows = list.value().at(&ListPastesResult::pastes);
        return ui::column(
            {ui::form<CreatePaste>(pastes, {.onSuccess = {ui::refetch(list)}})
                 .id(ui::element("createForm"))
                 .testId("createForm"),
             ui::table(
                 rows,
                 {.key = ui::row("id"),
                  .columns = {{.name = "id", .label = "Paste", .cell = ui::text({.text = ui::row("id")})},
                              {.name = "syntax", .label = "Syntax", .cell = ui::text({.text = ui::row("syntax")})},
                              {.name = "open",
                               .label = "",
                               .cell = ui::button(
                                   {.label = "Open", .onClick = ui::run(open, {{&GetPaste::id, ui::row("id")}})})},
                              {.name = "delete",
                               .label = "",
                               .cell =
                                   ui::button({.label = "Delete",
                                               .onClick = ui::run(remove, {{&DeletePaste::id, ui::row("id")}})})}}})
                 .a11y("Pastes")
                 .testId("pastes"),
             ui::text({.text = ui::format("{0} pastes", {ui::count(rows)})}).testId("count"),
             ui::panel(
                 {ui::text({.text = opened.at(&PasteView::content)}).testId("content"),
                  ui::text({.text = ui::format("{0}, read {1} times",
                                               {opened.at(&PasteView::syntax),
                                                ui::call<std::string>(
                                                    "decimal", {opened.at(&PasteView::readCount), std::int64_t{0}})})})
                      .testId("reads")},
                 {.title = "Opened"})
                 .visible(ui::engaged(opened)),
             ui::text({.text = ui::coalesce({list.error(), open.error(), remove.error()})}).testId("error")},
            {.gap = 8});
    });
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(pastesScreen());
    if (!registered) {
        return std::unexpected(std::move(registered.error()));
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kPastes};
    entry.title = "Pastes";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace pastebin::screens
