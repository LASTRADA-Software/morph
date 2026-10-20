// SPDX-License-Identifier: Apache-2.0

#include "probe_app.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>

BRIDGE_REGISTER_MODEL(probe::ProbeModel, "Probe_Model")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Answer, "Probe_Answer")
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Login, "Probe_Login")
BRIDGE_REGISTER_EFFECTS(probe::Login, ::morph::effects::none)
BRIDGE_REGISTER_ACTION(probe::ProbeModel, probe::Attach, "Probe_Attach")
BRIDGE_REGISTER_EFFECTS(probe::Attach, ::morph::effects::none)

namespace probe {

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
std::int64_t ProbeModel::execute(Answer const& /*action*/) const { return 42; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
LoginReply ProbeModel::execute(Login const& /*action*/) const { return LoginReply{.user = "ada", .jwt = "t-1"}; }

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the bridge calls an action on a model instance.
std::int64_t ProbeModel::execute(Attach const& action) const {
    return static_cast<std::int64_t>(action.content.size());
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
