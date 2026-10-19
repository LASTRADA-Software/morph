// SPDX-License-Identifier: Apache-2.0

// The server side of the UI service: the registry's refusals and the bundle it
// builds, and UiService's three kinds with their gates, the catalog filter and
// the fetch rules. The client and the end-to-end compatibility and security
// cases are in test_ui_service_client.cpp.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/service.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ui_service_support.hpp"

using morph::ui::CatalogEntry;
using morph::ui::contentHash;
using morph::ui::UiRegistry;
using morph::ui::UiService;
using morph::wire::Envelope;
using uiservice::entry;

namespace {

UiRegistry emptyRegistry() { return UiRegistry{morph::ui::ApplicationInfo{.id = "app", .version = "1"}}; }

/// The reason a registry refusal carries, or a marker when it was accepted.
template <typename Result>
std::string refused(Result const& result) {
    if (result) {
        return "<accepted>";
    }
    return result.error().reason;
}

/// A request of @p kind from @p principal.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the principal, then the body.
Envelope request(std::string_view kind, std::string principal, std::string body = {}) {
    Envelope env;
    env.kind = std::string{kind};
    env.callId = 42;
    env.session.principal = std::move(principal);
    env.body = std::move(body);
    return env;
}

/// Asks @p service and decodes its reply.
Envelope ask(UiService& service, morph::session::IAuthorizer const& authorizer, Envelope const& env) {
    return morph::wire::decode(service.handle(env, authorizer));
}

std::string fetchBody(std::string const& hash) { return R"({"hash":")" + hash + R"("})"; }

std::string helloBody(std::vector<std::string> const& vocabularies) {
    std::string out = R"({"vocabularies":[)";
    for (std::size_t index = 0; index < vocabularies.size(); ++index) {
        out += (index == 0 ? "\"" : ",\"") + vocabularies[index] + "\"";
    }
    return out + "]}";
}

std::vector<std::string> ids(morph::ui::CatalogReply const& reply) {
    std::vector<std::string> out;
    out.reserve(reply.entries.size());
    for (CatalogEntry const& listed : reply.entries) {
        out.push_back(listed.id);
    }
    return out;
}

CatalogEntry const& listed(morph::ui::CatalogReply const& reply, std::string_view id) {
    auto const found = std::ranges::find(reply.entries, id, &CatalogEntry::id);
    REQUIRE(found != reply.entries.end());
    return *found;
}

}  // namespace

// ── The registry ────────────────────────────────────────────────────────────

TEST_CASE("UiRegistry builds a manifest of every file, sorted, with its digest", "[ui-service][registry]") {
    auto const bundle = uiservice::bundle();
    auto const manifest = morph::ui::parseManifest(bundle->manifest);
    REQUIRE(manifest);
    REQUIRE(manifest->application == "lims");
    REQUIRE(manifest->version == "1.4.0");
    REQUIRE(bundle->digest == contentHash(bundle->manifest));
    std::vector<std::string> listedFiles;
    for (auto const& file : manifest->files) {
        listedFiles.push_back(file.kind + ":" + file.id);
        REQUIRE(bundle->files.at(file.hash).bytes.size() == file.size);
        REQUIRE(contentHash(bundle->files.at(file.hash).bytes) == file.hash);
    }
    REQUIRE(listedFiles == std::vector<std::string>{"app:app", "component:Chart", "screen:auth.login",
                                                    "screen:lims.report", "screen:lims.results", "screen:lims.verify",
                                                    "theme:default"});
    REQUIRE(manifest->find("screen", "auth.login")->isPublic);
    REQUIRE(bundle->files.at(bundle->digest).bytes == bundle->manifest);
    REQUIRE(bundle->vocabularies == std::vector<std::string>{"expr/1", "ui-theme/1", "ui/1"});
    REQUIRE(bundle->screens.size() == 6U);
}

