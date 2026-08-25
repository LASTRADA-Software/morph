// SPDX-License-Identifier: Apache-2.0
#include "kanban/http/attachment_server.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <QByteArray>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>

#include "kanban/core/types.hpp"
#include "kanban/db/kanban_entity.hpp"
#include "kanban/dto/attachment_dto.hpp"

namespace kanban::http {

namespace {

/// @brief Wall-clock now, ms since epoch -- same shape as
///        `morph::session::systemClockMs`, duplicated here rather than
///        shared so this header stays free of a `session_auth.hpp`-internal
///        dependency (that function is not exported for reuse).
[[nodiscard]] std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief Mints a fresh storage key: 32 random bytes from `std::random_device`,
///        hex-encoded (64 characters). See the class doc comment / this
///        file's header for why a random token, not a content hash, was
///        chosen.
[[nodiscard]] std::string mintStorageKey() {
    std::random_device rd;
    std::array<unsigned char, 32> bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(rd() & 0xff);
    }
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        out.push_back(kHex[(byte >> 4) & 0x0f]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

/// @brief The caller's own role on @p projectDbId, or `std::nullopt` if they
///        have none. Duplicated from `kanban::(anonymous)::loadCallerRole`
///        (`board_model.cpp`, also duplicated again in
///        `project_admin_model.cpp`) rather than shared or exposed from
///        either -- this file already follows the same "each model gets its
///        own copy" precedent design spec §3 establishes for that helper
///        (both existing copies are file-local, anonymous-namespace-scoped
///        functions, not declared in any header), and this HTTP server is a
///        third, independent translation unit with the same shape of need:
///        "does this principal hold at least this role on this project."
[[nodiscard]] std::optional<Role> loadCallerRole(::Lightweight::DataMapper& mapper, std::uint64_t projectDbId,
                                                 const std::string& principal) {
    auto rows = mapper.Query<db::ProjectRoleRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::project>, "=", projectDbId)
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::principal>, "=", principal)
                    .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return roleFromString(rows.front().role.Value().str());
}

/// @brief Resolves whether @p principal may read the attachment blob named by
///        @p storageKey: looks up the `AttachmentRecord` row matching
///        `storageKey`, follows it to its owning `TaskRecord`, then that
///        task's `ProjectRecord`, and checks @p principal holds at least
///        `Role::Viewer` there -- the same read bar
///        `BoardModel::execute(const GetAttachments&)` enforces
///        (`requireRole(Role::Viewer)` + `requireTaskBelongsToProject`,
///        `board_model.cpp`), reconstructed here since the HTTP server has no
///        `BoardModel` instance (and no `_projectIdStr` to gate against --
///        the project is only known *after* resolving the storage key, not
///        ambient like it is inside an already-`OpenBoard`'d `BoardModel`).
/// @return `true` only if a matching `AttachmentRecord` row exists AND its
///         owning project grants @p principal at least `Role::Viewer`.
///         `false` for a nonexistent `storageKey`, a dangling row whose task
///         or project no longer resolves, or an authenticated principal with
///         no (or too low a) role on the owning project -- deliberately
///         collapsed to one boolean so the caller cannot accidentally emit a
///         different status code for "key doesn't exist" vs. "key exists but
///         you have no access," which would leak existence to an
///         unauthorized prober.
[[nodiscard]] bool callerMayReadAttachment(std::string_view storageKey, const std::string& principal) {
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto attachmentRows =
        mapper->Query<db::AttachmentRecord>()
            .Where(::Lightweight::FieldNameOf<&db::AttachmentRecord::storageKey>, "=", std::string{storageKey})
            .All();
    if (attachmentRows.empty()) {
        return false;
    }
    auto taskRows =
        mapper->Query<db::TaskRecord>()
            .Where(::Lightweight::FieldNameOf<&db::TaskRecord::id>, "=", attachmentRows.front().task.Value())
            .All();
    if (taskRows.empty()) {
        // Cross-tenant/dangling-FK re-check discipline (design spec §2):
        // TaskRecord::project is FK-shaped but not FK-enforced by SQLite, so
        // a task row that has vanished out from under an attachment row must
        // fail closed here, not be treated as "no restriction."
        return false;
    }
    const std::uint64_t projectDbId = taskRows.front().project.Value();
    const auto role = loadCallerRole(mapper.Get(), projectDbId, principal);
    return role.has_value() && static_cast<std::uint8_t>(*role) >= static_cast<std::uint8_t>(Role::Viewer);
}

/// @brief A storage key must be exactly the shape `mintStorageKey()` produces
///        (64 lowercase hex characters) -- rejecting anything else before it
///        ever reaches `std::filesystem::path` construction closes off path
///        traversal (`../../etc/passwd`), absolute-path escape, and null-byte
///        tricks in one check, rather than trying to escape/sanitize a
///        general string.
[[nodiscard]] bool isValidStorageKey(std::string_view key) noexcept {
    if (key.size() != 64) {
        return false;
    }
    for (const char chr : key) {
        const bool isDigit = chr >= '0' && chr <= '9';
        const bool isLowerHex = chr >= 'a' && chr <= 'f';
        if (!isDigit && !isLowerHex) {
            return false;
        }
    }
    return true;
}

/// @brief The value this server falls back to whenever an
///        `X-Attachment-Content-Type` header (or a `.contenttype` sidecar
///        file read back at `GET` time) fails `isPlausibleMediaType` below.
inline constexpr std::string_view kDefaultContentType = "application/octet-stream";

/// @brief Strict allowlist check for a MIME media-type-shaped string:
///        `type/subtype`, both halves non-empty and drawn only from
///        `[A-Za-z0-9!#$&^_.+-]` (RFC 7231 §3.1.1.1's `token` charset,
///        restricted to what a media type actually uses -- no `*`, no
///        quoted-string parameters), overall length capped at
///        `kMaxAttachmentContentTypeBytes` (the same bound
///        `AddAttachment::validate()` -- Task 16, `attachment_dto.hpp` --
///        already enforces for this same logical field, reused rather than
///        inventing a second bound for the same value).
///
/// This is the single choke point protecting the `GET` response's
/// `Content-Type:` header line from injection: a value containing `\r` or
/// `\n` (this server's own `parseHeaders` only splits on `\r\n`, but a
/// lenient intermediary might honor a bare `\n` as a line terminator) must
/// never reach `buildResponse` unvalidated. Anything that fails this check
/// is *not* an upload error -- content type is convenience metadata, not a
/// security-critical field in its own right -- so callers substitute
/// `kDefaultContentType` rather than rejecting the request.
[[nodiscard]] bool isPlausibleMediaType(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxAttachmentContentTypeBytes) {
        return false;
    }
    const auto isAllowedChar = [](char chr) noexcept {
        const bool isAlnum = (chr >= 'A' && chr <= 'Z') || (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9');
        switch (chr) {
            case '!':
            case '#':
            case '$':
            case '&':
            case '^':
            case '_':
            case '.':
            case '+':
            case '-':
                return true;
            default:
                return isAlnum;
        }
    };
    const auto slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash == value.size() - 1) {
        return false;  // no '/', or an empty type/subtype half
    }
    const std::string_view type = value.substr(0, slash);
    const std::string_view subtype = value.substr(slash + 1);
    if (subtype.find('/') != std::string_view::npos) {
        return false;  // more than one '/' -- not a plain type/subtype shape
    }
    for (const char chr : type) {
        if (!isAllowedChar(chr)) {
            return false;
        }
    }
    for (const char chr : subtype) {
        if (!isAllowedChar(chr)) {
            return false;
        }
    }
    return true;
}

