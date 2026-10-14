// SPDX-License-Identifier: Apache-2.0
//
// Fixtures shared by the UI service's server and client tests: a small
// application's documents, a registry over them, and an authoriser whose
// refusals a test sets per principal.
#pragma once

#include <map>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace uiservice {

/// A screen whose top-level query every principal may run.
inline constexpr std::string_view kResults =
    R"({"vocab":"ui/1","id":"lims.results","models":{"sample":{"type":"Lims_Sample"}},)"
    R"("queries":{"list":{"model":"sample","action":"Lims_List"}},)"
    R"("mutations":{"save":{"model":"sample","action":"Lims_Save"}},"view":{"kind":"text","text":"Results"}})";

/// A screen whose top-level query only a verifier may run, with a nested scope's query and a model-bound form.
inline constexpr std::string_view kVerify =
    R"({"vocab":"ui/1","id":"lims.verify","models":{"sample":{"type":"Lims_Sample"}},)"
    R"("queries":{"pending":{"model":"sample","action":"Lims_Pending"}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"dialog","id":"audit","open":false,"scope":{"models":{"audit":{"type":"Lims_Audit"}},)"
    R"("queries":{"trail":{"model":"audit","action":"Lims_Trail"},"again":{"model":"sample","action":"Lims_Pending"}}},)"
    R"("children":[{"kind":"text","text":"a"}]},)"
    R"({"kind":"tabs","tabs":[{"label":"One","children":[{"kind":"form","model":"sample","action":"Lims_Verify"}]}]}]}})";

/// A screen only a verifier may run, listed disabled for everyone else.
inline constexpr std::string_view kReport =
    R"({"vocab":"ui/1","id":"lims.report","models":{"sample":{"type":"Lims_Sample"}},)"
    R"("queries":{"report":{"model":"sample","action":"Lims_Report"}},"view":{"kind":"text","text":"Report"}})";

/// The login screen: public, with no query.
inline constexpr std::string_view kLogin =
    R"({"vocab":"ui/1","id":"auth.login","models":{"auth":{"type":"Auth_Session"}},)"
    R"("mutations":{"login":{"model":"auth","action":"Auth_Login"}},"view":{"kind":"text","text":"Sign in"}})";

/// The app document.
inline constexpr std::string_view kApp = R"({"vocab":"ui/1","id":"app","models":{"prefs":{"type":"Prefs"}},)"
                                         R"("queries":{"settings":{"model":"prefs","action":"Prefs_Get"}}})";

/// A delivered component.
inline constexpr std::string_view kChart = "Rectangle { color: \"teal\" }";

/// A theme.
inline constexpr std::string_view kTheme = R"({"color":{"accent":{"$type":"color","$value":"#00aa66"}}})";

/// Refuses, per principal, the (model type, action) pairs a test names; allows everything else.
struct RuleAuthorizer final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        auto const found = denied.find(ctx.principal);
        return found == denied.end() ||
               !found->second.contains(std::pair{std::string{modelType}, std::string{actionType}});
    }

    /// The token is the principal's name.
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        if (ctx.token.empty()) {
            return std::nullopt;
        }
        return ctx.token;
    }

    /// Refusals by principal; the empty principal is a caller who has not signed in.
    std::map<std::string, std::set<std::pair<std::string, std::string>>> denied;
};

/// The facts of the fixture's models and actions.
inline morph::ui::Catalog facts() {
    morph::ui::Catalog catalog;
    catalog.model = [](std::string_view typeId) -> std::optional<morph::ui::ModelInfo> {
        static std::set<std::string, std::less<>> const known = {"Lims_Sample", "Lims_Audit", "Auth_Session", "Prefs"};
        if (!known.contains(typeId)) {
            return std::nullopt;
        }
        return morph::ui::ModelInfo{.stateless = typeId == "Prefs", .keyed = typeId == "Lims_Sample"};
    };
    catalog.action = [](std::string_view actionId) -> std::optional<morph::ui::ActionInfo> {
        static std::map<std::string, std::string, std::less<>> const owners = {
            {"Lims_List", "Lims_Sample"},   {"Lims_Save", "Lims_Sample"},   {"Lims_Pending", "Lims_Sample"},
            {"Lims_Trail", "Lims_Audit"},   {"Lims_Verify", "Lims_Sample"}, {"Lims_Report", "Lims_Sample"},
            {"Auth_Login", "Auth_Session"}, {"Prefs_Get", "Prefs"}};
        auto const found = owners.find(actionId);
        if (found == owners.end()) {
            return std::nullopt;
        }
        return morph::ui::ActionInfo{.modelType = found->second,
                                     .calculatedFields = {},
                                     .secretFields = {},
                                     .credential = actionId == "Auth_Login"};
    };
    return catalog;
}

/// A catalog entry.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the id, then how a refusal shows it.
inline morph::ui::CatalogEntry entry(std::string id, std::string whenDenied = "hide", bool isPublic = false) {
    morph::ui::CatalogEntry made;
    made.id = std::move(id);
    made.whenDenied = std::move(whenDenied);
    made.isPublic = isPublic;
    return made;
}

/// A bundle file.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): kind, id, bytes, in the order a manifest lists them.
inline morph::ui::BundleFile file(std::string_view kind, std::string id, std::string bytes, bool isPublic = false) {
    return morph::ui::BundleFile{
        .kind = std::string{kind}, .id = std::move(id), .bytes = std::move(bytes), .isPublic = isPublic};
}

/// The fixture's registry: four screens, a host screen, one not built, the app document and a theme.
inline morph::ui::UiRegistry registry(morph::ui::Catalog catalog = facts()) {
    morph::ui::UiRegistry made{morph::ui::ApplicationInfo{.id = "lims", .version = "1.4.0"}, std::move(catalog)};
    auto must = [](auto const& result) {
        if (!result) {
            throw std::runtime_error{result.error().message()};
        }
    };
    must(made.addScreen(entry("lims.results"), std::string{kResults}));
    must(made.addScreen(entry("lims.verify"), std::string{kVerify}));
    must(made.addScreen(entry("lims.report", "disable"), std::string{kReport}));
    must(made.addScreen(entry("auth.login", "hide", true), std::string{kLogin}));
    morph::ui::CatalogEntry host = entry("lims.scanner");
    host.host = "Scanner";
    must(made.addEntry(host));
    morph::ui::CatalogEntry later = entry("lims.billing");
    later.available = false;
    later.reason = "coming in 2.0";
    must(made.addEntry(later));
    must(made.addFile(uiservice::file(morph::ui::kAppFile, "app", std::string{kApp})));
    must(made.addFile(uiservice::file(morph::ui::kThemeFile, "default", std::string{kTheme}, true)));
    must(made.addFile(uiservice::file(morph::ui::kComponentFile, "Chart", std::string{kChart})));
    made.requireVocabulary("ui-theme/1");
    return made;
}

/// The fixture's bundle.
inline std::shared_ptr<morph::ui::ServedBundle const> bundle() {
    auto built = registry().build();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    return *built;
}

/// An authoriser under which `tech` may not run the verifier's actions, and a caller who has not signed in may only
/// say hello and fetch what is public.
inline std::shared_ptr<RuleAuthorizer> authorizer() {
    auto made = std::make_shared<RuleAuthorizer>();
    made->denied["tech"] = {
        {"Lims_Sample", "Lims_Pending"}, {"Lims_Sample", "Lims_Report"}, {"Lims_Sample", "Lims_Verify"}};
    made->denied[""] = {{"ui", "catalog"}, {"ui", "fetch"}};
    return made;
}

}  // namespace uiservice
