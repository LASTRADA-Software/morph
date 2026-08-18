// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "kanban/http/attachment_server.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>

#include <QByteArray>
#include <QTcpSocket>

#include <filesystem>
#include <string>

using kanban::http::AttachmentServer;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::pumpUntil;

namespace {

constexpr std::string_view kSecret = "attachment-server-test-secret-32bytes";

/// @brief See `test_board_model.cpp`'s identical `contextFor`/`ScopedPrincipal`
///        pair for why this is not a designated initializer
///        (`-Wmissing-designated-field-initializers` under this target's
///        strict warnings). Duplicated locally rather than shared, following
///        that file's own precedent (itself following
///        `bookmarks::test_bookmark_model.cpp`).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

[[nodiscard]] kanban::ProjectId createProjectAs(const std::string& principal, const std::string& name) {
    const ScopedPrincipal p{principal};
    kanban::ProjectAdminModel admin;
    return admin.execute(kanban::CreateProject{.name = name}).id;
}

/// @brief A fresh, empty storage directory per test, removed at scope entry
///        and left for inspection (tests remove it themselves at the end).
[[nodiscard]] std::filesystem::path freshStorageDir(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("kanban_attachments_" + name);
    std::filesystem::remove_all(path);
    return path;
}

/// @brief Builds a signed, unexpired bearer token for @p principal.
[[nodiscard]] std::string validToken(const morph::session::TokenIssuer& issuer, std::string principal) {
    return issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
}

/// @brief Connects to 127.0.0.1:@p port, writes @p request, and waits for at
///        least one byte of response (or the deadline). Returns everything
///        read back within the deadline (a single reply may need more than
///        one `readyRead` if the body is large -- callers that need the full
///        response for a large body should keep pumping using the returned
///        socket state; every test here reads well under a TCP segment).
[[nodiscard]] QByteArray sendRawRequest(quint16 port, const QByteArray& request,
                                        std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!pumpUntil([&] { return socket.state() == QAbstractSocket::ConnectedState; }, deadline)) {
        return {};
    }
    socket.write(request);
    // Wait for the connection to close (this server closes after replying),
    // which is also how the test knows the full response has arrived.
    static_cast<void>(pumpUntil([&] { return socket.state() == QAbstractSocket::UnconnectedState; }, deadline));
    return socket.readAll();
}

}  // namespace