/// @brief Validates @p value as a media type, substituting
///        `kDefaultContentType` if it does not pass `isPlausibleMediaType`
///        (covers both an absent/malformed upload header and a `\r`/`\n`-
///        bearing injection attempt alike -- both fail closed to the same
///        safe default). Applied at both the point a `Content-Type` value is
///        captured (upload) and the point one is read back (`GET`), since
///        the on-disk `.contenttype` sidecar file could in principle be
///        written by something other than this exact upload code path.
[[nodiscard]] std::string sanitizedContentType(std::string_view value) {
    if (isPlausibleMediaType(value)) {
        return std::string{value};
    }
    return std::string{kDefaultContentType};
}

/// @brief One parsed HTTP request line + headers (case-insensitively looked
///        up). Not a general HTTP parser -- only the handful of fields this
///        server actually reads.
struct ParsedRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;  // lower-cased keys
    bool valid = false;
};

/// @brief Lower-cases an ASCII string (header names/values are ASCII per RFC 7230).
[[nodiscard]] std::string toLowerAscii(std::string_view in) {
    std::string out{in};
    for (char& chr : out) {
        if (chr >= 'A' && chr <= 'Z') {
            chr = static_cast<char>(chr - 'A' + 'a');
        }
    }
    return out;
}

/// @brief Strips leading/trailing spaces and horizontal tabs (RFC 7230 header value OWS).
[[nodiscard]] std::string_view trimOws(std::string_view in) {
    while (!in.empty() && (in.front() == ' ' || in.front() == '\t')) {
        in.remove_prefix(1);
    }
    while (!in.empty() && (in.back() == ' ' || in.back() == '\t')) {
        in.remove_suffix(1);
    }
    return in;
}

