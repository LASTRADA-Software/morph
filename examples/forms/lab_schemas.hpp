// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The `{actionType: schema}` object shared by every forms client (HTML page,
/// REPL `--schemas`, QML renderer). One source; all renderers see the same
/// contract. A remote deployment would serve this from a `describe` endpoint
/// instead of compiling it in. `viewsJson()` is the equivalent `{viewType:
/// viewSchema}` object for the view-schema layer (docs/spec/forms/views.md).

#include <array>
#include <morph/forms/views.hpp>
#include <string>
#include <string_view>

#include "lab_model.hpp"
#include "lab_wizard.hpp"

namespace lab {

/// @brief Schemas of every LabModel action rendered as its own standalone
///        form, keyed by action type id. A pure query/options-provider
///        action (`ListSamples`, `ListCountries`, `ListCities`) is not
///        included here — it never renders as its own form, only as a
///        `Choice`'s `x-optionsAction` target or (`ListSamples`) a view's
///        `v-query` — exactly the existing convention for the two other
///        options providers.
[[nodiscard]] inline std::string schemasJson() {
    std::string out;
    out += "{\"ComputeDryDensity\":";
    out += morph::forms::schemaJson<ComputeDryDensity>();
    out += ",\"RecordMeasurement\":";
    out += morph::forms::schemaJson<RecordMeasurement>();
    out += ",\"ShippingAddress\":";
    out += morph::forms::schemaJson<ShippingAddress>();
    out += ",\"EditSample\":";
    out += morph::forms::schemaJson<EditSample>();
    out += ",\"DeleteSample\":";
    out += morph::forms::schemaJson<DeleteSample>();
    out += ",\"CreateSample\":";
    out += morph::forms::schemaJson<CreateSample>();
    out += ",\"RegisterSample\":";
    out += morph::forms::schemaJson<RegisterSample>();
    out += "}";
    return out;
}

/// @brief `{wizardId: schema}` JSON for every registered wizard the demo's
///        app shell references — parallel to `schemasJson` for actions.
[[nodiscard]] inline std::string wizardSchemasJson() {
    std::string out;
    out += "{\"IntakeWizard\":";
    out += morph::flows::wizardSchemaJson<IntakeWizard>();
    out += "}";
    return out;
}

/// @brief The `app-*` document for the demo's app shell (menu -> screens).
[[nodiscard]] inline std::string appSchemaJson() { return morph::app::appSchemaJson<LabApp>(); }

/// @brief View descriptor for the sample list/master-detail screen (E-G7,
///        docs/spec/forms/views.md): lists `ListSamples`' rows, opens
///        `EditSample` (prefilled with the row's id) on row activation,
///        deletes a row (confirm-guarded), and creates a new one.
///
/// `kEditBind` binds only `id`, matching docs/spec/forms/views.md's own
/// `v-rowAction` example (`"bind": { "id": "id" }`) — deliberately, not just
/// for parity: binding *every* required field of `EditSample` (id **and**
/// name) would make the Qt/QML reference renderer (`DynamicForm`'s
/// auto-fire-on-ready, no submit button) fire `EditSample` the instant the
/// row opens, before the user changes anything, since both required fields
/// would already be engaged. Binding only the row key leaves `name` blank
/// for the user to (re)type, so the edit fires only once they actually enter
/// a value — see docs/spec/forms/views.md, "Limitations".
struct SamplesView {
    using kind = morph::views::CollectionView;
    using query = ListSamples;

    static constexpr std::string_view title = "Samples";

    static constexpr std::array<morph::views::BindEntry, 1> kEditBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr auto rowAction =
        morph::views::describeAction<EditSample>({}, morph::views::ActionScope::Row, kEditBind);

    static constexpr std::array<morph::views::BindEntry, 1> kDeleteBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr std::array<morph::views::ActionDescriptor, 2> actions{
        morph::views::describeAction<DeleteSample>("Delete", morph::views::ActionScope::Row, kDeleteBind, true),
        morph::views::describeAction<CreateSample>("New", morph::views::ActionScope::Collection),
    };
};

/// @brief Views of every registered LabModel view, keyed by view type id.
[[nodiscard]] inline std::string viewsJson() {
    std::string out;
    out += "{\"SamplesView\":";
    out += morph::views::viewSchemaJson<SamplesView>();
    out += "}";
    return out;
}

}  // namespace lab

using lab::SamplesView;

BRIDGE_REGISTER_VIEW(SamplesView, "SamplesView")