TEST_CASE("UiRegistry's digest is a function of the files alone", "[ui-service][registry]") {
    auto const first = uiservice::bundle();
    auto const second = uiservice::bundle();
    REQUIRE(first->digest == second->digest);
    REQUIRE(first->manifest == second->manifest);

    UiRegistry changed = uiservice::registry();
    // One changed byte, inside the token's value so the theme stays a valid token file.
    std::string theme{uiservice::kTheme};
    theme.replace(theme.find("#00aa66"), 7, "#00aa67");
    REQUIRE(changed.addFile(uiservice::file(morph::ui::kThemeFile, "changed", theme)));
    auto const rebuilt = changed.build();
    REQUIRE(rebuilt);
    REQUIRE((*rebuilt)->digest != first->digest);
}

TEST_CASE("UiRegistry names every action and model type a document names, at any scope", "[ui-service][registry]") {
    auto const bundle = uiservice::bundle();
    auto const verify = std::ranges::find(bundle->screens, std::string{"lims.verify"},
                                          [](morph::ui::ServedScreen const& screen) { return screen.entry.id; });
    REQUIRE(verify != bundle->screens.end());
    REQUIRE(verify->topQueries ==
            std::vector<morph::ui::ActionRef>{{.modelType = "Lims_Sample", .action = "Lims_Pending"}});
    REQUIRE(verify->actions ==
            std::vector<morph::ui::ActionRef>{{.modelType = "Lims_Audit", .action = "Lims_Trail"},
                                              {.modelType = "Lims_Sample", .action = "Lims_Pending"},
                                              {.modelType = "Lims_Sample", .action = "Lims_Verify"}});
    REQUIRE(verify->modelTypes == std::vector<std::string>{"Lims_Audit", "Lims_Sample"});
    REQUIRE(bundle->appActions == std::vector<morph::ui::ActionRef>{{.modelType = "Prefs", .action = "Prefs_Get"}});
    REQUIRE(bundle->allActions.size() == 8U);
    REQUIRE(bundle->models->size() == 4U);
    REQUIRE(bundle->actions->at("Auth_Login").credential);
}

TEST_CASE("UiRegistry without catalog lookups serves no facts", "[ui-service][registry]") {
    auto const built = uiservice::registry(morph::ui::Catalog{}).build();
    REQUIRE(built);
    REQUIRE_FALSE((*built)->models);
    REQUIRE_FALSE((*built)->actions);
}

TEST_CASE("UiRegistry::addScreen refuses what it cannot serve", "[ui-service][registry]") {
    UiRegistry registry = emptyRegistry();
    std::string const results{uiservice::kResults};
    REQUIRE(registry.addScreen(entry("lims.results"), results));

    SECTION("an entry without an id") {
        REQUIRE(refused(registry.addScreen(entry(""), results)) == "a catalog entry has an id");
    }
    SECTION("an id longer than the bound") {
        REQUIRE(refused(registry.addScreen(entry(std::string(257, 'x')), results)) ==
                "a catalog entry id is longer than 256 bytes");
    }
    SECTION("an unknown whenDenied") {
        REQUIRE(refused(registry.addScreen(entry("x", "grey"), results)) ==
                "`whenDenied` is `hide` or `disable`, not `grey`");
    }
    SECTION("an id registered twice") {
        REQUIRE(refused(registry.addScreen(entry("lims.results"), results)) ==
                "a catalog entry with this id is registered");
    }
    SECTION("a host screen") {
        CatalogEntry host = entry("lims.other");
        host.host = "Other";
        REQUIRE(refused(registry.addScreen(host, results)) ==
                "a host screen has no document; it is added with addEntry");
    }
    SECTION("a document the loader refuses") {
        auto const result = registry.addScreen(entry("t"), R"({"vocab":"ui/1","id":"t","view":{"kind":"nope"}})");
        REQUIRE_FALSE(result);
        REQUIRE(result.error().document == "t");
        REQUIRE(result.error().path == "/view/kind");
    }
    SECTION("a document with an unknown must-understand key") {
        auto const result = registry.addScreen(
            entry("t"), R"({"vocab":"ui/1","id":"t","!signed":true,"view":{"kind":"text","text":"x"}})");
        REQUIRE_FALSE(result);
        REQUIRE(result.error().reason == "the must-understand key `!signed` is not part of ui/1");
    }
    SECTION("an entry that is not the document's") {
        REQUIRE(refused(registry.addScreen(entry("other"), results)) ==
                "the entry's id is not the document's id `lims.results`");
    }
    SECTION("the app document") {
        REQUIRE(refused(registry.addScreen(entry("app"), std::string{uiservice::kApp})) ==
                "the app document is added with addFile");
    }
    SECTION("a document larger than the bound") {
        std::string big = R"({"vocab":"ui/1","id":"big","view":{"kind":"text","text":")";
        big += std::string(morph::ui::kMaxFileBytes, 'x') + R"("}})";
        REQUIRE(refused(registry.addScreen(entry("big"), big)) == "the file is larger than 4194304 bytes");
    }
}