/// @brief Parses the header block (everything up to, not including, the
///        blank line) of an HTTP/1.x request. Malformed input (no request
///        line, no method/path, a header line with no `:`) yields
///        `valid == false` rather than throwing or asserting -- an
///        adversarial or truncated client is ordinary input to this parser,
///        never an exceptional one.
[[nodiscard]] ParsedRequest parseHeaders(std::string_view headerBlock) {
    ParsedRequest result;
    std::size_t lineStart = 0;
    bool firstLine = true;
    while (lineStart <= headerBlock.size()) {
        const auto lineEnd = headerBlock.find("\r\n", lineStart);
        const std::string_view line = headerBlock.substr(
            lineStart, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);
        if (firstLine) {
            firstLine = false;
            const auto sp1 = line.find(' ');
            if (sp1 == std::string_view::npos) {
                return result;  // invalid: no method/path separator
            }
            const auto sp2 = line.find(' ', sp1 + 1);
            if (sp2 == std::string_view::npos) {
                return result;  // invalid: no path/version separator
            }
            result.method = std::string{line.substr(0, sp1)};
            result.path = std::string{line.substr(sp1 + 1, sp2 - sp1 - 1)};
            if (result.method.empty() || result.path.empty()) {
                return result;
            }
        } else if (!line.empty()) {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                return result;  // invalid: header line with no ':'
            }
            const std::string name = toLowerAscii(trimOws(line.substr(0, colon)));
            const std::string value{trimOws(line.substr(colon + 1))};
            if (name.empty()) {
                return result;
            }
            result.headers[name] = value;
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 2;
    }
    result.valid = !result.method.empty() && !result.path.empty();
    return result;
}

/// @brief Extracts the bearer token from an `Authorization: Bearer <token>`
///        header value, or `nullopt` if the header is missing/malformed.
[[nodiscard]] std::optional<std::string> extractBearerToken(const ParsedRequest& req) {
    const auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    static constexpr std::string_view kPrefix = "Bearer ";
    if (it->second.size() <= kPrefix.size() ||
        toLowerAscii(std::string_view{it->second}.substr(0, kPrefix.size())) != toLowerAscii(kPrefix)) {
        return std::nullopt;
    }
    return it->second.substr(kPrefix.size());
}

/// @brief Builds a minimal well-formed HTTP/1.1 response with @p body as
///        the entity, `Connection: close` (this server serves one request
///        per connection), and @p contentType (defaulting to a value safe
///        for both JSON error bodies and arbitrary attachment bytes).
[[nodiscard]] QByteArray buildResponse(int status, std::string_view statusText, std::string_view body,
                                       std::string_view contentType = "application/json") {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    const std::string text = out.str();
    return QByteArray{text.data(), static_cast<int>(text.size())};
}

/// @brief A JSON body carrying a single `"error"` string field. Hand-built
///        (not Glaze) since the error text is a fixed, known-safe literal in
///        every call site below -- never untrusted input reflected back.
[[nodiscard]] std::string errorJson(std::string_view message) {
    return "{\"error\":\"" + std::string{message} + "\"}";
}

}  // namespace

AttachmentServer::AttachmentServer(const ::morph::session::TokenVerifier& verifier, Config cfg, QObject* parent)
    : QObject{parent}, _verifier{verifier}, _cfg{std::move(cfg)}, _listener{this} {
    std::filesystem::create_directories(_cfg.storageDir);
    connect(&_listener, &QTcpServer::newConnection, this, &AttachmentServer::onNewConnection);
}

AttachmentServer::~AttachmentServer() { close(); }

bool AttachmentServer::listen(quint16 port) { return _listener.listen(_cfg.bindAddress, port); }

quint16 AttachmentServer::port() const { return _listener.serverPort(); }

void AttachmentServer::close() {
    _listener.close();
    for (auto& [socket, state] : _connections) {
        socket->deleteLater();
    }
    _connections.clear();
}

