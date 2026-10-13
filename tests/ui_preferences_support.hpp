// SPDX-License-Identifier: Apache-2.0
//
// The server side of the preferences tests: a RemoteServer that offers the preferences model over a store that
// records what it is given, and a raw execute as a chosen caller.

#pragma once
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/preferences.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prefs_test {

using morph::wire::Envelope;

/// Allows every call; the token, when there is one, is the verified principal.
struct TokenAuthorizer final : morph::session::IAuthorizer {
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): IAuthorizer's signature.
    [[nodiscard]] bool authorize(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }

    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        if (ctx.token.empty()) {
            return std::nullopt;
        }
        return ctx.token;
    }
};

/// A store over the memory store that records every key and value it is given.
class RecordingStore final : public morph::ui::IPreferencesStore {
public:
    explicit RecordingStore(morph::exec::IExecutor& base) : _inner{base} {}

    [[nodiscard]] morph::async::Completion<std::optional<std::string>> get(morph::ui::PrefKey const& key) override {
        note("get", key, {});
        return _inner.get(key);
    }

    [[nodiscard]] morph::async::Completion<bool> set(morph::ui::PrefKey const& key, std::string json) override {
        note("set", key, json);
        return _inner.set(key, std::move(json));
    }

    [[nodiscard]] morph::async::Completion<bool> remove(morph::ui::PrefKey const& key) override {
        note("remove", key, {});
        return _inner.remove(key);
    }

    /// Every call, as `verb person app scope point value`.
    [[nodiscard]] std::vector<std::string> calls() const {
        std::scoped_lock const lock{_mutex};
        return _calls;
    }

private:
    void note(std::string_view verb, morph::ui::PrefKey const& key, std::string const& value) {
        std::scoped_lock const lock{_mutex};
        _calls.push_back(std::string{verb} + " " + key.person + " " + key.app + " " + key.scope + " " + key.point +
                         (value.empty() ? "" : " " + value));
    }

    morph::ui::MemoryPreferencesStore _inner;
    mutable std::mutex _mutex;
    std::vector<std::string> _calls;
};

/// A RemoteServer that offers preferences over a recording store.
struct Server {
    explicit Server(morph::ui::PreferencesOptions options = {}) {
        morph::ui::registerPreferences(store, dispatcher, registry, options);
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<TokenAuthorizer>(), dispatcher,
                                                                registry);
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): bind() needs the server built above.
        instance = bind();
    }

    /// Sends @p request and waits for the reply.
    Envelope send(Envelope const& request) {
        std::promise<std::string> replied;
        auto reply = replied.get_future();
        server->handle(morph::wire::encode(request),
                       [&replied](std::string text) { replied.set_value(std::move(text)); });
        REQUIRE(reply.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        return morph::wire::decode(reply.get());
    }

    /// Registers a preferences instance and returns its id.
    std::uint64_t bind() {
        Envelope const reply = send(morph::wire::makeRegister(std::string{morph::ui::kPreferencesModel}));
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }

    /// Runs @p action with @p body as the caller whose token is @p token, claiming @p claim as its principal.
    Envelope call(std::string_view action, std::string body, std::string token, std::string claim = {}) {
        Envelope request;
        request.kind = "execute";
        request.callId = ++lastCallId;
        request.modelId = instance;
        request.modelType = std::string{morph::ui::kPreferencesModel};
        request.actionType = std::string{action};
        request.body = std::move(body);
        request.session.token = std::move(token);
        request.session.principal = std::move(claim);
        return send(request);
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<RecordingStore> store = std::make_shared<RecordingStore>(pool);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t instance = 0;
    std::uint64_t lastCallId = 0;
};

inline constexpr char const* kLoadDensity = R"({"app":"lims","scope":"lims.results","points":["density"]})";

}  // namespace prefs_test
