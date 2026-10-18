// SPDX-License-Identifier: Apache-2.0

// The client side of the UI service, end to end through a RemoteServer: spec 5
// §14's "Compatibility" cases (an old client with a new server and the reverse,
// a server without the UI service, a client missing a vocabulary, an unknown
// `!`-key) and its "Security" cases for delivery (a changed byte in a served
// file refuses the bundle), and the verified cache.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <latch>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/envelope_transport.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_service_support.hpp"

// A named namespace, not an anonymous one: the traits specialisations below
// must name these types from a namespace enclosing morph::model.
namespace ui_service_client_test {

struct Echo {
    int value = 0;
};

struct EchoModel {
    int execute(Echo action) { return action.value + 1; }
};

}  // namespace ui_service_client_test

using ui_service_client_test::Echo;
using ui_service_client_test::EchoModel;

template <>
struct morph::model::ModelTraits<EchoModel> {
    static constexpr std::string_view typeId() { return "UiSvc_Echo"; }
};
template <>
struct morph::model::ActionTraits<Echo> {
    using Result = int;
    static constexpr std::string_view typeId() { return "UiSvc_EchoAction"; }
    static std::string toJson(const Echo& action) { return glz::write_json(action).value_or("{}"); }
    static Echo fromJson(std::string_view json) {
        Echo action{};
        static_cast<void>(glz::read<glz::opts{.error_on_unknown_keys = false}>(action, json));
        return action;
    }
    static std::string resultToJson(const int& result) { return std::to_string(result); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

using morph::ui::ConnectError;
using morph::ui::LoadedBundle;
using morph::ui::UiClient;
using morph::wire::Envelope;

namespace {

using OpenResult = std::expected<LoadedBundle, ConnectError>;

/// What the client speaks: the fixture's vocabularies, and delivered components.
morph::ui::HelloRequest fullClient() {
    morph::ui::HelloRequest client;
    client.vocabularies = {"ui/1", "expr/1", "ui-theme/1"};
    client.qt = "6.8";
    client.components = true;
    return client;
}

/// A session for @p principal: the fixture's authoriser takes the token as the principal.
morph::session::Context as(std::string principal) {
    morph::session::Context session;
    session.token = std::move(principal);
    return session;
}

/// A server, with or without the UI service, and a transport to it that records what it carries and lets a test
/// rewrite a reply on its way back.
struct Harness {
    explicit Harness(bool withService = true,
                     std::shared_ptr<morph::ui::ServedBundle const> served = uiservice::bundle()) {
        morph::backend::ServerConfig config;
        if (withService) {
            config.services.push_back(std::make_shared<morph::ui::UiService>(std::move(served)));
        }
        server = std::make_shared<morph::backend::RemoteServer>(pool, uiservice::authorizer(), config, dispatcher,
                                                                registry);
    }

    [[nodiscard]] morph::ui::Exchange exchange() {
        return [this](Envelope const& request, std::function<void(morph::ui::ExchangeReply)> const& done) {
            {
                std::scoped_lock const lock{sentMutex};
                sent.push_back(request.kind);
            }
            if (unreachable) {
                done(std::unexpected(std::string{"connection lost"}));
                return;
            }
            Envelope reply = morph::wire::decode(server->handleInline(morph::wire::encode(request)));
            if (tamper) {
                tamper(request, reply);
            }
            done(std::move(reply));
        };
    }

    [[nodiscard]] UiClient client(morph::ui::HelloRequest hello = fullClient()) {
        return UiClient{exchange(), std::move(hello), store};
    }

    /// Opens through @p client with a synchronous transport, so the result is there when `open` returns.
    static OpenResult open(UiClient const& client, morph::session::Context session) {
        std::optional<OpenResult> result;
        client.open(std::move(session), [&result](OpenResult opened) { result = std::move(opened); });
        REQUIRE(result.has_value());
        return std::move(*result);
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::shared_ptr<morph::ui::MemoryBundleStore> store = std::make_shared<morph::ui::MemoryBundleStore>();
    std::mutex sentMutex;
    std::vector<std::string> sent;
    std::function<void(Envelope const& request, Envelope& reply)> tamper;
    bool unreachable = false;
};

/// The content hash the fixture's manifest gives a file.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a manifest is keyed by kind, then id.
std::string hashOf(std::string_view kind, std::string_view id) {
    auto const manifest = morph::ui::parseManifest(uiservice::bundle()->manifest);
    REQUIRE(manifest);
    return manifest->find(kind, id)->hash;
}

/// Changes one bit of one byte in the middle of @p bytes.
void flipOneBit(std::string& bytes) {
    char& middle = bytes.at(bytes.size() / 2);
    middle = static_cast<char>(middle ^ 0x01);
}

/// The hash a `ui-fetch` request asks for.
std::string fetched(Envelope const& request) {
    auto const parsed = morph::ui::parseFetchRequest(request.body);
    if (!parsed) {
        return {};
    }
    return parsed->hash;
}

}  // namespace

// ── A new client and a new server ───────────────────────────────────────────

TEST_CASE("UiClient opens a verified bundle filtered for the principal", "[ui-service][client]") {
    Harness harness;
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE(opened);
    morph::ui::Bundle const& bundle = opened->bundle;
    REQUIRE(bundle.applicationId == "lims");
    REQUIRE("sha256:" + bundle.manifestDigest == uiservice::bundle()->digest);
    // lims.verify is hidden and lims.report disabled for a technician: neither is fetched.
    REQUIRE(bundle.documents.size() == 3U);
    REQUIRE(bundle.documents.at("lims.results") == uiservice::kResults);
    REQUIRE(bundle.documents.at("auth.login") == uiservice::kLogin);
    REQUIRE(bundle.documents.at("app") == uiservice::kApp);
    REQUIRE(opened->files.at(hashOf("theme", "default")) == uiservice::kTheme);
    // The shell reads the theme from the bundle, by file id, as it reads the documents.
    CHECK(opened->bundle.themes.at("default") == uiservice::kTheme);
    REQUIRE(opened->files.at(hashOf("component", "Chart")) == uiservice::kChart);
    REQUIRE(opened->hello.application == "lims");
    REQUIRE(opened->catalog.entries.size() == 5U);
    REQUIRE(harness.sent == std::vector<std::string>{"ui-hello", "ui-fetch", "ui-catalog", "ui-fetch", "ui-fetch",
                                                     "ui-fetch", "ui-fetch", "ui-fetch"});
    REQUIRE(harness.store->lastDigest("lims") == uiservice::bundle()->digest);

    // The catalog's facts validate the documents on the client as the server did at registration.
    morph::ui::Catalog const facts = opened->catalog.facts();
    REQUIRE(morph::ui::loadDocumentText(bundle.documents.at("lims.results"), {.catalog = facts}));
    REQUIRE(morph::ui::loadDocumentText(bundle.documents.at("auth.login"), {.catalog = facts}));
}

TEST_CASE("UiClient fetches nothing for an unchanged manifest", "[ui-service][client]") {
    Harness harness;
    REQUIRE(Harness::open(harness.client(), as("tech")));
    harness.sent.clear();
    OpenResult const again = Harness::open(harness.client(), as("tech"));
    REQUIRE(again);
    REQUIRE(again->bundle.documents.size() == 3U);
    REQUIRE(harness.sent == std::vector<std::string>{"ui-hello", "ui-catalog"});
}

// Mutation: skip the digest comparison in onHello — the unchanged poll opens the bundle (catalog sent, a value
// delivered).
TEST_CASE("UiClient::poll sends only ui-hello while the digest is unchanged, and opens a changed bundle",
          "[ui-service][client][dev-reload]") {
    Harness harness;
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE(opened);
    harness.sent.clear();

    using Polled = std::expected<std::optional<LoadedBundle>, ConnectError>;
    std::optional<Polled> unchanged;
    harness.client().poll(as("tech"), opened->hello.digest,
                          [&unchanged](Polled polled) { unchanged = std::move(polled); });
    REQUIRE(unchanged.has_value());
    REQUIRE(unchanged->has_value());
    CHECK_FALSE(unchanged->value().has_value());
    CHECK(harness.sent == std::vector<std::string>{"ui-hello"});

    harness.sent.clear();
    std::optional<Polled> changed;
    harness.client().poll(as("tech"), "sha256:" + std::string(64, '0'),
                          [&changed](Polled polled) { changed = std::move(polled); });
    REQUIRE(changed.has_value());
    REQUIRE(changed->has_value());
    REQUIRE(changed->value().has_value());
    CHECK(changed->value()->hello.digest == uiservice::bundle()->digest);
    CHECK(changed->value()->bundle.documents.at("lims.results") == uiservice::kResults);
    CHECK(harness.sent == std::vector<std::string>{"ui-hello", "ui-catalog"});  // the cache holds every file

    harness.unreachable = true;
    std::optional<Polled> failed;
    harness.client().poll(as("tech"), opened->hello.digest, [&failed](Polled polled) { failed = std::move(polled); });
    REQUIRE(failed.has_value());
    REQUIRE_FALSE(failed->has_value());
    CHECK(failed->error().kind == ConnectError::Kind::Unreachable);
}

TEST_CASE("UiClient fetches what a principal newly may, and keeps the rest", "[ui-service][client]") {
    Harness harness;
    REQUIRE(Harness::open(harness.client(), as("tech")));
    harness.sent.clear();
    OpenResult const verifier = Harness::open(harness.client(), as("vera"));
    REQUIRE(verifier);
    REQUIRE(verifier->bundle.documents.contains("lims.verify"));
    REQUIRE(verifier->bundle.documents.contains("lims.report"));
    REQUIRE(harness.sent == std::vector<std::string>{"ui-hello", "ui-catalog", "ui-fetch", "ui-fetch"});
}

TEST_CASE("UiClient leaves components to a client that loads them", "[ui-service][client]") {
    Harness harness;
    morph::ui::HelloRequest hello = fullClient();
    hello.components = false;
    OpenResult const opened = Harness::open(harness.client(hello), as("tech"));
    REQUIRE(opened);
    REQUIRE_FALSE(opened->files.contains(hashOf("component", "Chart")));
}

TEST_CASE("UiClient answers on whatever thread the transport calls back on", "[ui-service][client]") {
    Harness harness;
    morph::exec::ThreadPoolExecutor replies{4};
    auto synchronous = harness.exchange();
    morph::ui::Exchange const asynchronous = [&replies, synchronous](Envelope request, auto done) {
        replies.post([synchronous, request = std::move(request), done = std::move(done)]() mutable {
            synchronous(std::move(request), std::move(done));
        });
    };
    UiClient const client{asynchronous, fullClient(), harness.store};
    std::latch arrived{1};
    std::optional<OpenResult> result;
    client.open(as("vera"), [&](OpenResult opened) {
        result = std::move(opened);
        arrived.count_down();
    });
    arrived.wait();
    REQUIRE(result.has_value());
    REQUIRE(*result);
    REQUIRE((*result)->bundle.documents.size() == 5U);
}

// ── Compatibility (spec 5 §14) ──────────────────────────────────────────────

TEST_CASE("Compatibility: a server without the UI service answers unknown envelope kind", "[ui-service][compat]") {
    Harness harness{false};
    Envelope request;
    request.kind = "ui-hello";
    Envelope const raw = morph::wire::decode(harness.server->handleInline(morph::wire::encode(request)));
    REQUIRE(raw.kind == "err");
    REQUIRE(raw.message == "unknown envelope kind: ui-hello");

    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().kind == ConnectError::Kind::NoUiService);
    REQUIRE(opened.error().message == "the server serves no UI");
    REQUIRE(harness.sent == std::vector<std::string>{"ui-hello"});
}

TEST_CASE("Compatibility: an old client is unchanged by a server with the UI service", "[ui-service][compat]") {
    Harness withService;
    Harness without{false};
    for (Harness* harness : {&withService, &without}) {
        harness->registry.registerModel<EchoModel>("UiSvc_Echo");
        harness->dispatcher.registerAction<EchoModel, Echo>("UiSvc_Echo", "UiSvc_EchoAction");
    }
    // The handshake an old client sends: the same reply from both servers, no capability added.
    std::string const helloWith = withService.server->handleInline(morph::wire::encode(morph::wire::makeHello()));
    std::string const helloWithout = without.server->handleInline(morph::wire::encode(morph::wire::makeHello()));
    REQUIRE(helloWith == helloWithout);
    REQUIRE(morph::wire::interpretHelloReply(morph::wire::decode(helloWith)) ==
            morph::wire::ProtocolNegotiationResult::Negotiated);

    // Register and execute as before.
    Envelope registerRequest = morph::wire::makeRegister("UiSvc_Echo");
    registerRequest.session.token = "tech";
    Envelope const registered =
        morph::wire::decode(withService.server->handleInline(morph::wire::encode(registerRequest)));
    REQUIRE(registered.kind == "ok");
    Envelope execute;
    execute.kind = "execute";
    execute.callId = 9;
    execute.modelId = registered.modelId;
    execute.modelType = "UiSvc_Echo";
    execute.actionType = "UiSvc_EchoAction";
    execute.body = R"({"value":41})";
    execute.session.token = "tech";
    morph::testing::WaitReply waiter;
    withService.server->handle(morph::wire::encode(execute), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.body == "42");
}

TEST_CASE("Compatibility: a client missing a vocabulary the bundle needs is refused, naming it",
          "[ui-service][compat]") {
    Harness harness;
    morph::ui::HelloRequest older = fullClient();
    older.vocabularies = {"ui/1", "expr/1"};
    OpenResult const opened = Harness::open(harness.client(older), as("tech"));
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
    REQUIRE(opened.error().message == "missing vocabulary: ui-theme/1");
}

TEST_CASE("Compatibility: a server whose bundle needs a vocabulary the client lacks is refused by the client",
          "[ui-service][compat]") {
    Harness harness;
    harness.tamper = [](Envelope const& request, Envelope& reply) {
        if (request.kind == "ui-hello") {
            auto hello = morph::ui::parseHelloReply(reply.body);
            REQUIRE(hello);
            hello->vocabularies.emplace_back("forms/1");
            reply.body = morph::ui::detail::writeMessage(*hello);
        }
    };
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
    REQUIRE(opened.error().message ==
            "the application needs the vocabulary forms/1, which this client does not speak");
}

TEST_CASE("Compatibility: a newer server's optional keys are ignored", "[ui-service][compat]") {
    Harness harness;
    harness.tamper = [](Envelope const& request, Envelope& reply) {
        if (request.kind == "ui-hello" || request.kind == "ui-catalog") {
            reply.body.insert(1, R"("addedLater":{"x":[1,2]},)");
        }
    };
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE(opened);
    REQUIRE(opened->bundle.documents.size() == 3U);
}

TEST_CASE("Compatibility: an unknown must-understand key is refused", "[ui-service][compat]") {
    Harness harness;

    SECTION("in the ui-hello reply") {
        harness.tamper = [](Envelope const& request, Envelope& reply) {
            if (request.kind == "ui-hello") {
                reply.body.insert(1, R"("!push":true,)");
            }
        };
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
        REQUIRE(opened.error().message == "the must-understand key `!push` in the ui-hello reply is not understood");
    }
    SECTION("in a catalog entry") {
        harness.tamper = [](Envelope const& request, Envelope& reply) {
            if (request.kind == "ui-catalog") {
                auto const at = reply.body.find(R"({"id":"lims.results")");
                REQUIRE(at != std::string::npos);
                reply.body.insert(at + 1, R"("!licensed":true,)");
            }
        };
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
        REQUIRE(opened.error().message ==
                "the must-understand key `!licensed` in the catalog entries is not understood");
    }
    SECTION("in the manifest, whose digest the server vouches for") {
        std::string const manifest = R"({"!signed":true,"application":"x","files":[]})";
        std::string const digest = morph::ui::contentHash(manifest);
        morph::ui::Exchange const fake = [&](Envelope const& request, auto const& done) {
            Envelope reply = morph::wire::makeOk(
                0, request.kind == "ui-hello" ? R"({"application":"x","digest":")" + digest + R"("})" : manifest);
            done(std::move(reply));
        };
        OpenResult const opened = Harness::open(UiClient{fake, fullClient(), harness.store}, as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
        REQUIRE(opened.error().message == "the must-understand key `!signed` in the manifest is not understood");
    }
    SECTION("in a served document, which load-time validation refuses") {
        std::string const doc = R"({"vocab":"ui/1","id":"t","!requires":"ui/2","view":{"kind":"text","text":"x"}})";
        auto const refused = morph::ui::loadDocumentText(doc);
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().document == "t");
        REQUIRE(refused.error().path == "/!requires");
        // And the registry refuses it before the server could serve it.
        morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "t", .version = "1"}};
        REQUIRE_FALSE(registry.addScreen(uiservice::entry("t"), doc));
    }
}