TEST_CASE("UiRegistry::addEntry lists only host screens and unavailable ones", "[ui-service][registry]") {
    UiRegistry registry = emptyRegistry();
    REQUIRE(refused(registry.addEntry(entry("plain"))) ==
            "an entry without a document is a host screen or unavailable");
    CatalogEntry host = entry("scan");
    host.host = "Scanner";
    REQUIRE(registry.addEntry(host));
    REQUIRE(refused(registry.addEntry(host)) == "a catalog entry with this id is registered");
    REQUIRE(refused(registry.addEntry(entry("x", "maybe"))) == "`whenDenied` is `hide` or `disable`, not `maybe`");
}

TEST_CASE("UiRegistry::addFile refuses what it cannot serve", "[ui-service][registry]") {
    UiRegistry registry = emptyRegistry();
    REQUIRE(refused(registry.addFile(uiservice::file(morph::ui::kScreenFile, "s", "{}"))) ==
            "a file is an app document, a theme, a message catalogue or a component, not `screen`");
    REQUIRE(refused(registry.addFile(uiservice::file(morph::ui::kThemeFile, "", "{}"))) == "a file has an id");
    REQUIRE(refused(registry.addFile(uiservice::file(morph::ui::kThemeFile, std::string(257, 't'), "{}"))) ==
            "a file id is longer than 256 bytes");
    REQUIRE(registry.addFile(uiservice::file(morph::ui::kThemeFile, "dark", "{}")));
    REQUIRE(registry.addFile(uiservice::file(morph::ui::kThemeFile, "light", "{}")));
    REQUIRE(refused(registry.addFile(uiservice::file(morph::ui::kThemeFile, "dark", "{}"))) ==
            "the theme `dark` is already added");
    REQUIRE(registry.addFile(uiservice::file(morph::ui::kComponentFile, "dark", "Item {}")));
    REQUIRE(refused(registry.addFile(
                uiservice::file(morph::ui::kMessagesFile, "en", std::string(morph::ui::kMaxFileBytes + 1, 'm')))) ==
            "the file is larger than 4194304 bytes");
    // Within the file bound, but six bytes on the wire for each control byte.
    REQUIRE(refused(registry.addFile(
                uiservice::file(morph::ui::kMessagesFile, "de", std::string(morph::ui::kMaxFileBytes, '\x01')))) ==
            "the file does not fit one envelope once escaped");
    auto const notJson = registry.addFile(uiservice::file(morph::ui::kAppFile, "app", "{"));
    REQUIRE_FALSE(notJson);
    REQUIRE(notJson.error().reason.starts_with("not JSON"));
    REQUIRE(refused(registry.addFile(uiservice::file(
                morph::ui::kAppFile, "app", R"({"vocab":"ui/1","id":"t","view":{"kind":"text","text":"x"}})"))) ==
            "the app document's id is `app`");
}

