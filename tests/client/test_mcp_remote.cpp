// SPDX-License-Identifier: Apache-2.0

// serveMcp in remote mode over morph::net: a SocketServer over an in-process server built from probe::agentApp(), so
// the remote binary needs no application of its own.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <istream>
#include <memory>
#include <morph/client/mcp.hpp>
#include <morph/core/executor.hpp>
#include <morph/net/socket_server.hpp>
#include <mutex>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "../../src/client/mcp_internal.hpp"
#include "probe_app.hpp"

namespace {

// What a call passes when it needs no session and no linked application.
morph::session::Context const kNoSession;
std::vector<morph::client::LocalApp> const kNoApps;

// An input a test feeds line by line: reading blocks until a line is pushed or the input is closed, as a pipe does.
class PipeBuf final : public std::streambuf {
public:
    void push(std::string const& text) {
        {
            std::scoped_lock const lock{_mutex};
            _pending.append(text);
        }
        _ready.notify_all();
    }

    void close() {
        {
            std::scoped_lock const lock{_mutex};
            _closed = true;
        }
        _ready.notify_all();
    }

protected:
    int_type underflow() override {
        std::unique_lock lock{_mutex};
        _ready.wait(lock, [this] { return !_pending.empty() || _closed; });
        if (_pending.empty()) {
            return traits_type::eof();
        }
        _chunk = std::move(_pending);
        _pending.clear();
        std::span<char> const view{_chunk};
        setg(view.data(), view.data(), std::to_address(view.end()));
        return traits_type::to_int_type(*gptr());
    }

private:
    std::mutex _mutex;
    std::condition_variable _ready;
    std::string _pending;
    std::string _chunk;
    bool _closed = false;
};

std::size_t countOf(std::string const& text, std::string const& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

std::string url(std::uint16_t port) { return "ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(port)); }

}  // namespace

TEST_CASE("serveMcp in remote mode answers over a WebSocket", "[client][mcp][net]") {
    morph::exec::ThreadPoolExecutor workers{2};
    auto const server = morph::client::makeLocalServer(probe::agentApp(), workers);
    REQUIRE(server.has_value());
    morph::net::SocketServer socket{**server, 0};
    REQUIRE(socket.listen());
    morph::client::Args args;
    args.server = url(socket.port());
    args.mcpStdio = true;
    std::istringstream input{R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
                             "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    CHECK(morph::client::serveMcp(args, kNoApps, kNoSession, input, output, diagnostics) == 0);
    CHECK(output.str().contains(R"("name":"evaluate")"));
    // `evaluate` plus at least one of the application's own actions.
    CHECK(countOf(output.str(), R"("name":")") > 1);
}

// Mutation: build the transport with retryFirstConnect off; the first connect is refused for good, the call fails, and
// the reply lists no tool.
TEST_CASE("serveMcp in remote mode reaches a server that starts after it", "[client][mcp][net]") {
    morph::exec::ThreadPoolExecutor workers{2};
    auto const server = morph::client::makeLocalServer(probe::agentApp(), workers);
    REQUIRE(server.has_value());
    // A port nothing listens on: bound, then released.
    std::uint16_t port = 0;
    {
        morph::net::SocketServer reserve{**server, 0};
        REQUIRE(reserve.listen());
        port = reserve.port();
    }

    morph::client::Args args;
    args.server = url(port);
    args.mcpStdio = true;
    PipeBuf pipe;
    std::istream input{&pipe};
    std::ostringstream output;
    std::ostringstream diagnostics;
    auto finished = std::async(std::launch::async, [&] {
        return morph::client::serveMcp(args, kNoApps, kNoSession, input, output, diagnostics);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    morph::net::SocketServer late{**server, port};
    REQUIRE(late.listen());
    pipe.push(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
              "\n");
    pipe.close();
    REQUIRE(finished.wait_for(std::chrono::seconds{60}) == std::future_status::ready);
    CHECK(finished.get() == 0);
    CHECK(output.str().contains(R"("name":"evaluate")"));
    CHECK(countOf(output.str(), R"("name":")") > 1);
}

// A call after the server restarted succeeds only if the adapter dropped the instance ids it cached: the new server
// has never issued them. Each server here is a fresh one, so a stale id names nothing.
// Mutation: remove `transport->backend->setDisconnectHandler(adapter.resetter())` in serveRemote; the second call
// reuses the first server's instance id, which the second server does not know, and answers an error.
// The probe model is stateless, so a second connection cannot observe a call's effect; the second server answering
// the same call is what shows the call reached the server.
TEST_CASE("serveMcp in remote mode makes a call after the server restarted", "[client][mcp][net]") {
    morph::exec::ThreadPoolExecutor workers{2};
    auto first = morph::client::makeLocalServer(probe::agentApp(), workers);
    REQUIRE(first.has_value());
    auto socket = std::make_unique<morph::net::SocketServer>(**first, 0);
    REQUIRE(socket->listen());
    std::uint16_t const port = socket->port();

    morph::client::Args args;
    args.server = url(port);
    args.mcpStdio = true;
    PipeBuf pipe;
    std::istream input{&pipe};
    std::ostringstream output;
    std::ostringstream diagnostics;
    auto finished = std::async(std::launch::async, [&] {
        return morph::client::serveMcp(args, kNoApps, kNoSession, input, output, diagnostics);
    });

    auto const awaitReplies = [&](std::size_t count) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (countOf(output.str(), R"("jsonrpc")") < count && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return countOf(output.str(), R"("jsonrpc")") >= count;
    };
    auto const call = [](int id) {
        return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
               R"(,"method":"tools/call","params":{"name":"Probe_Answer"}})"
               "\n";
    };

    pipe.push(call(1));
    REQUIRE(awaitReplies(1));
    REQUIRE(output.str().contains(R"("isError":false)"));

    socket.reset();
    first->reset();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto second = morph::client::makeLocalServer(probe::agentApp(), workers);
    REQUIRE(second.has_value());
    morph::net::SocketServer restarted{**second, port};
    REQUIRE(restarted.listen());

    // The backend reconnects on its own schedule: call until one succeeds, bounded.
    bool succeeded = false;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    for (int id = 2; !succeeded && std::chrono::steady_clock::now() < deadline; ++id) {
        pipe.push(call(id));
        REQUIRE(awaitReplies(static_cast<std::size_t>(id)));
        std::string const text = output.str();
        std::size_t const lastReply = text.rfind(R"({"jsonrpc")");
        succeeded = text.find(R"("isError":false)", lastReply) != std::string::npos;
        if (!succeeded) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
    pipe.close();
    REQUIRE(finished.wait_for(std::chrono::seconds{60}) == std::future_status::ready);
    CHECK(finished.get() == 0);
    CHECK(succeeded);
}

#ifdef MORPH_CLIENT_TESTS_HAVE_QT_TRANSPORT
// The Qt WebSocket path serves over a ws:// URL as well as wss://, so a SocketServer reaches it here.
// Mutation: leave out `options.callbacks = &owner` in serveMcpOverQt; the adapter refuses to be built and the case
// fails.
TEST_CASE("serveMcpOverQt answers over a WebSocket and refuses a URL that is not ws:// or wss://",
          "[client][mcp][net][qt]") {
    morph::exec::ThreadPoolExecutor workers{2};
    auto const server = morph::client::makeLocalServer(probe::agentApp(), workers);
    REQUIRE(server.has_value());
    morph::net::SocketServer socket{**server, 0};
    REQUIRE(socket.listen());
    std::istringstream input{R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
                             "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    CHECK(morph::client::detail::serveMcpOverQt(url(socket.port()), morph::ui::McpOptions{}, input, output,
                                                diagnostics) == 0);
    CHECK(output.str().contains(R"("name":"evaluate")"));
    CHECK(countOf(output.str(), R"("name":")") > 1);

    std::istringstream none;
    std::ostringstream refusedOutput;
    std::ostringstream refusal;
    CHECK(morph::client::detail::serveMcpOverQt("http://x", morph::ui::McpOptions{}, none, refusedOutput, refusal) ==
          2);
    CHECK(refusal.str().contains("http://x"));
}
#endif