// ── Security (spec 5 §14, §13) ──────────────────────────────────────────────

TEST_CASE("Security: one changed byte in a served file refuses the bundle", "[ui-service][security]") {
    Harness harness;
    std::string const results = hashOf("screen", "lims.results");
    harness.tamper = [&results](Envelope const& request, Envelope& reply) {
        if (request.kind == "ui-fetch" && fetched(request) == results) {
            flipOneBit(reply.body);
        }
    };
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().kind == ConnectError::Kind::Unverified);
    REQUIRE(opened.error().message == "the screen `lims.results` does not match its hash");
    // Nothing unverified is kept, and the bundle is not recorded as the last verified one.
    REQUIRE_FALSE(harness.store->get(results));
    REQUIRE_FALSE(harness.store->lastDigest("lims"));
}

TEST_CASE("Security: one changed byte in the manifest refuses the bundle", "[ui-service][security]") {
    Harness harness;
    harness.tamper = [](Envelope const& request, Envelope& reply) {
        if (request.kind == "ui-fetch" && fetched(request) == uiservice::bundle()->digest) {
            flipOneBit(reply.body);
        }
    };
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().kind == ConnectError::Kind::Unverified);
    REQUIRE(opened.error().message == "the manifest does not match its hash");
    REQUIRE(harness.sent == std::vector<std::string>{"ui-hello", "ui-fetch"});
}

