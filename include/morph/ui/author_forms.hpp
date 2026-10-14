// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <tuple>

#include "../forms/flows.hpp"
#include "../forms/views.hpp"
#include "author.hpp"

/// @file
/// The `collection` and `wizard` builders that carry their forms documents (spec 5 §11): a view descriptor's `v-*`
/// document and a wizard's `w-*` document go into the node's catalog facts, so the bundle serves them with the
/// `ui-catalog` reply and a client builds the node without knowing the types. Separate from `author.hpp` so the
/// builders that need no forms documents do not pull in the forms headers.

namespace morph::ui::author {

/// @brief A view descriptor: a `views::` kind and the registered query action it lists.
template <typename V>
concept ViewDescriptor = requires {
    typename V::kind;
    typename V::query;
};

/// @brief A `flows::Wizard`: a title and its steps.
template <typename W>
concept WizardDescriptor = requires {
    typename W::steps;
    W::title();
};

/// @brief The action of a wizard's first step, which names the wizard in a document.
/// @tparam W The wizard.
template <WizardDescriptor W>
using FirstStepAction = std::tuple_element_t<0, typename W::steps>::action;

/// @brief A `collection` node over a view descriptor: the node names the view's query action, and the view's
///        document travels with the node's catalog facts.
/// @tparam View  The view descriptor.
/// @tparam Model The model type of the alias; it must run the view's query.
/// @param model The alias.
/// @return The node.
template <ViewDescriptor View, typename Model>
    requires Runs<Model, typename View::query>
[[nodiscard]] Node collection(ModelHandle<Model> const& model) {
    using Query = View::query;
    CatalogFacts facts;
    facts.action<Model, Query>();
    facts.views.emplace(std::string{::morph::model::ActionTraits<Query>::typeId()},
                        ::morph::views::viewSchemaJson<View>());
    return detail::NodeWriter{"collection"}
        .text("model", model.alias())
        .text("action", std::string{::morph::model::ActionTraits<Query>::typeId()})
        .facts(facts)
        .done();
}

/// @brief A `wizard` node over a `flows::Wizard`: the node names the first step's action, and the wizard's document
///        travels with the node's catalog facts.
/// @tparam Wizard The wizard.
/// @tparam Model  The model type of the alias; it must run the first step.
/// @param model The alias.
/// @return The node.
template <WizardDescriptor Wizard, typename Model>
    requires Runs<Model, FirstStepAction<Wizard>>
[[nodiscard]] Node wizard(ModelHandle<Model> const& model) {
    using First = FirstStepAction<Wizard>;
    CatalogFacts facts;
    facts.action<Model, First>();
    facts.wizards.emplace(std::string{::morph::model::ActionTraits<First>::typeId()},
                          ::morph::flows::wizardSchemaJson<Wizard>());
    return detail::NodeWriter{"wizard"}
        .text("model", model.alias())
        .text("action", std::string{::morph::model::ActionTraits<First>::typeId()})
        .facts(facts)
        .done();
}

}  // namespace morph::ui::author
