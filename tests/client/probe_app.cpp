// SPDX-License-Identifier: Apache-2.0

#include "probe_app.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>

BRIDGE_REGISTER_MODEL(probe::ProbeModel, "Probe_Model")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Answer, "Probe_Answer")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Login, "Probe_Login")
BRIDGE_REGISTER_EFFECTS(probe::Login, ::morph::effects::none)
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Scopes, "Probe_Scopes")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Search, "Probe_Search")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Count, "Probe_Count")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Attach, "Probe_Attach")
BRIDGE_REGISTER_EFFECTS(probe::Attach, ::morph::effects::none)

namespace probe {

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
std::int64_t ProbeModel::execute(Answer const& /*action*/) const { return 42; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
LoginReply ProbeModel::execute(Login const& /*action*/) const { return LoginReply{.user = "ada", .jwt = "t-1"}; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
ScopeList ProbeModel::execute(Scopes const& /*action*/) const {
    return ScopeList{
        .options = {ScopeOption{.label = "North", .value = "n"}, ScopeOption{.label = "South", .value = "s"}}};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
SearchHits ProbeModel::execute(Search const& /*action*/) const {
    return SearchHits{.hits = {SearchHit{.screen = "probe.width", .title = "Width"},
                               SearchHit{.screen = "probe.second", .title = "Second"}}};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
CountReply ProbeModel::execute(Count const& /*action*/) const { return CountReply{.n = 3}; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
std::int64_t ProbeModel::execute(Attach const& action) const {
    return static_cast<std::int64_t>(action.content.size());
}

morph::ui::Catalog searchCatalog() {
    morph::ui::author::CatalogFacts facts;
    facts.action<ProbeModel, Search>();
    facts.action<ProbeModel, Count>();
    return facts.catalog();
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.home", kHome}, std::pair{"probe.other", kOther}}) {
        morph::ui::CatalogEntry entry;
        entry.id = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

morph::client::LocalApp app() { return morph::client::LocalApp{.name = "probe", .bundle = &bundle, .setUp = {}}; }

}  // namespace probe