TEST_CASE("Security: a served file larger than the bound is refused", "[ui-service][security]") {
    Harness harness;
    std::string const theme = hashOf("theme", "default");
    harness.tamper = [&theme](Envelope const& request, Envelope& reply) {
        if (request.kind == "ui-fetch" && fetched(request) == theme) {
            reply.body.assign(morph::ui::kMaxFileBytes + 1, ' ');
        }
    };
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE_FALSE(opened);
    REQUIRE(opened.error().kind == ConnectError::Kind::Unverified);
    REQUIRE(opened.error().message == "the theme `default` is larger than 4194304 bytes");
}

TEST_CASE("Security: a cached file that no longer verifies is fetched again", "[ui-service][security]") {
    Harness harness;
    REQUIRE(Harness::open(harness.client(), as("tech")));
    std::string const results = hashOf("screen", "lims.results");
    harness.store->put(results, "tampered on disk");
    harness.store->put(uiservice::bundle()->digest, "{}");
    harness.sent.clear();
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE(opened);
    REQUIRE(opened->bundle.documents.at("lims.results") == uiservice::kResults);
    REQUIRE(harness.sent == std::vector<std::string>{"ui-hello", "ui-fetch", "ui-catalog", "ui-fetch"});
    REQUIRE(harness.store->get(results) == uiservice::kResults);
}