TEST_CASE("UiRegistry::build refuses a bundle it cannot serve", "[ui-service][registry]") {
    std::string const chart = "Rectangle {}";
    std::string const screen =
        R"({"vocab":"ui/1","id":"chart","components":{"Chart":{"qml":")" + contentHash(chart) +
        R"(","props":{},"fallback":{"kind":"text","text":"chart"}},)"
        R"("Grid":{"host":"ResultGrid","version":2,"props":{},"fallback":{"kind":"text","text":"grid"}}},)"
        R"("view":{"kind":"text","text":"x"}})";

    SECTION("a delivered component that is not in the bundle") {
        UiRegistry registry = emptyRegistry();
        REQUIRE(registry.addScreen(entry("chart"), screen));
        auto const built = registry.build();
        REQUIRE_FALSE(built);
        REQUIRE(built.error().document == "chart");
        REQUIRE(built.error().path == "/components/Chart/qml");
        REQUIRE(built.error().reason == "the delivered component is not in the bundle");
        REQUIRE(registry.addFile(uiservice::file(morph::ui::kComponentFile, "Chart", chart)));
        REQUIRE(registry.build());
    }
    SECTION("more files than the bound") {
        UiRegistry registry = emptyRegistry();
        for (std::size_t index = 0; index <= morph::ui::kMaxBundleFiles; ++index) {
            CatalogEntry later = entry("e" + std::to_string(index));
            later.available = false;
            REQUIRE(registry.addEntry(later));
        }
        REQUIRE(refused(registry.build()) == "the bundle holds more than 4096 files or entries");
    }
    SECTION("more files and entries together than the bound") {
        UiRegistry registry = emptyRegistry();
        for (std::size_t index = 0; index < morph::ui::kMaxBundleFiles; ++index) {
            CatalogEntry later = entry("e" + std::to_string(index));
            later.available = false;
            REQUIRE(registry.addEntry(later));
        }
        REQUIRE(registry.addFile(uiservice::file(morph::ui::kThemeFile, "t", "{}")));
        REQUIRE(refused(registry.build()) == "the bundle holds more than 4096 files or entries");
    }
    SECTION("a vocabulary longer than the bound") {
        UiRegistry registry = emptyRegistry();
        registry.requireVocabulary(std::string(257, 'v'));
        REQUIRE(refused(registry.build()) == "a vocabulary is longer than 256 bytes");
    }
}

// ── The service ─────────────────────────────────────────────────────────────

TEST_CASE("UiService serves the four ui kinds and nothing else", "[ui-service]") {
    UiService const service{uiservice::bundle()};
    REQUIRE(service.serves("ui-hello"));
    REQUIRE(service.serves("ui-catalog"));
    REQUIRE(service.serves("ui-fetch"));
    REQUIRE(service.serves("ui-evaluate"));
    REQUIRE_FALSE(service.serves("ui-file"));
    REQUIRE_FALSE(service.serves("hello"));
    REQUIRE_THROWS_AS(UiService{nullptr}, std::invalid_argument);
}

TEST_CASE("ui-hello names the application and the digest, and refuses a client missing a vocabulary", "[ui-service]") {
    auto const bundle = uiservice::bundle();
    UiService service{bundle};
    auto const authorizer = uiservice::authorizer();

    SECTION("a client that speaks every vocabulary") {
        Envelope const reply =
            ask(service, *authorizer, request("ui-hello", "", helloBody({"ui/1", "expr/1", "ui-theme/1", "files/1"})));
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.callId == 42U);
        auto const hello = morph::ui::parseHelloReply(reply.body);
        REQUIRE(hello);
        REQUIRE(hello->application == "lims");
        REQUIRE(hello->version == "1.4.0");
        REQUIRE(hello->digest == bundle->digest);
        REQUIRE(hello->vocabularies == bundle->vocabularies);
    }
    SECTION("a client missing vocabularies is told which") {
        Envelope const reply = ask(service, *authorizer, request("ui-hello", "", helloBody({"ui/1"})));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "missing vocabulary: expr/1, ui-theme/1");
    }
    SECTION("a refused gate") {
        authorizer->denied[""].insert({"ui", "hello"});
        Envelope const reply = ask(service, *authorizer, request("ui-hello", "", helloBody({"ui/1"})));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "unauthorized");
    }
    SECTION("a body it cannot read") {
        Envelope const reply = ask(service, *authorizer, request("ui-hello", "", R"({"!push":true})"));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "the must-understand key `!push` in the ui-hello request is not understood");
    }
}