void AttachmentServer::onNewConnection() {
    while (QTcpSocket* socket = _listener.nextPendingConnection()) {
        _connections[socket];  // default-construct ConnectionState
        connect(socket, &QTcpSocket::readyRead, this, &AttachmentServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &AttachmentServer::onDisconnected);
    }
}

void AttachmentServer::onReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket == nullptr) {
        return;
    }
    const auto stateIt = _connections.find(socket);
    if (stateIt == _connections.end()) {
        return;
    }
    ConnectionState& state = stateIt->second;
    if (state.responded) {
        return;
    }
    state.buffer.append(socket->readAll());

    if (!state.headersParsed) {
        const auto headerEnd = state.buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            // Headers not fully received yet -- keep buffering, but never
            // past a sane header-block size, so a client that never sends
            // "\r\n\r\n" cannot make this server buffer unboundedly.
            constexpr qint64 kMaxHeaderBytes = 16 * 1024;
            if (state.buffer.size() > kMaxHeaderBytes) {
                socket->write(buildResponse(400, "Bad Request", errorJson("headers too large")));
                state.responded = true;
                socket->disconnectFromHost();
            }
            return;
        }
    }

    handleRequest(socket, state);
}

void AttachmentServer::onDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket == nullptr) {
        return;
    }
    _connections.erase(socket);
    socket->deleteLater();
}

void AttachmentServer::respondAndClose(QTcpSocket* socket, ConnectionState& state, const QByteArray& response) {
    socket->write(response);
    state.responded = true;
    socket->disconnectFromHost();
}