TEST_CASE("Security: refusals by the server surface as Refused", "[ui-service][security]") {
    Harness harness;

    SECTION("the catalog, before sign-in") {
        OpenResult const opened = Harness::open(harness.client(), morph::session::Context{});
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Refused);
        REQUIRE(opened.error().message == "unauthorized");
        // The handshake and the public manifest were admitted.
        REQUIRE(harness.sent == std::vector<std::string>{"ui-hello", "ui-fetch", "ui-catalog"});
    }
    SECTION("a fetch") {
        harness.tamper = [](Envelope const& request, Envelope& reply) {
            if (request.kind == "ui-fetch" && fetched(request) == hashOf("app", "app")) {
                reply = morph::wire::makeErr("unauthorized");
            }
        };
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Refused);
        REQUIRE(opened.error().message == "the app `app`: unauthorized");
    }
    SECTION("the handshake") {
        harness.tamper = [](Envelope const& request, Envelope& reply) {
            if (request.kind == "ui-hello") {
                reply = morph::wire::makeErr("unauthorized");
            }
        };
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Refused);
    }
}

TEST_CASE("UiClient reports a transport failure as Unreachable", "[ui-service][client]") {
    Harness harness;

    SECTION("at the handshake") {
        harness.unreachable = true;
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unreachable);
        REQUIRE(opened.error().message == "connection lost");
    }
    SECTION("while fetching") {
        std::string const theme = hashOf("theme", "default");
        auto inner = harness.exchange();
        morph::ui::Exchange const flaky = [inner, theme](Envelope request, auto done) {
            if (request.kind == "ui-fetch" && fetched(request) == theme) {
                done(std::unexpected(std::string{"connection lost"}));
                return;
            }
            inner(std::move(request), std::move(done));
        };
        OpenResult const opened = Harness::open(UiClient{flaky, fullClient(), harness.store}, as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unreachable);
        REQUIRE(opened.error().message == "the theme `default`: connection lost");
    }
}

