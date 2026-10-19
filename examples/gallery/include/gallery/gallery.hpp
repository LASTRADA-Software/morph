// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/forms/flows.hpp>
#include <morph/forms/views.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// The gallery: one screen that uses every node kind the interpreter lowers, served by a small application, as a
/// visual check for renderer authors (spec 1 §8).

namespace gallery {

/// @brief One row the gallery's lists show.
struct Fact {
    /// @brief The row's key.
    std::int64_t id = 0;
    /// @brief What it is.
    std::string label;
    /// @brief Its value.
    std::string value;
};

/// @brief Lists the facts.
struct ListFacts {};

/// @brief Adds a fact: the gallery's `form`, and its wizard's first step.
struct AddFact {
    /// @brief What it is.
    std::string label;
    /// @brief Its value.
    std::string value;

    /// @brief Whether both are given.
    /// @return `true` when neither is empty.
    [[nodiscard]] bool validate() const { return !label.empty() && !value.empty(); }
};

/// @brief Confirms the fact the wizard's first step added: its second step.
struct ConfirmFact {
    /// @brief The fact, bound from the first step's reply.
    std::int64_t factId = 0;
    /// @brief Whether to keep it.
    bool keep = true;

    /// @brief Whether a fact is named.
    /// @return `true` for a non-zero id.
    [[nodiscard]] bool validate() const { return factId != 0; }
};

/// @brief The gallery's one model: a fixed list of facts, so a query crosses the bridge in every mode. Adding a fact
///        answers with the fact and keeps nothing: the gallery shows the forms engine's nodes, not a store.
struct GalleryModel {
    /// @brief The facts.
    /// @param action The request.
    /// @return Three facts.
    [[nodiscard]] std::vector<Fact> execute(ListFacts const& action) const;

    /// @brief Echoes a fact under the next id.
    /// @param action The fact.
    /// @return The fact, as it would be listed.
    [[nodiscard]] Fact execute(AddFact const& action) const;

    /// @brief Confirms a fact.
    /// @param action The confirmation.
    /// @return Whether it is kept.
    [[nodiscard]] bool execute(ConfirmFact const& action) const;
};

/// @brief The facts as the forms engine's collection: `ListFacts`' rows as a table.
struct FactsView {
    /// @brief A list of rows.
    using kind = ::morph::views::CollectionView;
    /// @brief The action that lists them.
    using query = ListFacts;
    /// @brief The collection's title.
    static constexpr std::string_view title = "Facts";
};

/// @brief Adding a fact as a wizard: add it, then confirm it.
using FactWizard = ::morph::flows::Wizard<
    "Add a fact", ::morph::flows::WizardStep<AddFact, "Fact">,
    ::morph::flows::WizardStep<ConfirmFact, "Confirm", ::morph::flows::Bind<"factId", "Gallery_AddFact.id">>>;

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "gallery";

/// @brief The screen's id.
inline constexpr std::string_view kHomeScreen = "gallery.home";

/// @brief The application's bundle: the gallery screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

/// @brief The gallery as a linked application, for a local-mode client.
/// @return It.
[[nodiscard]] morph::client::LocalApp localApp();

}  // namespace gallery
