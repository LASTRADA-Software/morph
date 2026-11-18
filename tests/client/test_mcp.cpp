// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <iostream>
#include <morph/client/mcp.hpp>
#include <morph/ui/value.hpp>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "probe_app.hpp"

namespace {

// What a call passes when it needs no session.
morph::session::Context const kNoSession;

std::vector<std::string> lines(std::string const& text) {
    std::vector<std::string> out;
    std::istringstream stream{text};
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

#ifndef _WIN32
std::string drain(int descriptor) {
    std::string text;
    std::array<char, 256> chunk{};
    for (ssize_t count = ::read(descriptor, chunk.data(), chunk.size()); count > 0;
         count = ::read(descriptor, chunk.data(), chunk.size())) {
        text.append(chunk.data(), static_cast<std::size_t>(count));
    }
    return text;
}
#endif

}  // namespace

// `agentApp`, not `app`: `app`'s catalog describes no model, so `tools/list` returns only `evaluate` for it.
TEST_CASE("serveMcp in local mode lists the application's actions and runs one", "[client][mcp]") {
    morph::client::Args args;
    args.app = "probe";
    args.mcpStdio = true;
    std::istringstream input{
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
        "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"Probe_Answer","arguments":{}}})"
        "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    int const code = morph::client::serveMcp(args, {probe::agentApp()}, kNoSession, input, output, diagnostics);
    CHECK(code == 0);
    auto const replies = lines(output.str());
    REQUIRE(replies.size() == 3);
    for (auto const& reply : replies) {
        CHECK(morph::ui::parseJson(reply).has_value());
    }
    CHECK(replies.at(0).contains(R"("serverInfo":{"name":"probe")"));
    CHECK(replies.at(1).contains(R"("name":"Probe_Answer")"));
    CHECK(replies.at(2).contains(R"("isError":false)"));
}

TEST_CASE("serveMcp refuses an application the binary does not link", "[client][mcp]") {
    morph::client::Args args;
    args.app = "nope";
    args.mcpStdio = true;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream diagnostics;
    CHECK(morph::client::serveMcp(args, {probe::agentApp()}, kNoSession, input, output, diagnostics) == 2);
    CHECK(output.str().empty());
    CHECK(diagnostics.str().contains("probe"));
}

// A wss:// URL needs the Qt transport; without it, serveMcp says which one is missing and starts nothing.
TEST_CASE("serveMcp refuses wss:// when no transport can reach it", "[client][mcp]") {
#ifdef MORPH_CLIENT_TESTS_HAVE_QT_TRANSPORT
    SUCCEED("a Qt WebSocket transport is built, which would try to connect");
#else
    std::vector<morph::client::LocalApp> const noApps;
    morph::client::Args args;
    args.server = "wss://127.0.0.1:1";
    args.mcpStdio = true;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream diagnostics;
    CHECK(morph::client::serveMcp(args, noApps, kNoSession, input, output, diagnostics) == 2);
    CHECK(output.str().empty());
    CHECK(diagnostics.str().contains("Qt WebSockets"));
#endif
}

#ifndef _WIN32
// Run in a child process, since claiming stdout redirects the process's descriptor 1: the child's stdout and stderr
// are pipes, and it writes through the claimed stream and through `std::cout`.
TEST_CASE("claimStdout leaves only the claimed stream on the original stdout", "[client][mcp]") {
    std::array<int, 2> out{};
    std::array<int, 2> err{};
    REQUIRE(::pipe(out.data()) == 0);
    REQUIRE(::pipe(err.data()) == 0);
    pid_t const child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // The test binary has run threads, and a fork carries only this one: a lock another thread held (the
        // allocator's, a stream's) stays held here. The child therefore does nothing beyond what claimStdout needs and
        // a write to each stream, and leaves with `_exit`, which runs no atexit handler or static destructor.
        ::dup2(out.at(1), 1);
        ::dup2(err.at(1), 2);
        ::close(out.at(0));
        ::close(err.at(0));
        ::close(out.at(1));
        ::close(err.at(1));
        auto const channel = morph::client::claimStdout();
        if (channel == nullptr) {
            ::_exit(3);
        }
        *channel << "rpc\n" << std::flush;
        std::cout << "stray\n" << std::flush;
        channel->flush();
        ::_exit(0);
    }
    ::close(out.at(1));
    ::close(err.at(1));
    std::string const claimed = drain(out.at(0));
    std::string const logged = drain(err.at(0));
    ::close(out.at(0));
    ::close(err.at(0));
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(claimed == "rpc\n");
    CHECK(logged == "stray\n");
}
#endif