TEST_CASE("ui-catalog is filtered for the principal", "[ui-service][catalog]") {
    UiService service{uiservice::bundle()};
    auto const authorizer = uiservice::authorizer();

    SECTION("a verifier sees every screen") {
        Envelope const reply = ask(service, *authorizer, request("ui-catalog", "vera"));
        REQUIRE(reply.kind == "ok");
        auto const catalog = morph::ui::parseCatalogReply(reply.body);
        REQUIRE(catalog);
        REQUIRE(ids(*catalog) == std::vector<std::string>{"lims.results", "lims.verify", "lims.report", "auth.login",
                                                          "lims.scanner", "lims.billing"});
        REQUIRE(listed(*catalog, "lims.report").available);
        REQUIRE_FALSE(listed(*catalog, "lims.billing").available);
        REQUIRE(listed(*catalog, "lims.billing").reason == "coming in 2.0");
        REQUIRE(listed(*catalog, "lims.scanner").host == "Scanner");
        REQUIRE(catalog->permitted.size() == 8U);
        REQUIRE(catalog->models->contains("Lims_Audit"));
        REQUIRE(catalog->actions->contains("Lims_Verify"));
    }
    SECTION("a technician does not see what it may not run") {
        Envelope const reply = ask(service, *authorizer, request("ui-catalog", "tech"));
        auto const catalog = morph::ui::parseCatalogReply(reply.body);
        REQUIRE(catalog);
        // lims.verify hides; lims.report is listed disabled, as `denied`.
        REQUIRE(ids(*catalog) ==
                std::vector<std::string>{"lims.results", "lims.report", "auth.login", "lims.scanner", "lims.billing"});
        REQUIRE_FALSE(listed(*catalog, "lims.report").available);
        REQUIRE(listed(*catalog, "lims.report").reason == "denied");
        REQUIRE(std::ranges::find(catalog->permitted, "Lims_Verify") == catalog->permitted.end());
        REQUIRE(std::ranges::find(catalog->permitted, "Lims_List") != catalog->permitted.end());
        // The facts cover only the documents it may fetch.
        REQUIRE_FALSE(catalog->models->contains("Lims_Audit"));
        REQUIRE_FALSE(catalog->actions->contains("Lims_Report"));
        REQUIRE(catalog->actions->contains("Auth_Login"));
        REQUIRE(catalog->actions->contains("Prefs_Get"));
    }
    SECTION("a refused gate") {
        Envelope const reply = ask(service, *authorizer, request("ui-catalog", ""));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "unauthorized");
    }
    SECTION("a bundle without facts sends none") {
        auto const built = uiservice::registry(morph::ui::Catalog{}).build();
        REQUIRE(built);
        UiService bare{*built};
        auto const catalog = morph::ui::parseCatalogReply(ask(bare, *authorizer, request("ui-catalog", "vera")).body);
        REQUIRE(catalog);
        REQUIRE_FALSE(catalog->models);
        REQUIRE_FALSE(catalog->actions);
    }
}

