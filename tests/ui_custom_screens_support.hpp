// SPDX-License-Identifier: Apache-2.0
//
// Fixtures of the custom screens tests: a base screen that uses one of every capability the base rule compares,
// and a loader that refuses to go on with a fixture that does not load.

#pragma once
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>

#include "ui_service_support.hpp"

namespace screens_test {

/// The base screen, under @p id. It runs `save` on mount, offers `verify` and an upload behind buttons, calls the
/// host command `print`, and declares a delivered component (`Chart`, the fixture bundle's) and a host component.
inline std::string baseScreen(std::string_view id) {
    return R"({"vocab":"ui/1","id":")" + std::string{id} +
           R"(","models":{"sample":{"type":"Lims_Sample"}},)"
           R"("queries":{"list":{"model":"sample","action":"Lims_List"}},)"
           R"("mutations":{"save":{"model":"sample","action":"Lims_Save"},)"
           R"("verify":{"model":"sample","action":"Lims_Verify"}},)"
           R"("onMount":[{"run":"save"}],)"
           R"("components":{"Chart":{"qml":")" +
           morph::ui::contentHash(uiservice::kChart) +
           R"(","props":{},"fallback":{"kind":"text","text":"chart"}},)"
           R"("Grid":{"host":"ResultGrid","version":2,"props":{},"fallback":{"kind":"text","text":"grid"}}},)"
           R"("view":{"kind":"column","children":[)"
           R"({"kind":"text","text":"Results"},)"
           R"({"kind":"button","label":"Print","onClick":[{"host":"print"}]},)"
           R"({"kind":"button","label":"Verify","onClick":[{"run":"verify"}]},)"
           R"({"kind":"button","label":"Attach","onClick":[{"upload":"/tmp/a.csv","to":"save","as":"ref"}]}]}})";
}

/// @p text with its first @p from replaced by @p to; fails the test when @p from is absent, so a fixture edit that
/// no longer matches is caught rather than silently testing the unchanged base.
inline std::string replaced(std::string text, std::string_view from, std::string_view to) {
    auto const at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// @p text loaded against the fixture's catalog. A fixture that does not load is a fixture bug: fix its JSON
/// against docs/spec/ui/document.md, never the check that refused it.
inline morph::ui::Document load(std::string const& text) {
    auto loaded = morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = uiservice::facts()});
    if (!loaded) {
        FAIL("fixture does not load: " << loaded.error().message());
    }
    return std::move(*loaded);
}

}  // namespace screens_test