TEST_CASE("AttachmentServer accepts a valid upload and returns a storageKey; bytes land on disk", "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("valid_upload");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    const std::string token = validToken(issuer, "alice");
    const std::string body = "hello attachment bytes";
    const QByteArray request = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "X-Attachment-Filename: hello.txt\r\n"
        "X-Attachment-Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);

    const QByteArray response = sendRawRequest(server.port(), request);
    const std::string responseText = response.toStdString();

    REQUIRE(responseText.starts_with("HTTP/1.1 200"));
    CHECK(responseText.find("storageKey") != std::string::npos);

    // Extract the storageKey (a bare-bones JSON scrape -- good enough for a test).
    const auto keyPos = responseText.find("\"storageKey\"");
    REQUIRE(keyPos != std::string::npos);
    const auto colonPos = responseText.find(':', keyPos);
    const auto firstQuote = responseText.find('"', colonPos);
    const auto secondQuote = responseText.find('"', firstQuote + 1);
    const std::string storageKey = responseText.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    REQUIRE_FALSE(storageKey.empty());

    const auto blobPath = storageDir / storageKey;
    REQUIRE(std::filesystem::exists(blobPath));
    CHECK(std::filesystem::file_size(blobPath) == body.size());

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer downloads an existing storageKey's bytes with the recorded content type "
          "for a principal with Viewer-or-above role on the owning project",
          "[kanban][attachments][http]") {
    // Positive control for the ownership-authorization gate below: uploading
    // bytes alone is not enough to authorize a GET any more -- the storageKey
    // must actually be committed to a task (via AddAttachment, exactly like a
    // real client would) on a project the requesting principal has a role
    // on. This supersedes what used to be a bare upload-then-GET with no
    // AttachmentRecord at all (see the Critical-finding fix report).
    DbFixture fixture;
    const auto storageDir = freshStorageDir("download_existing");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    const auto projectId = createProjectAs("alice", "Download Existing Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto columnId = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;

    const std::string token = validToken(issuer, "alice");
    const std::string body = "downloadable payload bytes";

    const QByteArray uploadRequest = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "X-Attachment-Filename: dl.txt\r\n"
        "X-Attachment-Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);
    const std::string uploadResponse = sendRawRequest(server.port(), uploadRequest).toStdString();
    REQUIRE(uploadResponse.starts_with("HTTP/1.1 200"));
    const auto keyPos = uploadResponse.find("\"storageKey\"");
    const auto colonPos = uploadResponse.find(':', keyPos);
    const auto firstQuote = uploadResponse.find('"', colonPos);
    const auto secondQuote = uploadResponse.find('"', firstQuote + 1);
    const std::string storageKey = uploadResponse.substr(firstQuote + 1, secondQuote - firstQuote - 1);

    // Commit the metadata row, exactly as a real client's follow-up
    // AddAttachment call would -- this is what makes the storageKey resolve
    // to a project the GET-time authorization check can find.
    model.execute(kanban::AddAttachment{
        .taskId = taskId, .filename = "dl.txt", .contentType = "text/plain", .sizeBytes = static_cast<std::int64_t>(body.size()),
        .storageKey = storageKey});

    const QByteArray getRequest = QByteArray::fromStdString(
        "GET /attachments/" + storageKey + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "\r\n");
    const std::string getResponse = sendRawRequest(server.port(), getRequest).toStdString();

    REQUIRE(getResponse.starts_with("HTTP/1.1 200"));
    CHECK(getResponse.find("Content-Type: text/plain") != std::string::npos);
    CHECK(getResponse.ends_with(body));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer returns 404 (not 200) for a GET whose bearer token is validly signed for a "
          "DIFFERENT project the principal has no role on -- authentication alone is not authorization",
          "[kanban][attachments][http]") {
    // The Critical-finding regression test: `mallory` holds a validly-signed
    // token (real signature, unexpired) but has NO role on the project that
    // owns this attachment -- she is authenticated, not authorized. Before
    // the fix, this GET would return 200 with the bytes (the server checked
    // only that *some* bearer token verified, never whose project it
    // belonged to). 404, not 403, matching the existing dangling-row
    // precedent: an unauthorized caller must not be able to distinguish "this
    // key doesn't exist" from "this key exists but you have no access."
    DbFixture fixture;
    const auto storageDir = freshStorageDir("cross_tenant_get");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    // alice's project owns the attachment.
    const auto aliceProjectId = createProjectAs("alice", "Alice's Board");
    kanban::BoardModel aliceModel;
    kanban::TaskId aliceTaskId;
    {
        const ScopedPrincipal alice{"alice"};
        aliceModel.execute(kanban::OpenBoard{.projectId = aliceProjectId});
        const auto columnId = aliceModel.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
        const auto swimlaneId = aliceModel.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
        aliceTaskId =
            aliceModel.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Secret task"})
                .tasks.front()
                .id;
    }

    // mallory has her own, entirely separate project -- she is a real,
    // authenticated principal, just not one with any standing on alice's
    // project.
    const auto malloryProjectId = createProjectAs("mallory", "Mallory's Own Board");
    static_cast<void>(malloryProjectId);

    const std::string aliceToken = validToken(issuer, "alice");
    const std::string body = "secret attachment bytes only alice's project should see";
    const QByteArray uploadRequest = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + aliceToken + "\r\n"
        "X-Attachment-Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);
    const std::string uploadResponse = sendRawRequest(server.port(), uploadRequest).toStdString();
    REQUIRE(uploadResponse.starts_with("HTTP/1.1 200"));
    const auto keyPos = uploadResponse.find("\"storageKey\"");
    const auto colonPos = uploadResponse.find(':', keyPos);
    const auto firstQuote = uploadResponse.find('"', colonPos);
    const auto secondQuote = uploadResponse.find('"', firstQuote + 1);
    const std::string storageKey = uploadResponse.substr(firstQuote + 1, secondQuote - firstQuote - 1);

    {
        const ScopedPrincipal alice{"alice"};
        aliceModel.execute(kanban::AddAttachment{.taskId = aliceTaskId,
                                                  .filename = "secret.txt",
                                                  .contentType = "text/plain",
                                                  .sizeBytes = static_cast<std::int64_t>(body.size()),
                                                  .storageKey = storageKey});
    }

    // mallory presents her own validly-signed token (real signature, real
    // principal, unexpired) against alice's storageKey.
    const std::string malloryToken = validToken(issuer, "mallory");
    const QByteArray getRequest = QByteArray::fromStdString(
        "GET /attachments/" + storageKey + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + malloryToken + "\r\n"
        "\r\n");
    const std::string getResponse = sendRawRequest(server.port(), getRequest).toStdString();

    REQUIRE(getResponse.starts_with("HTTP/1.1 404"));
    CHECK(getResponse.find(body) == std::string::npos);

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer returns 404 for a GET naming a storageKey that was never uploaded",
          "[kanban][attachments][http]") {
    // DbFixture: the GET-time authorization check (fix for the Critical
    // finding) queries AttachmentRecord/TaskRecord/ProjectRoleRecord for
    // every GET, even one whose storageKey never existed at all -- a real DB
    // connection is now needed here, where none was before that fix.
    DbFixture fixture;
    const auto storageDir = freshStorageDir("download_missing");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    const std::string token = validToken(issuer, "alice");
    const std::string fakeKey(64, 'a');  // well-formed shape, never actually uploaded
    const QByteArray getRequest = QByteArray::fromStdString(
        "GET /attachments/" + fakeKey + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "\r\n");
    const std::string getResponse = sendRawRequest(server.port(), getRequest).toStdString();

    REQUIRE(getResponse.starts_with("HTTP/1.1 404"));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer's parser does not crash or hang on malformed/garbage request bytes",
          "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("garbage_input");
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    // Every input below is a documented DEFINED outcome only in the sense
    // that the server must not crash and must not hang -- unlike the
    // wire_decode fuzz harness, there is no assertion that a specific status
    // code comes back for each: the point is robustness of the parser itself.
    const std::vector<QByteArray> garbageInputs = {
        QByteArray{"\x00\x01\x02\x03\xff\xfe\xfd\xfc", 8},
        QByteArray{"not even close to an http request"},
        QByteArray{"GET"},                                     // no path/version at all
        QByteArray{"GET /attachments/x"},                      // no version, no headers, no terminator
        QByteArray{"POST /attachments HTTP/1.1\r\n"},           // headers never terminate
        QByteArray{"POST /attachments HTTP/1.1\r\nContent-Length: notanumber\r\n\r\n"},
        QByteArray{"POST /attachments HTTP/1.1\r\nContent-Length: -5\r\n\r\n"},
        QByteArray{"\r\n\r\n"},                                 // headers end with nothing before it
        QByteArray(20000, 'a'),                                 // header block far past the 16KiB guard, no terminator
    };

    for (const auto& garbage : garbageInputs) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        REQUIRE(pumpUntil([&] { return socket.state() == QAbstractSocket::ConnectedState; }));
        socket.write(garbage);
        // No crash and no hang: either the server responds and closes, or
        // (for an input with no header terminator at all and under the size
        // guard) it simply keeps waiting -- this test bounds *its own* wait,
        // it does not require the server to ever respond to an incomplete
        // request.
        static_cast<void>(pumpUntil([&] { return socket.state() == QAbstractSocket::UnconnectedState; },
                                     std::chrono::milliseconds{500}));
        socket.abort();
    }

    // The server is still alive and functions normally after all of that.
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const std::string token = validToken(issuer, "alice");
    const std::string body = "still working";
    const QByteArray request = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);
    const std::string response = sendRawRequest(server.port(), request).toStdString();
    CHECK(response.starts_with("HTTP/1.1 200"));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer rejects an oversized upload with 413 before writing anything to disk", "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("oversized_upload");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    // A deliberately tiny configured bound -- not a real multi-GB payload --
    // so the test stays fast while still exercising the real rejection path.
    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir, .maxBodyBytes = 8}};
    REQUIRE(server.listen());

    const std::string token = validToken(issuer, "alice");
    const std::string body = "this body is far larger than the configured 8-byte bound";
    const QByteArray request = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "X-Attachment-Filename: big.bin\r\n"
        "X-Attachment-Content-Type: application/octet-stream\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);

    const QByteArray response = sendRawRequest(server.port(), request);
    const std::string responseText = response.toStdString();

    REQUIRE(responseText.starts_with("HTTP/1.1 413"));
    CHECK(std::filesystem::is_empty(storageDir));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("A dangling metadata row -- AddAttachment committed for a storageKey no upload ever produced -- "
          "downloads as a clean 404, not a crash",
          "[kanban][attachments][http]") {
    DbFixture fixture;
    const auto storageDir = freshStorageDir("dangling_row");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    // Set up a task to attach metadata to, exactly as Task 16's own
    // AddAttachment tests do (test_board_model.cpp).
    const auto projectId = createProjectAs("alice", "Dangling Row Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto columnId = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId =
        model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "Fix bug"})
            .tasks.front()
            .id;

    // The scenario: metadata is committed for a storageKey this
    // AttachmentServer's storage directory has never seen -- the upload
    // "died after metadata commit" (or never happened at all). This
    // AddAttachment call itself never touches the HTTP server; it exercises
    // the same BoardModel action Task 16 added.
    const std::string danglingKey(64, 'd');
    model.execute(kanban::AddAttachment{.taskId = taskId,
                                         .filename = "ghost.pdf",
                                         .contentType = "application/pdf",
                                         .sizeBytes = 4096,
                                         .storageKey = danglingKey});
    const auto attachments = model.execute(kanban::GetAttachments{.taskId = taskId});
    REQUIRE(attachments.attachments.size() == 1);
    CHECK(attachments.attachments.front().storageKey == danglingKey);

    // Downloading that exact storageKey from the real HTTP server must
    // return a clean 404 -- not a crash, not a hang, not a 200 with garbage
    // bytes.
    const std::string token = validToken(issuer, "alice");
    const QByteArray getRequest = QByteArray::fromStdString(
        "GET /attachments/" + danglingKey + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "\r\n");
    const std::string getResponse = sendRawRequest(server.port(), getRequest).toStdString();
    REQUIRE(getResponse.starts_with("HTTP/1.1 404"));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer rejects an upload with no bearer token before writing anything to disk",
          "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("no_token_upload");
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    const std::string body = "bytes that must never be written";
    const QByteArray request = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Attachment-Filename: sneaky.txt\r\n"
        "X-Attachment-Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);

    const QByteArray response = sendRawRequest(server.port(), request);
    const std::string responseText = response.toStdString();

    REQUIRE(responseText.starts_with("HTTP/1.1 401"));
    CHECK(std::filesystem::is_empty(storageDir));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer rejects an upload carrying a bearer token with a bad signature",
          "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("bad_token_upload");
    const morph::session::TokenIssuer wrongIssuer{"a-completely-different-signing-secret", morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    const std::string forgedToken = validToken(wrongIssuer, "mallory");
    const std::string body = "bytes that must never be written";
    const QByteArray request = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + forgedToken + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);

    const QByteArray response = sendRawRequest(server.port(), request);
    const std::string responseText = response.toStdString();

    REQUIRE(responseText.starts_with("HTTP/1.1 401"));
    CHECK(std::filesystem::is_empty(storageDir));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer rejects a GET download with no bearer token", "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("no_token_download");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir}};
    REQUIRE(server.listen());

    // Upload something real first (authenticated), so the unauthenticated GET
    // below is denied for lack of auth, not because the key genuinely doesn't
    // exist.
    const std::string token = validToken(issuer, "alice");
    const std::string body = "protected bytes";
    const QByteArray uploadRequest = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body);
    const std::string uploadResponse = sendRawRequest(server.port(), uploadRequest).toStdString();
    REQUIRE(uploadResponse.starts_with("HTTP/1.1 200"));
    const auto keyPos = uploadResponse.find("\"storageKey\"");
    const auto colonPos = uploadResponse.find(':', keyPos);
    const auto firstQuote = uploadResponse.find('"', colonPos);
    const auto secondQuote = uploadResponse.find('"', firstQuote + 1);
    const std::string storageKey = uploadResponse.substr(firstQuote + 1, secondQuote - firstQuote - 1);

    const QByteArray getRequest = QByteArray::fromStdString(
        "GET /attachments/" + storageKey + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n");
    const std::string getResponse = sendRawRequest(server.port(), getRequest).toStdString();
    REQUIRE(getResponse.starts_with("HTTP/1.1 401"));

    std::filesystem::remove_all(storageDir);
}