TEST_CASE("ui-fetch serves a file by its hash, behind its gate", "[ui-service][fetch]") {
    auto const bundle = uiservice::bundle();
    UiService service{bundle};
    auto const authorizer = uiservice::authorizer();
    auto const manifest = morph::ui::parseManifest(bundle->manifest);
    REQUIRE(manifest);
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a manifest is keyed by kind, then id.
    auto hashOf = [&](std::string_view kind, std::string_view id) { return manifest->find(kind, id)->hash; };

    SECTION("the manifest, by the digest, before sign-in") {
        Envelope const reply = ask(service, *authorizer, request("ui-fetch", "", fetchBody(bundle->digest)));
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.body == bundle->manifest);
    }
    SECTION("a public screen and a public theme, before sign-in") {
        Envelope const login =
            ask(service, *authorizer, request("ui-fetch", "", fetchBody(hashOf("screen", "auth.login"))));
        REQUIRE(login.kind == "ok");
        REQUIRE(login.body == uiservice::kLogin);
        Envelope const theme =
            ask(service, *authorizer, request("ui-fetch", "", fetchBody(hashOf("theme", "default"))));
        REQUIRE(theme.body == uiservice::kTheme);
    }
    SECTION("a public file behind a refused ui-hello gate") {
        authorizer->denied[""].insert({"ui", "hello"});
        Envelope const reply = ask(service, *authorizer, request("ui-fetch", "", fetchBody(bundle->digest)));
        REQUIRE(reply.message == "unauthorized");
    }
    SECTION("a screen that is not public, before sign-in") {
        Envelope const reply =
            ask(service, *authorizer, request("ui-fetch", "", fetchBody(hashOf("screen", "lims.results"))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message == "unauthorized");
    }
    SECTION("a screen, signed in") {
        Envelope const reply =
            ask(service, *authorizer, request("ui-fetch", "tech", fetchBody(hashOf("screen", "lims.results"))));
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.body == uiservice::kResults);
    }
    SECTION("the app document, signed in") {
        Envelope const reply = ask(service, *authorizer, request("ui-fetch", "tech", fetchBody(hashOf("app", "app"))));
        REQUIRE(reply.body == uiservice::kApp);
    }
    SECTION("a screen the catalog hides or disables for the principal") {
        for (std::string_view const id : {"lims.verify", "lims.report"}) {
            Envelope const reply =
                ask(service, *authorizer, request("ui-fetch", "tech", fetchBody(hashOf("screen", id))));
            REQUIRE(reply.kind == "err");
            REQUIRE(reply.message == "unauthorized");
        }
        Envelope const allowed =
            ask(service, *authorizer, request("ui-fetch", "vera", fetchBody(hashOf("screen", "lims.verify"))));
        REQUIRE(allowed.kind == "ok");
    }
    SECTION("a hash nobody serves is answered only past the gate") {
        std::string const unknown = contentHash("nothing");
        REQUIRE(ask(service, *authorizer, request("ui-fetch", "tech", fetchBody(unknown))).message == "not found");
        REQUIRE(ask(service, *authorizer, request("ui-fetch", "", fetchBody(unknown))).message == "unauthorized");
    }
    SECTION("a request it cannot read") {
        REQUIRE(ask(service, *authorizer, request("ui-fetch", "tech", R"({"hash":"../../etc/passwd"})")).message ==
                "the ui-fetch request's hash is not a content hash");
        REQUIRE(
            ask(service, *authorizer, request("ui-fetch", "tech", "")).message.starts_with("the ui-fetch request: "));
    }
}

TEST_CASE("ui-fetch serves equal bytes under any entry that admits the caller", "[ui-service][fetch]") {
    // A screen with no query and a delivered component with the same bytes share one file.
    std::string const doc = R"({"vocab":"ui/1","id":"same","view":{"kind":"text","text":"x"}})";
    UiRegistry registry = emptyRegistry();
    CatalogEntry unavailable = entry("same");
    unavailable.available = false;
    REQUIRE(registry.addScreen(unavailable, doc));
    REQUIRE(registry.addFile(uiservice::file(morph::ui::kComponentFile, "copy", doc)));
    auto const built = registry.build();
    REQUIRE(built);
    UiService service{*built};
    auto const authorizer = uiservice::authorizer();
    REQUIRE((*built)->files.at(contentHash(doc)).holders.size() == 2U);
    Envelope const reply = ask(service, *authorizer, request("ui-fetch", "tech", fetchBody(contentHash(doc))));
    REQUIRE(reply.kind == "ok");
    REQUIRE(reply.body == doc);
}

