// SPDX-License-Identifier: Apache-2.0
//
// Fixtures of the custom screens tests: a base screen that uses one of every capability the base rule compares,
// and a loader that refuses to go on with a fixture that does not load.

#pragma once
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <stdexcept>
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
           R"({"kind":"filePicker","label":"Attach","onPick":[{"upload":{"ref":"event"},"to":"save","as":"ref"}]}]}})";
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

/// The service fixture's registry with the base screen added as `lims.base`.
inline morph::ui::UiRegistry registry() {
    morph::ui::UiRegistry made = uiservice::registry();
    auto const added = made.addScreen(uiservice::entry("lims.base"), baseScreen("lims.base"));
    if (!added) {
        throw std::runtime_error{added.error().message()};
    }
    return made;
}

/// That registry's bundle.
inline std::shared_ptr<morph::ui::ServedBundle const> bundle() {
    auto built = registry().build();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    return *built;
}

using morph::wire::Envelope;

/// @p text as a JSON string literal, quotes included.
inline std::string quoted(std::string const& text) { return glz::write_json(text).value_or("\"\""); }

/// A RemoteServer that offers custom screens over @p store, checking against the fixture bundle.
struct Server {
    explicit Server(std::shared_ptr<morph::ui::ICustomScreenStore> chosen = nullptr,
                    morph::ui::CustomScreensOptions options = {})
        : store{chosen != nullptr ? std::move(chosen) : std::make_shared<morph::ui::MemoryCustomScreenStore>(pool)} {
        morph::ui::registerCustomScreens(store, [held = served] { return held; }, authorizer, dispatcher, registry,
                                         options);
        server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): bind() needs the server built above.
        instance = bind();
    }

    Envelope send(Envelope const& request) {
        std::promise<std::string> replied;
        auto reply = replied.get_future();
        server->handle(morph::wire::encode(request),
                       [&replied](std::string text) { replied.set_value(std::move(text)); });
        REQUIRE(reply.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        return morph::wire::decode(reply.get());
    }

    std::uint64_t bind() {
        Envelope const reply = send(morph::wire::makeRegister(std::string{morph::ui::kCustomScreensModel}));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    /// Runs @p action with @p body as the caller whose token is @p token.
    Envelope call(std::string_view action, std::string body, std::string token) {
        Envelope request;
        request.kind = "execute";
        request.callId = ++lastCallId;
        request.modelId = instance;
        request.modelType = std::string{morph::ui::kCustomScreensModel};
        request.actionType = std::string{action};
        request.body = std::move(body);
        request.session.token = std::move(token);
        return send(request);
    }

    /// Saves @p document as a new custom screen of @p token's, based on `lims.base`.
    Envelope save(std::string const& document, std::string token, std::string const& title = "Mine") {
        return call(morph::ui::kCustomScreensSave,
                    R"({"app":"lims","title":)" + quoted(title) + R"(,"basedOn":"lims.base","document":)" +
                        quoted(document) + "}",
                    std::move(token));
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<morph::ui::ServedBundle const> served = bundle();
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    std::shared_ptr<morph::ui::ICustomScreenStore> store;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t instance = 0;
    std::uint64_t lastCallId = 0;
};

}  // namespace screens_test
