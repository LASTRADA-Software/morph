// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base64.hpp"
#include "sha1.hpp"

namespace morph::net::detail {

/// @brief The RFC 6455 §1.3 magic GUID appended to a client's key before hashing.
inline constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// @brief Computes the `Sec-WebSocket-Accept` value for a given client key.
/// @param clientKey The `Sec-WebSocket-Key` header value sent by the client.
/// @return `base64(sha1(clientKey + kWebSocketGuid))`, per RFC 6455 §1.3.
inline std::string computeAcceptKey(std::string_view clientKey) {
    std::string concatenated{clientKey};
    concatenated += kWebSocketGuid;
    auto digest = sha1Digest(concatenated);
    return base64Encode(digest);
}

/// @brief Generates a random 16-byte `Sec-WebSocket-Key`, base64-encoded.
/// @return A 24-character base64 string suitable for the `Sec-WebSocket-Key` header.
inline std::string generateClientKey() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<int> dist{0, 255};
    std::array<std::uint8_t, 16> raw{};
    for (auto& b : raw) {
        b = static_cast<std::uint8_t>(dist(gen));
    }
    return base64Encode(raw);
}

/// @brief The pieces of a `ws://` URL relevant to opening a socket + handshake.
struct ParsedWsUrl {
    /// @brief Hostname or numeric address to connect to.
    std::string host;
    /// @brief TCP port to connect to.
    std::uint16_t port{0};
    /// @brief HTTP request path (defaults to `/` when the URL has none).
    std::string path;
};

/// @brief Parses a `ws://host:port[/path]` URL.
/// @param url The URL to parse.
/// @return The parsed host/port/path.
/// @throws std::runtime_error if @p url is `wss://` (unsupported — see
///         `docs/spec/core/backend.md`'s `morph::net` section), does not start
///         with `ws://`, has no explicit port, or has an invalid port.
inline ParsedWsUrl parseWsUrl(std::string_view url) {
    constexpr std::string_view kWsScheme = "ws://";
    constexpr std::string_view kWssScheme = "wss://";
    if (url.substr(0, kWssScheme.size()) == kWssScheme) {
        throw std::runtime_error(
            "parseWsUrl: wss:// is not supported by morph::net (plaintext ws:// only; TLS is future work)");
    }
    if (url.substr(0, kWsScheme.size()) != kWsScheme) {
        throw std::runtime_error("parseWsUrl: URL must start with ws://");
    }
    std::string_view rest = url.substr(kWsScheme.size());
    auto slashPos = rest.find('/');
    std::string_view hostPort = (slashPos == std::string_view::npos) ? rest : rest.substr(0, slashPos);
    std::string path = (slashPos == std::string_view::npos) ? "/" : std::string(rest.substr(slashPos));
    auto colonPos = hostPort.rfind(':');
    if (colonPos == std::string_view::npos || colonPos == 0) {
        throw std::runtime_error("parseWsUrl: URL must include an explicit host and port (e.g. ws://host:1234)");
    }
    std::string host{hostPort.substr(0, colonPos)};
    std::string_view portStr = hostPort.substr(colonPos + 1);
    if (portStr.empty()) {
        throw std::runtime_error("parseWsUrl: invalid port in URL");
    }
    int portVal = 0;
    for (char ch : portStr) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("parseWsUrl: invalid port in URL");
        }
        portVal = (portVal * 10) + (ch - '0');
        if (portVal > 65535) {
            throw std::runtime_error("parseWsUrl: port out of range");
        }
    }
    if (portVal == 0) {
        throw std::runtime_error("parseWsUrl: invalid port in URL");
    }
    return ParsedWsUrl{std::move(host), static_cast<std::uint16_t>(portVal), std::move(path)};
}

/// @brief Builds the client's HTTP/1.1 Upgrade request.
/// @param url The target host/port/path.
/// @param key The `Sec-WebSocket-Key` to send (see `generateClientKey`).
/// @return The full request, including the trailing blank-line terminator.
inline std::string buildClientHandshakeRequest(const ParsedWsUrl& url, std::string_view key) {
    std::string req;
    req += "GET " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host + ":" + std::to_string(url.port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + std::string(key) + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";
    return req;
}

/// @brief Builds the server's HTTP/1.1 101 Switching Protocols response.
/// @param clientKey The `Sec-WebSocket-Key` value the client sent.
/// @return The full response, including the trailing blank-line terminator.
inline std::string buildServerHandshakeResponse(std::string_view clientKey) {
    std::string resp;
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: " + computeAcceptKey(clientKey) + "\r\n";
    resp += "\r\n";
    return resp;
}

namespace ws_handshake_impl {

inline std::string toLowerAscii(std::string_view s) {
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

inline std::vector<std::string_view> splitLines(std::string_view raw) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < raw.size()) {
        auto pos = raw.find("\r\n", start);
        if (pos == std::string_view::npos) {
            lines.push_back(raw.substr(start));
            break;
        }
        lines.push_back(raw.substr(start, pos - start));
        start = pos + 2;
    }
    return lines;
}

inline std::string_view trimLeadingSpace(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') {
        ++i;
    }
    return s.substr(i);
}

}  // namespace ws_handshake_impl