TEST_CASE("ui-fetch refuses an unavailable screen's document", "[ui-service][fetch]") {
    std::string const doc = R"({"vocab":"ui/1","id":"later","view":{"kind":"text","text":"x"}})";
    UiRegistry registry = emptyRegistry();
    CatalogEntry unavailable = entry("later");
    unavailable.available = false;
    REQUIRE(registry.addScreen(unavailable, doc));
    auto const built = registry.build();
    REQUIRE(built);
    UiService service{*built};
    auto const authorizer = uiservice::authorizer();
    Envelope const reply = ask(service, *authorizer, request("ui-fetch", "vera", fetchBody(contentHash(doc))));
    REQUIRE(reply.message == "unauthorized");
}

TEST_CASE("ui-catalog gives the facts of a public screen it lists disabled", "[ui-service][catalog]") {
    std::string const doc = R"({"vocab":"ui/1","id":"pub","models":{"sample":{"type":"Lims_Sample"}},)"
                            R"("queries":{"report":{"model":"sample","action":"Lims_Report"}},)"
                            R"("view":{"kind":"text","text":"x"}})";
    UiRegistry registry{morph::ui::ApplicationInfo{.id = "pub", .version = "1"}, uiservice::facts()};
    REQUIRE(registry.addScreen(entry("pub", "disable", true), doc));
    auto const built = registry.build();
    REQUIRE(built);
    UiService service{*built};
    auto const authorizer = uiservice::authorizer();
    auto const catalog = morph::ui::parseCatalogReply(ask(service, *authorizer, request("ui-catalog", "tech")).body);
    REQUIRE(catalog);
    REQUIRE(catalog->entries.size() == 1U);
    REQUIRE_FALSE(catalog->entries.front().available);
    REQUIRE(catalog->entries.front().reason == "denied");
    REQUIRE(catalog->actions->contains("Lims_Report"));
    REQUIRE(catalog->permitted.empty());
}

namespace {

/// An authoriser for an application that signs its users in: before sign-in it admits the UI service's reads and
/// nothing else, as a token-checking authoriser that serves a login screen does.
struct SignInFirst final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        return !ctx.principal.empty() || morph::ui::isUiServiceRead(modelType, actionType);
    }
};

}  // namespace

// Mutation: admit `ui-evaluate`'s action too, or nothing at all (the login screen is no longer reachable).
TEST_CASE("isUiServiceRead admits the login screen before sign-in, and no other screen", "[ui-service][catalog]") {
    REQUIRE(morph::ui::isUiServiceRead("ui", "hello"));
    REQUIRE(morph::ui::isUiServiceRead("ui", "catalog"));
    REQUIRE(morph::ui::isUiServiceRead("ui", "fetch"));
    REQUIRE_FALSE(morph::ui::isUiServiceRead("ui", "evaluate"));
    REQUIRE_FALSE(morph::ui::isUiServiceRead("Lims_Sample", "fetch"));

    auto const bundle = uiservice::bundle();
    auto const manifest = morph::ui::parseManifest(bundle->manifest);
    REQUIRE(manifest);
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a manifest is keyed by kind, then id.
    auto const hashOf = [&](std::string_view kind, std::string_view id) { return manifest->find(kind, id)->hash; };
    UiService service{bundle};
    SignInFirst const authorizer;
    REQUIRE(ask(service, authorizer, request("ui-hello", "", helloBody({"ui/1", "expr/1", "ui-theme/1"}))).kind ==
            "ok");
    Envelope const catalogReply = ask(service, authorizer, request("ui-catalog", ""));
    REQUIRE(catalogReply.kind == "ok");
    auto const catalog = morph::ui::parseCatalogReply(catalogReply.body);
    REQUIRE(catalog);
    std::vector<std::string> const listedIds = ids(*catalog);
    CHECK(std::ranges::find(listedIds, "auth.login") != listedIds.end());
    CHECK(std::ranges::find(listedIds, "lims.results") == listedIds.end());
    CHECK(ask(service, authorizer, request("ui-fetch", "", fetchBody(hashOf("screen", "auth.login")))).kind == "ok");
    Envelope const results =
        ask(service, authorizer, request("ui-fetch", "", fetchBody(hashOf("screen", "lims.results"))));
    CHECK(results.message == "unauthorized");
}
