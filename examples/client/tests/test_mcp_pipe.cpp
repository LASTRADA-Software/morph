// SPDX-License-Identifier: Apache-2.0
//
// morph_examples_client --mcp-stdio, launched as a real process over a shell pipe: stdout carries JSON-RPC and
// nothing else, one reply per request line, and a refused start writes nothing there.

#include <sys/wait.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <morph/ui/value.hpp>
#include <string>
#include <vector>

namespace {

struct Run {
    std::vector<std::string> lines;
    int status = -1;
};

// Runs @p command through the shell; returns its stdout, one entry per line, and its exit status.
Run run(std::string const& command) {
    Run result;
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c): the command is this test's own, from its own paths.
    FILE* const pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string line;
    for (int next = std::fgetc(pipe); next != EOF; next = std::fgetc(pipe)) {
        if (next == '\n') {
            result.lines.push_back(line);
            line.clear();
        } else {
            line.push_back(static_cast<char>(next));
        }
    }
    if (!line.empty()) {
        result.lines.push_back(line);
    }
    int const status = ::pclose(pipe);
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::string const kInitializeAndList = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}\n)"
                                       R"({"jsonrpc":"2.0","id":2,"method":"tools/list"}\n)";

std::string client(std::string const& requests, std::string const& options) {
    return "printf '%b' '" + requests + "' | '" + std::string{MORPH_EXAMPLES_CLIENT_PATH} + "' --mcp-stdio " +
           options + " 2>/dev/null";
}

}  // namespace

TEST_CASE("morph_examples_client --mcp-stdio --app forms answers on a clean stdout", "[client][mcp][pipe]") {
    auto const result = run(client(kInitializeAndList, "--app forms --user alice"));
    CHECK(result.status == 0);
    REQUIRE(result.lines.size() == 2);
    for (auto const& line : result.lines) {
        CHECK(morph::ui::parseJson(line).has_value());
    }
    CHECK(result.lines.at(1).contains(R"("name":"evaluate")"));
}

#if MORPH_CLIENT_HAS_LIMS
TEST_CASE("morph_examples_client --mcp-stdio --app lims serves lims's actions", "[client][mcp][pipe][lims]") {
    auto const directory = std::filesystem::temp_directory_path() / "client_mcp_lims";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto const database = directory / "lims.db";

    auto const result = run(client(kInitializeAndList, "--app lims --db '" + database.string() + "' --user alice"));
    CHECK(result.status == 0);
    REQUIRE(result.lines.size() == 2);
    for (auto const& line : result.lines) {
        CHECK(morph::ui::parseJson(line).has_value());
    }
    auto const& tools = result.lines.at(1);
    CHECK(tools.contains(R"("name":"evaluate")"));
    // lims's own actions are tools too.
    CHECK(tools.contains(R"("name":"ListAnalyses")"));
    std::filesystem::remove_all(directory);
}
#endif

TEST_CASE("--mcp-stdio with a frontend option exits 2 and writes nothing to stdout", "[client][mcp][pipe]") {
    auto const result = run("printf '' | '" + std::string{MORPH_EXAMPLES_CLIENT_PATH} +
                            "' --mcp-stdio --app forms --screen x 2>/dev/null");
    CHECK(result.lines.empty());
    CHECK(result.status == 2);
}
