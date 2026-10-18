// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Loads every screen and app document of a served bundle against the catalog facts the bundle serves, as a client
/// does, and lists what the loader reports: each refusal and each load-time warning (`Document::warnings`). A rung's
/// test asserts the list is empty, so a mutation that refreshes nothing, a refresh declaration its action's effects
/// already cover, or a query whose action declares effects fails the rung.

namespace morph::examples::testing {

/// @brief The refusals and warnings of loading every screen and app document of @p bundle.
/// @param bundle The bundle, as the rung's server serves it.
/// @return One line per refusal or warning, naming its document; empty when every document loads cleanly.
[[nodiscard]] inline std::vector<std::string> bundleLoadFindings(morph::ui::ServedBundle const& bundle) {
    morph::ui::Catalog catalog;
    if (bundle.models) {
        catalog.model = [models = *bundle.models](std::string_view type) -> std::optional<morph::ui::ModelInfo> {
            auto const found = models.find(type);
            return found == models.end() ? std::nullopt : std::optional{found->second};
        };
    }
    if (bundle.actions) {
        catalog.action = [actions = *bundle.actions](std::string_view id) -> std::optional<morph::ui::ActionInfo> {
            auto const found = actions.find(id);
            return found == actions.end() ? std::nullopt : std::optional{found->second};
        };
    }
    std::vector<std::string> findings;
    if (!catalog.action) {
        findings.emplace_back("the bundle carries no action facts, so nothing could be checked");
        return findings;
    }
    for (auto const& [hash, file] : bundle.files) {
        bool document = false;
        for (morph::ui::ServedFile::Holder const& holder : file.holders) {
            document = document || holder.kind == morph::ui::kScreenFile || holder.kind == morph::ui::kAppFile;
        }
        if (!document) {
            continue;
        }
        auto const loaded = morph::ui::loadDocumentText(file.bytes, morph::ui::LoadOptions{.catalog = catalog});
        if (!loaded) {
            findings.push_back(loaded.error().message());
            continue;
        }
        for (morph::ui::LoadWarning const& warning : loaded->warnings) {
            findings.push_back(warning.message(loaded->id));
        }
    }
    return findings;
}

}  // namespace morph::examples::testing