TEST_CASE("AttachmentServer rejects a stream that keeps sending bytes past the size bound even though "
          "its own Content-Length header understated the body",
          "[kanban][attachments][http]") {
    const auto storageDir = freshStorageDir("dishonest_content_length");
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    const morph::session::TokenVerifier verifier{std::string{kSecret}, morph::session::hmacSha256};

    AttachmentServer server{verifier, AttachmentServer::Config{.storageDir = storageDir, .maxBodyBytes = 8}};
    REQUIRE(server.listen());

    const std::string token = validToken(issuer, "alice");
    // Content-Length lies (claims 4, well under the 8-byte bound) but the
    // socket then keeps streaming far more than that on the same connection.
    // The 413 must still fire, from the running-total check against actual
    // bytes received, not merely from the (understated) declared length.
    const QByteArray headers = QByteArray::fromStdString(
        "POST /attachments HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "Content-Length: 4\r\n"
        "\r\n");
    const std::string overflow(4096, 'x');

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, server.port());
    REQUIRE(pumpUntil([&] { return socket.state() == QAbstractSocket::ConnectedState; }));
    socket.write(headers);
    socket.write(QByteArray::fromStdString(overflow));
    REQUIRE(pumpUntil([&] { return socket.state() == QAbstractSocket::UnconnectedState; }));
    const std::string response = socket.readAll().toStdString();

    REQUIRE(response.starts_with("HTTP/1.1 413"));
    CHECK(std::filesystem::is_empty(storageDir));

    std::filesystem::remove_all(storageDir);
}
