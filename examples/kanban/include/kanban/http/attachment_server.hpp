// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <morph/session/session_auth.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

/// @file
/// `kanban::http::AttachmentServer` -- the HTTP side channel Task 16's
/// `AddAttachment` metadata action pairs with (README build-order step 8:
/// "bytes over a side channel, metadata through actions"). This is a small,
/// hand-rolled listener over `QTcpServer`/`QTcpSocket` -- there is no
/// `QHttpServer` dependency anywhere in this tree, and adding one for two
/// request shapes (`POST /attachments`, `GET /attachments/{storageKey}`)
/// would pull in a general-purpose HTTP module (chunked transfer encoding,
/// HTTP/2, etc.) whose whole feature surface would then need security review,
/// in exchange for no capability this server actually needs. See
/// `docs/spec/security.md`'s "Transport security" section for the sibling
/// WebSocket transport this shares its `TokenVerifier` with.

namespace kanban::http {

/// @brief Construction-time configuration for `AttachmentServer`.
///
/// Declared outside `AttachmentServer` so its default member initialisers are
/// fully parsed before any constructor default argument that names `Config{}`
/// is evaluated (same rationale as `morph::qt::QtWebSocketServerConfig`).
struct AttachmentServerConfig {
    /// @brief Directory attachment blobs are stored in. Created if it does
    ///        not already exist.
    std::filesystem::path storageDir;

    /// @brief Hard upper bound (bytes) on one uploaded body. Enforced from
    ///        the `Content-Length` header *before* any body bytes are read
    ///        off the socket, so an oversized upload is rejected before it is
    ///        buffered into memory at all -- not merely before it is written
    ///        to disk. A request with no `Content-Length` header is also
    ///        rejected (see the class doc comment).
    std::int64_t maxBodyBytes = 16 * 1024 * 1024;  // 16 MiB

    /// @brief Address `listen()` binds to. Default loopback-only, matching
    ///        `QtWebSocketServerConfig`'s own default and rationale.
    QHostAddress bindAddress = QHostAddress::LocalHost;
};

/// @brief Minimal hand-rolled HTTP server for attachment blob upload/download.
///
/// Exposes exactly two routes:
///
///  - `POST /attachments` with header `X-Attachment-Content-Type` (recorded
///    and played back on the matching `GET`) plus `Authorization: Bearer
///    <token>`, and the file's raw bytes as the request body. The filename
///    itself is not read by this server at all -- it is metadata Task 16's
///    `AddAttachment{filename, ...}` action records separately, alongside
///    the `storageKey` this server returns; this server only ever stores and
///    serves bytes plus a content type. Returns `200` with a JSON body
///    `{"storageKey": "..."}` on success -- the opaque key `AddAttachment` is
///    then called with to commit the metadata row. The `storageKey` this
///    server mints is 64 hex characters (32 random bytes from
///    `std::random_device`), always well under
///    `kanban::kMaxAttachmentStorageKeyBytes` (255) -- see the class's own
///    `.cpp` for why a random token was chosen over a content hash.
///  - `GET /attachments/{storageKey}` with `Authorization: Bearer <token>`.
///    Returns `200` streaming the stored bytes (with the `Content-Type`
///    recorded at upload time) on success, `404` if `storageKey` does not
///    name a file in the storage directory.
///
/// @par Authentication
/// Every request is authenticated via the *same* `morph::session::TokenVerifier`
/// instance the WebSocket server's authorizer uses (constructed from the same
/// signing secret in `main.cpp` -- see that file's own comment on why there is
/// only ever one `TokenVerifier` per process). A request with no/invalid
/// bearer token is rejected with `401` *before* any request body bytes are
/// read off the socket -- an unauthenticated caller cannot make this server
/// buffer or write anything, upload or download alike.
///
/// @par Size bound
/// `AttachmentServerConfig::maxBodyBytes` is enforced against the `Content-Length`
/// header immediately after headers are parsed and authentication has
/// succeeded, before a single body byte is read from the socket. An oversized
/// upload gets `413` and the connection is closed without ever entering the
/// body-buffering path.
///
/// @par Dangling metadata rows
/// A dangling metadata row -- `AddAttachment` called (Task 16) with a
/// `storageKey` no upload ever actually produced (e.g. the process crashed
/// between finishing the upload response and the client's follow-up
/// `AddAttachment` call, or a caller fabricates one) -- returns `404` on
/// download, rather than being treated as an error state. There is no
/// transactional link between this HTTP server's upload and the metadata
/// action's commit in this pass; reconciling the two is out of scope here.
///
/// @par Authorization (not just authentication)
/// `GET /attachments/{storageKey}` does more than check the bearer token is
/// validly signed and unexpired: it resolves the `storageKey` to its
/// `db::AttachmentRecord` row, follows that row's `task` to the owning
/// `db::TaskRecord`, and requires the verified principal hold at least
/// `Role::Viewer` on that task's `project` -- mirroring
/// `BoardModel::execute(const GetAttachments&)`'s own
/// `requireRole(Role::Viewer)` + `requireTaskBelongsToProject` gate. A
/// validly-signed token for some *other* project's principal is
/// authentication without authorization and must not be enough to read a
/// blob it has no role on. If no `AttachmentRecord` row names `storageKey`
/// at all (the dangling-row / never-uploaded case above), the response is
/// still `404` -- the same status an unauthorized caller gets -- so a probe
/// cannot distinguish "this key doesn't exist" from "this key exists but you
/// have no role on its project."
///
/// `POST /attachments` deliberately does **not** get an equivalent
/// project-scoped check: it mints a brand-new `storageKey` and there is, by
/// construction, no `AttachmentRecord` yet to resolve ownership from (that
/// row is only created afterward by the caller's own follow-up
/// `AddAttachment` call, per this server's designed flow order -- see the
/// "Dangling metadata rows" paragraph above). Any authenticated principal
/// may upload bytes and receive a storage key back; the bytes are, at that
/// point, an orphaned blob with no project association until
/// `AddAttachment` commits it -- `AddAttachment` is the real authorization
/// boundary for *committing* an attachment (`requireRole(Role::Member)` +
/// `requireTaskBelongsToProject`), and `GET` is the real authorization
/// boundary for *reading* a committed one. An uploaded-but-never-committed
/// blob carries no confidentiality value worth gating at upload time: its
/// `storageKey` is known only to the uploader until it is named in an
/// `AddAttachment` call or a `GET`, both of which are already
/// authorization-checked.
///
/// @par Threading
/// A `QObject` living on the Qt event loop thread, same as `QtWebSocketServer`:
/// every slot below runs there. One request is handled per connection; the
/// connection is closed once the response is written.
class AttachmentServer : public QObject {
    Q_OBJECT

