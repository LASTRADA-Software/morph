// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdio>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/client/mcp.hpp>
#include <morph/client/transport.hpp>
#include <morph/core/executor.hpp>
#include <morph/ui/agent.hpp>
#include <ostream>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include "mcp_internal.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace morph::client {

std::expected<void, std::string> checkMcpArgs(Args const& args, std::vector<std::string> const& rest) {
    if (!args.screen.empty()) {
        return std::unexpected(std::string{"--mcp-stdio mounts no screen: drop --screen"});
    }
    if (args.exitAfterMount) {
        return std::unexpected(std::string{"--mcp-stdio mounts no screen: drop --exit-after-mount"});
    }
    if (args.dev) {
        return std::unexpected(std::string{"--mcp-stdio has no development tools: drop --dev"});
    }
    if (rest.size() > 1) {
        return std::unexpected("--mcp-stdio takes no frontend arguments: " + rest.at(1));
    }
    return {};
}

session::Context mcpSession(Args const& args, std::function<char const*(char const*)> const& environment,
                            std::ostream& diagnostics) {
    session::Context session;
    session.principal = args.user;
    if (args.server) {
        if (session.principal.empty()) {
            if (char const* const principal = environment("MORPH_MCP_PRINCIPAL")) {
                session.principal = principal;
            }
        }
        if (char const* const token = environment("MORPH_MCP_TOKEN")) {
            session.token = token;
        }
    } else if (environment("MORPH_MCP_TOKEN") != nullptr) {
        diagnostics << "client: MORPH_MCP_TOKEN is ignored in local mode, whose server checks no token\n";
    }
    return session;
}

namespace {

int serveLocal(Args const& args, std::string const& appName, std::vector<LocalApp> const& apps,
               session::Context session, std::istream& input,
               // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the protocol's channel, then the log.
               std::ostream& output, std::ostream& diagnostics) {
    auto const found = std::ranges::find(apps, appName, &LocalApp::name);
    if (found == apps.end()) {
        diagnostics << "client: no application named " << appName << "; this client links:";
        for (LocalApp const& app : apps) {
            diagnostics << ' ' << app.name;
        }
        diagnostics << '\n';
        return 2;
    }
    if (found->setUp) {
        found->setUp(LocalSetup{.db = args.db, .user = args.user});
    }
    // Declared in the order that tears down safely: the adapter goes first, then the server, then the owner its
    // replies were delivered on, then the pool its strands ran on.
    exec::ThreadPoolExecutor workers{4};
    exec::MainThreadExecutor owner;
    auto server = makeLocalServer(*found, workers);
    if (!server) {
        diagnostics << "client: " << server.error() << '\n';
        return 2;
    }
    {
        ui::McpAdapter adapter{
            *server, ui::McpOptions{.session = std::move(session), .serverName = found->name, .callbacks = &owner}};
        ui::serveStdio(adapter, owner, input, output);
    }
    server->reset();
    return 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the protocol's channel, then the log.
int serveRemote(std::string const& url, session::Context session, std::istream& input, std::ostream& output,
                std::ostream& diagnostics) {
    ui::McpOptions options{.session = std::move(session), .serverName = "morph"};
#if MORPH_CLIENT_TRANSPORT_NET
    if (url.starts_with("ws://")) {
        // Declared in the order that tears down safely: the adapter goes first, then the transport (which rejects
        // what is still pending onto the owner), then the owner.
        exec::MainThreadExecutor owner;
        auto transport = detail::socketTransport(url, true);
        if (!transport) {
            diagnostics << "client: " << transport.error() << '\n';
            return 2;
        }
        options.callbacks = &owner;
        {
            ui::McpAdapter adapter{*transport->requests, std::move(options)};
            transport->backend->setDisconnectHandler(adapter.resetter());
            ui::serveStdio(adapter, owner, input, output);
            transport->backend->setDisconnectHandler(nullptr);
        }
        return 0;
    }
#endif
#if MORPH_CLIENT_TRANSPORT_QT
    return detail::serveMcpOverQt(url, std::move(options), input, output, diagnostics);
#else
    static_cast<void>(options);
    static_cast<void>(input);
    static_cast<void>(output);
    if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
        diagnostics << "client: not a ws:// or wss:// URL: " << url << '\n';
    } else {
        diagnostics << "client: " << url
                    << (url.starts_with("wss://") ? " needs Qt WebSockets, which are not built here\n"
                                                  : " needs morph::net or Qt WebSockets, and neither is built here\n");
    }
    return 2;
#endif
}

// A streambuf over a C stream: the duplicate of the process's original stdout.
class FileStreamBuf final : public std::streambuf {
public:
    explicit FileStreamBuf(std::FILE* file) : _file{file} {}
    ~FileStreamBuf() override {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the stream owns the C file it was handed.
        static_cast<void>(std::fclose(_file));
    }
    FileStreamBuf(FileStreamBuf const&) = delete;
    FileStreamBuf& operator=(FileStreamBuf const&) = delete;
    FileStreamBuf(FileStreamBuf&&) = delete;
    FileStreamBuf& operator=(FileStreamBuf&&) = delete;

protected:
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        return std::fputc(character, _file) != EOF ? character : traits_type::eof();
    }
    std::streamsize xsputn(char const* text, std::streamsize count) override {
        return static_cast<std::streamsize>(std::fwrite(text, 1, static_cast<std::size_t>(count), _file));
    }
    int sync() override { return std::fflush(_file) == 0 ? 0 : -1; }

private:
    std::FILE* _file;
};

class FileOStream final : public std::ostream {
public:
    explicit FileOStream(std::FILE* file) : std::ostream{nullptr}, _buffer{file} { rdbuf(&_buffer); }

private:
    FileStreamBuf _buffer;
};

}  // namespace

int serveMcp(Args const& args, std::vector<LocalApp> const& apps, session::Context session, std::istream& input,
             std::ostream& output, std::ostream& diagnostics) {
    if (args.app) {
        return serveLocal(args, *args.app, apps, std::move(session), input, output, diagnostics);
    }
    if (args.server) {
        return serveRemote(*args.server, std::move(session), input, output, diagnostics);
    }
    diagnostics << "client: --mcp-stdio needs --server or --app\n";
    return 2;
}

std::unique_ptr<std::ostream> claimStdout() {
    std::cout.flush();
    static_cast<void>(std::fflush(stdout));
#ifdef _WIN32
    int const channel = ::_dup(1);
    std::FILE* const file = channel < 0 ? nullptr : ::_fdopen(channel, "wb");
    if (file == nullptr) {
        if (channel >= 0) {
            ::_close(channel);
        }
        return nullptr;
    }
    if (::_dup2(2, 1) != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the file is this function's until a stream takes it.
        static_cast<void>(std::fclose(file));
        return nullptr;
    }
#else
    int const channel = ::dup(1);
    std::FILE* const file = channel < 0 ? nullptr : ::fdopen(channel, "w");
    if (file == nullptr) {
        if (channel >= 0) {
            ::close(channel);
        }
        return nullptr;
    }
    if (::dup2(2, 1) < 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the file is this function's until a stream takes it.
        static_cast<void>(std::fclose(file));
        return nullptr;
    }
#endif
    return std::make_unique<FileOStream>(file);
}

}  // namespace morph::client
