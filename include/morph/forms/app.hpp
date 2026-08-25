// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/app.hpp
/// @brief App-shell descriptor: menu -> screens (form / wizard / view refs).
///
/// A `morph::app::App` names an ordered menu and a map of screen-id ->
/// screen, where a screen is a reference to an already-registered action
/// form (`FormScreen`) or wizard (`WizardScreen`). `appSchemaJson<AppT>()`
/// emits the `app-*` document a renderer loads as its navigation root,
/// replacing "enumerate every schema on one scroll." Purely additive
/// metadata: no new dispatch path, the shell only routes to existing
/// action-forms and wizards. See docs/spec/forms/workflows_navigation.md.

#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <tuple>

#include "flows.hpp"

namespace morph::app {

/// @brief One `app-menu` entry: a label and the screen-id it routes to.
/// @tparam Label    Human label shown in the menu.
/// @tparam ScreenId Key into the app's `screens` map.
template <morph::forms::FixedString Label, morph::forms::FixedString ScreenId>
struct MenuEntry {
    /// @brief The menu entry's display label.
    /// @return The declared label.
    [[nodiscard]] static constexpr std::string_view label() noexcept { return Label.view(); }

    /// @brief The screen-id this entry routes to.
    /// @return The declared screen-id.
    [[nodiscard]] static constexpr std::string_view screen() noexcept { return ScreenId.view(); }
};

/// @brief A screen backed by one registered action's form.
/// @tparam Id     Screen-id, referenced from an `App::menu`'s `MenuEntry::screen()`.
/// @tparam Action Registered action type (`BRIDGE_REGISTER_ACTION`) this screen renders.
template <morph::forms::FixedString Id, typename Action>
struct FormScreen {
    /// @brief The screen's id.
    /// @return The declared id.
    [[nodiscard]] static constexpr std::string_view id() noexcept { return Id.view(); }

    /// @brief The screen's kind, for the `app-screens[id].kind` key.
    /// @return The literal `"form"`.
    [[nodiscard]] static constexpr std::string_view kind() noexcept { return "form"; }

    /// @brief The referenced action's registered type-id.
    /// @return `ActionTraits<Action>::typeId()`.
    [[nodiscard]] static constexpr std::string_view ref() noexcept {
        return ::morph::model::ActionTraits<Action>::typeId();
    }
};

/// @brief A screen backed by a registered `morph::flows::Wizard`.
/// @tparam Id     Screen-id, referenced from an `App::menu`'s `MenuEntry::screen()`.
/// @tparam Wizard Registered wizard type (`BRIDGE_REGISTER_WIZARD`) this screen renders.
template <morph::forms::FixedString Id, typename Wizard>
struct WizardScreen {
    /// @brief The screen's id.
    /// @return The declared id.
    [[nodiscard]] static constexpr std::string_view id() noexcept { return Id.view(); }

    /// @brief The screen's kind, for the `app-screens[id].kind` key.
    /// @return The literal `"wizard"`.
    [[nodiscard]] static constexpr std::string_view kind() noexcept { return "wizard"; }

    /// @brief The referenced wizard's registered type-id.
    /// @return `WizardTraits<Wizard>::typeId()`.
    [[nodiscard]] static constexpr std::string_view ref() noexcept {
        return ::morph::flows::WizardTraits<Wizard>::typeId();
    }
};

// A ViewScreen<Id, View> counterpart (kind: "view") belongs here once
// docs/planned/gui_collections_views.md's ViewTraits<V> exists. appSchemaJson
// below only requires each screen type to expose id()/kind()/ref(), so adding
// it later needs no change to appSchemaJson itself — this reference demo
// therefore only exercises "form" and "wizard" screens (see
// docs/spec/forms/workflows_navigation.md's Limitations).

/// @brief The app-shell descriptor: a title, an ordered menu, and the screens it routes to.
/// @tparam Title   Application title (window/header).
/// @tparam Menu    A `std::tuple<MenuEntry<...>...>` of ordered menu entries.
/// @tparam Screens A `std::tuple<FormScreen<...> | WizardScreen<...>...>` of screens.
template <morph::forms::FixedString Title, typename Menu, typename Screens>
struct App {
    /// @brief Tuple of this app's ordered `MenuEntry<...>` types.
    using menu = Menu;

    /// @brief Tuple of this app's screen descriptor types.
    using screens = Screens;

    /// @brief The application's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief Traits specialisation mapping an `App` type to its string type-id.
///
/// Specialise via `BRIDGE_REGISTER_APP` rather than by hand. The default is a
/// forward declaration — using it without a specialisation is an
/// incomplete-type error.
/// @tparam A Concrete `App<...>` type.
template <typename A>
struct AppTraits;  // forward — specialise or use BRIDGE_REGISTER_APP

/// @brief Generates the `app-*` JSON document for app-shell type @p AppT.
///
/// Emits `app-title`, an ordered `app-menu` array (`{label, screen}` per
/// entry), and an `app-screens` object mapping each screen's id to
/// `{kind, ref}`. See docs/spec/forms/workflows_navigation.md for the full
/// key vocabulary.
/// @tparam AppT Concrete `morph::app::App<Title, Menu, Screens>` type.
/// @return The app's JSON document. Empty string only if glaze's own JSON
///         writer fails on the assembled DOM (schema generation never throws).
template <typename AppT>
[[nodiscard]] std::string appSchemaJson() {
    glz::generic_u64 dom{};
    dom["app-title"] = std::string{AppT::title()};

    glz::generic_u64::array_t menu{};
    ::morph::flows::detail::forEachTupleElement<typename AppT::menu>([&]<typename Entry, std::size_t I>() {
        static_cast<void>(I);
        glz::generic_u64 entry{};
        entry["label"] = std::string{Entry::label()};
        entry["screen"] = std::string{Entry::screen()};
        menu.emplace_back(std::move(entry));
    });
    dom["app-menu"] = menu;

    auto& screensNode = dom["app-screens"];
    ::morph::flows::detail::forEachTupleElement<typename AppT::screens>([&]<typename S, std::size_t I>() {
        static_cast<void>(I);
        auto& screenNode = screensNode[std::string{S::id()}];
        screenNode["kind"] = std::string{S::kind()};
        screenNode["ref"] = std::string{S::ref()};
    });

    return glz::write_json(dom).value_or(std::string{});
}

}  // namespace morph::app

/// @brief Specialises `morph::app::AppTraits<A>` with the string type-id @p NAME.
///
/// Metadata only, exactly like `BRIDGE_REGISTER_WIZARD`: an app-shell
/// descriptor is never itself executed, only the actions/wizards its screens
/// reference are (see docs/spec/forms/workflows_navigation.md).
/// @param A    Concrete `morph::app::App<...>` type.
/// @param NAME String literal used as the app's type-id.
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define BRIDGE_REGISTER_APP(A, NAME)                                         \
    template <>                                                              \
    struct morph::app::AppTraits<A> {                                        \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
// clang-format on