TEST_CASE("UiClient refuses replies it cannot read", "[ui-service][client]") {
    Harness harness;

    SECTION("the handshake") {
        harness.tamper = [](Envelope const& request, Envelope& reply) {
            if (request.kind == "ui-hello") {
                reply.body = "not json";
            }
        };
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
    }
    SECTION("the catalog") {
        harness.tamper = [](Envelope const& request, Envelope& reply) {
            if (request.kind == "ui-catalog") {
                reply.body = R"({"entries":7})";
            }
        };
        OpenResult const opened = Harness::open(harness.client(), as("tech"));
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
        REQUIRE(opened.error().message.starts_with("the catalog: "));
    }
}

// ── Offline, and the cache ──────────────────────────────────────────────────

TEST_CASE("UiClient::openOffline starts from the last verified bundle", "[ui-service][client]") {
    Harness harness;
    UiClient const client = harness.client();

    SECTION("nothing verified yet") {
        auto const opened = client.openOffline("lims");
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unreachable);
        REQUIRE(opened.error().message == "no verified bundle of `lims` is cached");
    }
    SECTION("after a verified open") {
        REQUIRE(Harness::open(client, as("tech")));
        harness.sent.clear();
        auto const opened = client.openOffline("lims");
        REQUIRE(opened);
        REQUIRE(opened->bundle.documents.size() == 3U);
        REQUIRE(opened->catalog.entries.empty());
        REQUIRE(opened->files.contains(hashOf("component", "Chart")));
        REQUIRE(harness.sent.empty());
    }
    SECTION("a file that no longer verifies is left out") {
        REQUIRE(Harness::open(client, as("tech")));
        harness.store->put(hashOf("screen", "lims.results"), "changed");
        auto const opened = client.openOffline("lims");
        REQUIRE(opened);
        REQUIRE_FALSE(opened->bundle.documents.contains("lims.results"));
        REQUIRE(opened->bundle.documents.contains("auth.login"));
    }
    SECTION("a manifest that no longer verifies") {
        REQUIRE(Harness::open(client, as("tech")));
        harness.store->put(uiservice::bundle()->digest, "changed");
        auto const opened = client.openOffline("lims");
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().message == "the cached manifest of `lims` is gone or does not verify");
    }
    SECTION("a verified manifest this client cannot read") {
        std::string const unreadable = R"({"files":"none"})";
        harness.store->put(morph::ui::contentHash(unreadable), unreadable);
        harness.store->setLastDigest("lims", morph::ui::contentHash(unreadable));
        auto const opened = client.openOffline("lims");
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error().kind == ConnectError::Kind::Unsupported);
    }
    SECTION("a client without components leaves them out") {
        REQUIRE(Harness::open(client, as("tech")));
        morph::ui::HelloRequest hello = fullClient();
        hello.components = false;
        auto const opened = harness.client(hello).openOffline("lims");
        REQUIRE(opened);
        REQUIRE_FALSE(opened->files.contains(hashOf("component", "Chart")));
    }
}

