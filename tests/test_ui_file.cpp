// SPDX-License-Identifier: Apache-2.0

// The file side channel (`ui-file`, spec 6 §18), end to end through a RemoteServer: an upload in chunks authorised
// as the mutation that will reference it, and a download authorised by the principal-bound reference.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/file_store.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_service_support.hpp"

using morph::ui::DownloadedFile;
using morph::ui::FileInfo;
using morph::ui::FileTransfer;
using morph::wire::Envelope;

namespace {

morph::session::Context as(std::string principal) {
    morph::session::Context session;
    session.token = std::move(principal);
    return session;
}

/// A server with the UI service and a file store, and a synchronous transport that counts what it carries.
struct Harness {
    explicit Harness(bool withFiles = true) {
        morph::backend::ServerConfig config;
        config.services.push_back(
            std::make_shared<morph::ui::UiService>(uiservice::bundle(), dispatcher, withFiles ? store : nullptr));
        server = std::make_shared<morph::backend::RemoteServer>(pool, uiservice::authorizer(), config, dispatcher,
                                                                registry);
    }

    [[nodiscard]] morph::ui::Exchange exchange() {
        return [this](Envelope const& request, std::function<void(morph::ui::ExchangeReply)> const& done) {
            ++exchanges;
            done(morph::wire::decode(server->handleInline(morph::wire::encode(request))));
        };
    }

    [[nodiscard]] Envelope send(std::string principal, std::string body) {
        Envelope request;
        request.kind = std::string{morph::ui::kFileKind};
        request.callId = 3;
        request.session = as(std::move(principal));
        request.body = std::move(body);
        return morph::wire::decode(server->handleInline(morph::wire::encode(request)));
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<morph::ui::MemoryFileStore> store = std::make_shared<morph::ui::MemoryFileStore>();
    std::shared_ptr<morph::backend::RemoteServer> server;
    int exchanges = 0;
};

std::string pattern(std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>(i % 251);
    }
    return bytes;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the principal, then what it moves.
std::expected<std::string, std::string> upload(Harness& harness, std::string const& principal,
                                               std::string const& bytes, std::string const& action,
                                               std::vector<double>* progress = nullptr) {
    FileTransfer const transfer{harness.exchange(), as(principal)};
    std::optional<std::expected<std::string, std::string>> result;
    transfer.upload(
        FileInfo{.name = "scan.bin", .type = "application/octet-stream", .size = 0}, bytes, "Lims_Sample", action,
        [progress](double fraction) {
            if (progress != nullptr) {
                progress->push_back(fraction);
            }
        },
        [&result](std::expected<std::string, std::string> done) { result = std::move(done); });
    REQUIRE(result.has_value());
    return *result;
}

std::expected<DownloadedFile, std::string> download(Harness& harness, std::string const& principal,
                                                    std::string const& ref) {
    FileTransfer const transfer{harness.exchange(), as(principal)};
    std::optional<std::expected<DownloadedFile, std::string>> result;
    transfer.download(ref, {},
                      [&result](std::expected<DownloadedFile, std::string> done) { result = std::move(done); });
    REQUIRE(result.has_value());
    return *result;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

// Mutation: send the whole file in one chunk (three exchanges, one progress report).
TEST_CASE("ui-file uploads in chunks with progress, and the uploader downloads the same bytes",
          "[ui-service][files]") {
    Harness harness;
    std::string const bytes = pattern((2 * morph::ui::kMaxFileChunkBytes) + 1000);
    std::vector<double> progress;
    auto const ref = upload(harness, "tech", bytes, "Lims_Attach", &progress);
    REQUIRE(ref.has_value());
    CHECK(harness.exchanges == 5);  // begin, three chunks, end
    REQUIRE(progress.size() == 3);
    CHECK(progress.front() > 0.0);
    CHECK(progress.back() == 1.0);

    harness.exchanges = 0;
    auto const file = download(harness, "tech", *ref);
    REQUIRE(file.has_value());
    CHECK(file->bytes == bytes);
    CHECK(file->name == "scan.bin");
    CHECK(file->type == "application/octet-stream");
    CHECK(harness.exchanges == 3);

    auto const empty = upload(harness, "tech", std::string{}, "Lims_Attach");
    REQUIRE(empty.has_value());
    CHECK(download(harness, "tech", *empty)->bytes.empty());
}

// Mutation: skip the action's authorisation at begin (the denied upload gets a reference).
TEST_CASE("ui-file authorises an upload as its mutation, and a download by its reference", "[ui-service][files]") {
    Harness harness;
    auto const denied = upload(harness, "tech", "x", "Lims_Verify");  // tech may not run Lims_Verify
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error() == "unauthorized");

    auto const ref = upload(harness, "tech", "secret", "Lims_Attach");
    REQUIRE(ref.has_value());
    auto const other = download(harness, "lab", *ref);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error() == "not found");  // as for a reference that does not exist
    CHECK(download(harness, "tech", "file:00000000000000000000000000000000").error() == "not found");

    // A reference the application issued for a principal is that principal's to read.
    auto const issued = harness.store->issue("lab", FileInfo{.name = "r.csv", .type = "text/csv", .size = 0}, "a,b");
    REQUIRE(issued.has_value());
    CHECK(download(harness, "lab", *issued)->bytes == "a,b");
    CHECK_FALSE(download(harness, "tech", *issued).has_value());
}

TEST_CASE("ui-file refuses a chunk out of order, a malformed request, and is not served without a store",
          "[ui-service][files]") {
    Harness harness;
    Envelope const begun = harness.send(
        "tech", R"({"op":"begin","name":"a","type":"b","size":4,"model":"Lims_Sample","action":"Lims_Attach"})");
    REQUIRE(begun.kind == "ok");
    auto const reply = morph::ui::parseFileReply(begun.body);
    REQUIRE((reply && reply->upload));
    std::string const data = morph::session::detail::base64UrlEncode("ab");
    Envelope const wrong = harness.send(
        "tech", R"({"op":"chunk","upload":")" + *reply->upload + R"(","offset":2,"data":")" + data + "\"}");
    CHECK(wrong.kind == "err");
    CHECK(wrong.message == "bad offset");
    CHECK(harness.send("tech", R"({"op":"steal"})").kind == "err");
    CHECK(harness.send("tech", R"({"op":"begin","size":1})").message == "ui-file begin names no model and action");

    Harness const without{false};
    CHECK_FALSE(morph::ui::UiService{uiservice::bundle()}.serves("ui-file"));
}