void AttachmentServer::handleRequest(QTcpSocket* socket, ConnectionState& state) {
    if (state.headersParsed) {
        // Headers (and route/auth/size checks) already handled on an
        // earlier onReadyRead call for this connection -- this call is
        // delivering more of a POST body that arrived across multiple TCP
        // reads. The declared Content-Length was already checked against
        // maxBodyBytes before state.headersParsed was set, but a client's
        // actual byte stream is not obligated to match what it declared: a
        // dishonest Content-Length (small) followed by an unbounded stream
        // on the same connection must not be allowed to grow this buffer
        // past the configured bound regardless of what the header claimed.
        if (state.buffer.size() - state.bodyStart > _cfg.maxBodyBytes) {
            respondAndClose(
                socket, state,
                buildResponse(413, "Payload Too Large", errorJson("attachment exceeds the configured size bound")));
            return;
        }
        const qint64 bodyBytesSoFar = state.buffer.size() - state.bodyStart;
        if (bodyBytesSoFar < state.contentLength) {
            return;  // still waiting for more of the body
        }
        finishUpload(socket, state);
        return;
    }

    const auto headerEnd = state.buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return;  // onReadyRead already re-checks this before calling in
    }
    const std::string_view headerBlock{state.buffer.constData(), static_cast<std::size_t>(headerEnd)};
    const ParsedRequest req = parseHeaders(headerBlock);
    state.bodyStart = headerEnd + 4;

    if (!req.valid) {
        respondAndClose(socket, state, buildResponse(400, "Bad Request", errorJson("malformed request")));
        return;
    }

    // Authentication first, before any route logic or body handling: an
    // unauthenticated caller is rejected before this server ever inspects
    // (let alone buffers or writes) a single body byte, upload or download.
    // The verified SessionToken (not just a bool) is kept: GET below needs
    // its principal to check *authorization*, not merely that the token was
    // validly signed and unexpired -- authentication alone would let any
    // authenticated principal (a valid token for ANY project) read any
    // attachment blob by storage key. See the class doc comment's
    // "Authorization (not just authentication)" section.
    const auto token = extractBearerToken(req);
    const auto verified =
        token ? _verifier.verify(*token, nowMs()) : std::unexpected(::morph::session::AuthError::Malformed);
    if (!verified.has_value()) {
        respondAndClose(socket, state,
                        buildResponse(401, "Unauthorized", errorJson("missing or invalid bearer token")));
        return;
    }
    const std::string& principal = verified->principal;

    if (req.method == "GET" && req.path.starts_with("/attachments/")) {
        const std::string key = req.path.substr(std::string_view{"/attachments/"}.size());
        if (!isValidStorageKey(key)) {
            respondAndClose(socket, state, buildResponse(404, "Not Found", errorJson("not found")));
            return;
        }
        // Authorization: does `principal` (already authenticated above) hold
        // at least Viewer on the project this storageKey's attachment
        // belongs to? A nonexistent key and an existing-but-unauthorized key
        // are deliberately indistinguishable to the caller -- both 404 --
        // so this check is never allowed to leak "this key exists" via a
        // different status code (403) to a caller with no standing on it.
        if (!callerMayReadAttachment(key, principal)) {
            respondAndClose(socket, state, buildResponse(404, "Not Found", errorJson("not found")));
            return;
        }
        const auto blobPath = _cfg.storageDir / key;
        std::error_code existsEc;
        if (!std::filesystem::exists(blobPath, existsEc) || existsEc) {
            // Covers both "never uploaded" and the dangling-metadata-row
            // case (Task 16's AddAttachment called with a storageKey no
            // upload ever produced) identically: 404, not a crash. See the
            // class doc comment.
            respondAndClose(socket, state, buildResponse(404, "Not Found", errorJson("not found")));
            return;
        }
        std::ifstream in{blobPath, std::ios::binary};
        std::ostringstream contents;
        contents << in.rdbuf();
        std::string contentType{kDefaultContentType};
        if (std::ifstream metaIn{_cfg.storageDir / (key + ".contenttype"), std::ios::binary}) {
            std::ostringstream metaContents;
            metaContents << metaIn.rdbuf();
            // Re-validated here, not just trusted from having (presumably)
            // already been sanitized at upload time: the sidecar is a plain
            // file on disk, and this defense must hold even if something
            // other than finishUpload() ever wrote to it (defense in depth,
            // per the response-header-injection finding).
            contentType = sanitizedContentType(metaContents.str());
        }
        respondAndClose(socket, state, buildResponse(200, "OK", contents.str(), contentType));
        return;
    }

    if (req.method == "POST" && req.path == "/attachments") {
        const auto contentLenIt = req.headers.find("content-length");
        if (contentLenIt == req.headers.end()) {
            respondAndClose(socket, state,
                            buildResponse(411, "Length Required", errorJson("Content-Length is required")));
            return;
        }
        std::int64_t declaredLength = -1;
        try {
            declaredLength = std::stoll(contentLenIt->second);
        } catch (const std::exception&) {
            respondAndClose(socket, state, buildResponse(400, "Bad Request", errorJson("malformed Content-Length")));
            return;
        }
        if (declaredLength < 0) {
            respondAndClose(socket, state, buildResponse(400, "Bad Request", errorJson("malformed Content-Length")));
            return;
        }
        // The size bound is enforced right here, against the *declared*
        // length -- before any body byte beyond what this one socket read
        // already delivered is accepted. An oversized upload never reaches
        // the point of being buffered in full: it is rejected the moment
        // its own Content-Length header says it will not fit.
        if (declaredLength > _cfg.maxBodyBytes) {
            respondAndClose(
                socket, state,
                buildResponse(413, "Payload Too Large", errorJson("attachment exceeds the configured size bound")));
            return;
        }

        // Validated (fail-closed to kDefaultContentType) before it is ever
        // kept on state, let alone written to the `.contenttype` sidecar
        // file -- an attacker-supplied header value containing `\r`/`\n`
        // must never survive to be interpolated into a future GET response's
        // `Content-Type:` header line (response-header-injection finding).
        const auto contentTypeIt = req.headers.find("x-attachment-content-type");
        state.uploadContentType = contentTypeIt != req.headers.end() ? sanitizedContentType(contentTypeIt->second)
                                                                     : std::string{kDefaultContentType};
        state.contentLength = declaredLength;
        state.headersParsed = true;

        const qint64 bodyBytesSoFar = state.buffer.size() - state.bodyStart;
        if (bodyBytesSoFar < declaredLength) {
            return;  // wait for onReadyRead to deliver the rest
        }
        finishUpload(socket, state);
        return;
    }

    respondAndClose(socket, state, buildResponse(404, "Not Found", errorJson("no such route")));
}

void AttachmentServer::finishUpload(QTcpSocket* socket, ConnectionState& state) {
    const std::string storageKey = mintStorageKey();
    {
        std::ofstream out{_cfg.storageDir / storageKey, std::ios::binary | std::ios::trunc};
        out.write(state.buffer.constData() + state.bodyStart, state.contentLength);
    }
    {
        std::ofstream metaOut{_cfg.storageDir / (storageKey + ".contenttype"), std::ios::binary | std::ios::trunc};
        metaOut << state.uploadContentType;
    }
    respondAndClose(socket, state, buildResponse(200, "OK", "{\"storageKey\":\"" + storageKey + "\"}"));
}

}  // namespace kanban::http