TEST_CASE("VerifiedCache hands out and keeps only bytes that match their hash", "[ui-service][client]") {
    auto store = std::make_shared<morph::ui::MemoryBundleStore>();
    morph::ui::VerifiedCache cache{store};
    std::string const hash = morph::ui::contentHash("bytes");
    REQUIRE(store->size() == 0U);
    REQUIRE(cache.put("bytes") == hash);
    REQUIRE(cache.get(hash) == "bytes");
    store->put(hash, "rotted");
    REQUIRE_FALSE(cache.get(hash));
    REQUIRE(store->size() == 0U);
    REQUIRE_FALSE(cache.get(morph::ui::contentHash("absent")));
    store->remove(hash);
    REQUIRE_THROWS_AS(morph::ui::VerifiedCache{nullptr}, std::invalid_argument);
    REQUIRE_THROWS_AS((UiClient{morph::ui::Exchange{}, fullClient(), store}), std::invalid_argument);
}

TEST_CASE("Security: two changed files refuse the bundle once, naming the first", "[ui-service][security]") {
    Harness harness;
    std::string const results = hashOf("screen", "lims.results");
    std::string const theme = hashOf("theme", "default");
    harness.tamper = [&results, &theme](Envelope const& request, Envelope& reply) {
        if (request.kind == "ui-fetch" && (fetched(request) == results || fetched(request) == theme)) {
            flipOneBit(reply.body);
        }
    };
    int calls = 0;
    std::optional<OpenResult> result;
    harness.client().open(as("tech"), [&](OpenResult opened) {
        ++calls;
        result = std::move(opened);
    });
    REQUIRE(calls == 1);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(*result);
    REQUIRE(result->error().kind == ConnectError::Kind::Unverified);
    REQUIRE(result->error().message == "the screen `lims.results` does not match its hash");
}

