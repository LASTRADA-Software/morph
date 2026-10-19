// SPDX-License-Identifier: Apache-2.0
//
// The bank server serves an agent over MCP's stdio transport: started with
// --mcp-stdio, it answers JSON-RPC on its stdout, one reply per request line,
// and stops when its stdin ends.

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "unique_test_database.hpp"

namespace {

// Runs @p command through the shell and returns its stdout, one entry per line.
std::vector<std::string> outputLines(std::string const& command) {
    std::vector<std::string> lines;
    FILE* const pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string line;
    for (int c = std::fgetc(pipe); c != EOF; c = std::fgetc(pipe)) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(static_cast<char>(c));
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    CHECK(::pclose(pipe) == 0);
    return lines;
}

}  // namespace

TEST_CASE("the server with --mcp-stdio answers tools/list on a pipe", "[bank][mcp]") {
    bank::testing::detail::PrivateDatabase const database;
    auto const directory = std::filesystem::temp_directory_path() / "bank_mcp_stdio";
    std::filesystem::create_directories(directory);
    std::string const requests = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}\n)"
                                 R"({"jsonrpc":"2.0","method":"notifications/initialized"}\n)"
                                 R"({"jsonrpc":"2.0","id":2,"method":"tools/list"}\n)";
    std::string const command = "cd '" + directory.string() + "' && printf '%b' '" + requests + "' | BANK_DB='" +
                                database.connection() + "' BANK_PORT=0 MORPH_MCP_PRINCIPAL=alice '" +
                                std::string{BANK_SERVER_PATH} + "' --mcp-stdio 2>/dev/null";
    auto const lines = outputLines(command);
    REQUIRE(lines.size() == 2);
    CHECK(lines.at(0).contains(R"("id":1)"));
    CHECK(lines.at(0).contains(R"("serverInfo")"));
    CHECK(lines.at(1).contains(R"("id":2)"));
    CHECK(lines.at(1).contains(R"("name":"evaluate")"));
    // The bank's own actions are tools too, with their schemas.
    CHECK(lines.at(1).contains(R"("inputSchema")"));
    CHECK(lines.at(1).find(R"("name":")") != lines.at(1).rfind(R"("name":")"));
}