  public:
    /// @brief Alias for the configuration struct.
    using Config = AttachmentServerConfig;

    /// @brief Constructs the server. Does not start listening -- call `listen()`.
    /// @param verifier Shared `TokenVerifier` -- the *same instance* the
    ///        WebSocket server's authorizer verifies against (constructed
    ///        from the same signing secret). Not owned; must outlive this
    ///        server.
    /// @param cfg Storage directory, size bound, and bind address.
    /// @param parent Optional Qt parent object.
    explicit AttachmentServer(const ::morph::session::TokenVerifier& verifier, Config cfg, QObject* parent = nullptr);

    /// @brief Closes the listening socket.
    ~AttachmentServer() override;

    AttachmentServer(const AttachmentServer&) = delete;
    AttachmentServer& operator=(const AttachmentServer&) = delete;
    AttachmentServer(AttachmentServer&&) = delete;
    AttachmentServer& operator=(AttachmentServer&&) = delete;

    /// @brief Starts listening on @p port.
    /// @param port TCP port to listen on. Pass 0 to let the OS pick a free port.
    /// @return `true` if the server successfully bound to the requested port.
    [[nodiscard]] bool listen(quint16 port = 0);

    /// @brief The port this server is currently bound to.
    /// @return Bound TCP port, or 0 if not listening.
    [[nodiscard]] quint16 port() const;

    /// @brief Stops accepting new connections and closes the listening socket.
    void close();

  private:
    /// @brief Per-connection accumulation state while a request is being read.
    struct ConnectionState {
        /// @brief Raw bytes received so far (headers, then body).
        QByteArray buffer;
        /// @brief Set once the header block has been fully received and parsed.
        bool headersParsed = false;
        /// @brief Byte offset in `buffer` where the body starts, once known.
        qint64 bodyStart = 0;
        /// @brief Declared body length from `Content-Length`, once known. -1 = not yet known.
        std::int64_t contentLength = -1;
        /// @brief `X-Attachment-Content-Type` recorded once headers are parsed
        ///        for a POST upload, applied once the full body has arrived.
        std::string uploadContentType;
        /// @brief Set once this connection has been responded to and should be ignored.
        bool responded = false;
    };

    Q_SLOT void onNewConnection();
    Q_SLOT void onReadyRead();
    Q_SLOT void onDisconnected();

    /// @brief Handles one request on @p socket once its headers are available
    ///        (and, for a POST upload, once its full body has arrived too).
    void handleRequest(QTcpSocket* socket, ConnectionState& state);

    /// @brief Writes @p state's fully-received upload body to a freshly
    ///        minted storage key and replies `200` with that key.
    void finishUpload(QTcpSocket* socket, ConnectionState& state);

    /// @brief Sends @p response, marks @p state responded, and closes
    ///        @p socket. Every terminal branch in `handleRequest`/
    ///        `finishUpload` ends this way.
    static void respondAndClose(QTcpSocket* socket, ConnectionState& state, const QByteArray& response);

    const ::morph::session::TokenVerifier& _verifier;
    Config _cfg;
    QTcpServer _listener;
    std::unordered_map<QTcpSocket*, ConnectionState> _connections;
};

}  // namespace kanban::http
