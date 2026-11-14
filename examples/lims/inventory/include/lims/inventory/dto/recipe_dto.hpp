// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <string>
#include <vector>

#include "lims/inventory/core/types.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Recipes: how much of an item one analysis uses, per company. The analysis is lab's; inventory refers to it by the
/// shared id and knows nothing else about it.

namespace lims {

/// @brief Declares that one analysis uses a quantity of an item. Setting the same analysis and item again replaces the
///        quantity.
struct SetRecipe {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company whose stock the analysis uses.
    CompanyId company;
    /// @brief The analysis definition.
    AnalysisId analysis;
    /// @brief The item, of the company.
    ItemId item;
    /// @brief How much one analysis uses, in @p unit.
    Measure quantity;
    /// @brief The unit @p quantity is in: any unit of the item's dimension.
    StockUnit unit = StockUnit::piece;

    /// @brief Whether the recipe is well-formed.
    /// @return `true` when a company, an analysis, an item and a quantity are named; whether the quantity is positive
    ///         and in the item's dimension is the model's to judge.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && analysis.hasValue() && item.hasValue() && quantity.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One line of an analysis's recipe.
struct RecipeView {
    /// @brief Its id.
    RecipeId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The analysis definition.
    AnalysisId analysis;
    /// @brief The item.
    ItemId item;
    /// @brief The item's code.
    std::string itemCode;
    /// @brief How much one analysis uses, in @p unit.
    Measure quantity;
    /// @brief The item's unit.
    StockUnit unit = StockUnit::piece;
};

/// @brief Lists a company's recipes.
struct ListRecipes {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief A company's recipes, by analysis then item code.
struct ListRecipesResult {
    /// @brief The recipes.
    std::vector<RecipeView> recipes;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::RecipeView, "Inventory_Recipe")