TEST_CASE("UiClient fetches a public screen the catalog lists disabled", "[ui-service][client]") {
    std::string const doc = R"({"vocab":"ui/1","id":"pub","models":{"sample":{"type":"Lims_Sample"}},)"
                            R"("queries":{"report":{"model":"sample","action":"Lims_Report"}},)"
                            R"("view":{"kind":"text","text":"x"}})";
    morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "pub", .version = "1"}, uiservice::facts()};
    REQUIRE(registry.addScreen(uiservice::entry("pub", "disable", true), doc));
    auto const built = registry.build();
    REQUIRE(built);
    Harness harness{true, *built};
    OpenResult const opened = Harness::open(harness.client(), as("tech"));
    REQUIRE(opened);
    REQUIRE(opened->bundle.documents.at("pub") == doc);
    REQUIRE_FALSE(opened->catalog.entries.front().available);
}

TEST_CASE("UiClient::openOffline keeps only the kinds the client uses", "[ui-service][client]") {
    Harness harness;
    std::string const messages = R"({"hello":"Hallo"})";
    std::string const font = "binary";
    std::string const manifest =
        R"({"application":"lims","files":[{"id":"de","kind":"messages","hash":")" + morph::ui::contentHash(messages) +
        R"(","size":17},{"id":"f","kind":"font","hash":")" + morph::ui::contentHash(font) + R"(","size":6}]})";
    for (std::string const& file : {messages, font, manifest}) {
        harness.store->put(morph::ui::contentHash(file), file);
    }
    harness.store->setLastDigest("lims", morph::ui::contentHash(manifest));
    auto const opened = harness.client().openOffline("lims");
    REQUIRE(opened);
    REQUIRE(opened->files.contains(morph::ui::contentHash(messages)));
    REQUIRE_FALSE(opened->files.contains(morph::ui::contentHash(font)));
    REQUIRE(opened->bundle.documents.empty());
}

