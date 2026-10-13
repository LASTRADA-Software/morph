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

namespace probe {

std::int64_t ProbeModel::execute(Answer const& /*action*/) const { return 42; }

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

morph::examples::LocalApp app() { return morph::examples::LocalApp{.name = "probe", .bundle = &bundle, .setUp = {}}; }

}  // namespace probe
