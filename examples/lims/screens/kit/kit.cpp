// SPDX-License-Identifier: Apache-2.0

#include "kit/kit.hpp"

#include <map>
#include <string>

namespace lims::screens::kit {

namespace {

/// @brief A module's name where the catalogue has none.
/// @param module The module's id.
/// @return Its English name.
[[nodiscard]] std::string moduleName(std::string_view module) {
    static std::map<std::string_view, std::string_view> const names{
        {"lab", "Laboratory"},      {"billing", "Billing"},  {"ledger", "Ledger"},
        {"inventory", "Inventory"}, {"org", "Organisation"}, {"shell", "Home"},
    };
    auto const found = names.find(module);
    return std::string{found == names.end() ? module : found->second};
}

}  // namespace

void pageHead(ui::Scope& screen, std::string_view module, ui::AnyHandle const& title) {
    screen.eyebrow(ui::t("lims.module." + std::string{module}, moduleName(module)));
    screen.title(title);
}

ui::Handle<std::string> balance(ui::AnyHandle const& amount, ui::AnyHandle const& currency) {
    return ui::cond(amount < 0,
                    ui::concat({money(ui::abs(amount), currency), ui::AnyHandle{" "}, ui::t("lims.credit", "Cr")}),
                    money(amount, currency));
}

ui::Handle<std::string> personName(ui::AnyHandle const& users, ui::AnyHandle const& principal) {
    // Only rows that carry a name are kept, so the first of them is the person's name and an empty list (no row, or a
    // row with no name) falls through to the login.
    auto const named = ui::filter<UserView>(users, "person", [&principal](ui::Handle<UserView> const& person) {
        return ui::eq(person.at(&UserView::id), principal) && ui::ne(person.at(&UserView::displayName), "");
    });
    return ui::Handle<std::string>::raw(
        ui::coalesce({ui::call("first", {named}).at("displayName"), principal}).json());
}

ui::Node refusalBanner(ui::AnyHandle const& refusal) {
    return ui::banner({.tone = ui::AnyHandle{"err"}, .text = ui::refusalText(refusal)}).visible(ui::engaged(refusal));
}

}  // namespace lims::screens::kit