// ── exchangeOver: the exchange over a transport's request verb ──────────────

namespace {

/// A transport whose `request` answers from a `RemoteServer` in process, or fails as a dropped connection does.
class RequestBackend final : public morph::backend::IEnvelopeTransport {
public:
    explicit RequestBackend(std::shared_ptr<morph::backend::RemoteServer> server) : _server{std::move(server)} {}
    ~RequestBackend() = default;
    RequestBackend(RequestBackend const&) = delete;
    RequestBackend& operator=(RequestBackend const&) = delete;
    RequestBackend(RequestBackend&&) = delete;
    RequestBackend& operator=(RequestBackend&&) = delete;

    morph::async::Completion<Envelope> request(Envelope envelope, morph::exec::IExecutor& cbExec) override {
        sessions.push_back(envelope.session.token);
        auto [completion, promise] = morph::async::Completion<Envelope>::makeSettleable(&cbExec);
        if (dropped) {
            promise.reject(std::make_exception_ptr(morph::backend::DisconnectedError{}));
        } else {
            envelope.callId = ++_nextCallId;
            promise.resolve(morph::wire::decode(_server->handleInline(morph::wire::encode(envelope))));
        }
        return std::move(completion);
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test sets and reads these.
    bool dropped = false;
    std::vector<std::string> sessions;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    std::shared_ptr<morph::backend::RemoteServer> _server;
    std::uint64_t _nextCallId = 0;
};

/// Runs @p owner's queue until @p result holds a value.
OpenResult drainOpen(morph::exec::MainThreadExecutor& owner, std::optional<OpenResult>& result) {
    for (int turn = 0; turn < 1000 && !result.has_value(); ++turn) {
        static_cast<void>(owner.runOnce());
    }
    REQUIRE(result.has_value());
    return std::move(*result);
}

}  // namespace

TEST_CASE("exchangeOver: UiClient opens a bundle through a transport's request verb", "[ui-service][client]") {
    Harness const harness;
    RequestBackend transport{harness.server};
    morph::exec::MainThreadExecutor owner;
    UiClient const client{morph::ui::exchangeOver(transport, owner), fullClient(), harness.store};
    std::optional<OpenResult> result;
    client.open(as("tech"), [&result](OpenResult opened) { result = std::move(opened); });
    OpenResult const opened = drainOpen(owner, result);
    REQUIRE(opened.has_value());
    CHECK(opened->bundle.applicationId == "lims");
    CHECK(opened->bundle.documents.contains("lims.results"));
    // Every request carried the client's session.
    REQUIRE_FALSE(transport.sessions.empty());
    CHECK(std::ranges::all_of(transport.sessions, [](std::string const& token) { return token == "tech"; }));
}

// Mutation: hand the exchange an empty reply instead of the failure — the run reports Unsupported, not Unreachable.
TEST_CASE("exchangeOver: a request the transport could not deliver is Unreachable, with its reason",
          "[ui-service][client]") {
    Harness const harness;
    RequestBackend transport{harness.server};
    transport.dropped = true;
    morph::exec::MainThreadExecutor owner;
    UiClient const client{morph::ui::exchangeOver(transport, owner), fullClient(), harness.store};
    std::optional<OpenResult> result;
    client.open(as("tech"), [&result](OpenResult opened) { result = std::move(opened); });
    OpenResult const opened = drainOpen(owner, result);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == ConnectError::Kind::Unreachable);
    CHECK(opened.error().message == morph::backend::DisconnectedError{}.what());
}