/// @brief The pieces of a client handshake request relevant to completing it.
struct ClientHandshakeRequest {
    /// @brief The `Sec-WebSocket-Key` header value.
    std::string key;
    /// @brief The request-line path (e.g. `/`).
    std::string path;
};

/// @brief Parses a client's HTTP/1.1 Upgrade request (header block only, no
///        trailing blank line).
/// @param headerBlock The request's header lines, joined by `\r\n`, with no
///                     trailing `\r\n\r\n` (see `readHttpHeaderBlock`, Task 6).
/// @return The extracted key and path.
/// @throws std::runtime_error if the request line is not a `GET`, or the
///         `Sec-WebSocket-Key`/`Upgrade` headers are missing.
inline ClientHandshakeRequest parseClientHandshakeRequest(std::string_view headerBlock) {
    auto lines = ws_handshake_impl::splitLines(headerBlock);
    if (lines.empty()) {
        throw std::runtime_error("parseClientHandshakeRequest: empty request");
    }
    std::string_view requestLine = lines.front();
    if (requestLine.substr(0, 4) != "GET ") {
        throw std::runtime_error("parseClientHandshakeRequest: expected a GET request line");
    }
    std::size_t const pathStart = 4;
    std::size_t const pathEnd = requestLine.find(' ', pathStart);
    if (pathEnd == std::string_view::npos) {
        throw std::runtime_error("parseClientHandshakeRequest: malformed request line");
    }
    std::string path{requestLine.substr(pathStart, pathEnd - pathStart)};

    std::string key;
    bool hasUpgrade = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string name = ws_handshake_impl::toLowerAscii(line.substr(0, colon));
        std::string_view value = ws_handshake_impl::trimLeadingSpace(line.substr(colon + 1));
        if (name == "sec-websocket-key") {
            key = std::string(value);
        } else if (name == "upgrade") {
            hasUpgrade = true;
        }
    }
    if (key.empty()) {
        throw std::runtime_error("parseClientHandshakeRequest: missing Sec-WebSocket-Key header");
    }
    if (!hasUpgrade) {
        throw std::runtime_error("parseClientHandshakeRequest: missing Upgrade header");
    }
    return ClientHandshakeRequest{std::move(key), std::move(path)};
}

/// @brief Verifies the server's HTTP/1.1 101 response against the key the
///        client sent.
/// @param headerBlock The response's header lines, joined by `\r\n`, with no
///                     trailing `\r\n\r\n` (see `readHttpHeaderBlock`, Task 6).
/// @param clientKey   The `Sec-WebSocket-Key` this client sent in its request.
/// @throws std::runtime_error if the status line is not `101`, or
///         `Sec-WebSocket-Accept` does not match `computeAcceptKey(clientKey)`.
inline void verifyServerHandshakeResponse(std::string_view headerBlock, std::string_view clientKey) {
    auto lines = ws_handshake_impl::splitLines(headerBlock);
    if (lines.empty()) {
        throw std::runtime_error("verifyServerHandshakeResponse: empty response");
    }
    std::string_view statusLine = lines.front();
    if (statusLine.find(" 101 ") == std::string_view::npos) {
        throw std::runtime_error("verifyServerHandshakeResponse: server did not return 101 Switching Protocols: " +
                                 std::string(statusLine));
    }
    std::string accept;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string name = ws_handshake_impl::toLowerAscii(line.substr(0, colon));
        if (name == "sec-websocket-accept") {
            accept = std::string(ws_handshake_impl::trimLeadingSpace(line.substr(colon + 1)));
        }
    }
    std::string expected = computeAcceptKey(clientKey);
    if (accept != expected) {
        throw std::runtime_error("verifyServerHandshakeResponse: Sec-WebSocket-Accept mismatch");
    }
}

}  // namespace morph::net::detail
