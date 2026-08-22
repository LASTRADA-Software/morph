// SPDX-License-Identifier: Apache-2.0
//
// Headless kanban client, spawned as a separate process by
// `testkit/process_pool.hpp`.
//
// Drives `BoardPresenter` -- the presenter, not a raw `BridgeHandler` --
// because that is the layer a real GUI client actually uses, and the layer
// where a lost connection has to be survivable (examples/TESTING.md).
//
// Usage: ladder_kanban_headless --url <ws-url> [options]
//   --url <ws>       server to connect to (required)
//   --project <id>   project whose board to open (required)
//   --token <tok>    session token; the server's authorizer rejects us without one
//   --principal <p>  principal name to send alongside the token
//   --comments <n>   add n comments to the board's first task, then exit 0
//   --hold           attach, report readiness on stdout, then block forever --
//                    the mode the crash test uses, since the parent needs the
//                    client to be *attached* before it kills it
//
// Exit codes name the step that failed rather than a bare 1, so a failure in
// a spawned process is diagnosable from the parent's assertion message alone.
//   10 connect failed   11 openBoard failed   12 openBoard timed out
//   13 no task to comment on                  14 comment failed/timed out
//    2 bad arguments

#include <kanban/dto/board_dto.hpp>
#include "board_presenter.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QString>
#include <QUrl>

#include <morph/core/bridge.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/session/session.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

/// @brief Spins the client's own event loop until @p done or the deadline.
bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

struct Options {
    QString url;
    std::string token;
    std::string principal{"alice"};
    std::int64_t projectId = 0;
    int comments = 0;
    bool hold = false;
};

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};

    Options opts;
    for (int idx = 1; idx < argc; ++idx) {
        const std::string arg{argv[idx]};
        const auto next = [&]() -> std::string { return idx + 1 < argc ? argv[++idx] : std::string{}; };
        if (arg == "--url") {
            opts.url = QString::fromStdString(next());
        } else if (arg == "--token") {
            opts.token = next();
        } else if (arg == "--principal") {
            opts.principal = next();
        } else if (arg == "--project") {
            opts.projectId = std::stoll(next());
        } else if (arg == "--comments") {
            opts.comments = std::stoi(next());
        } else if (arg == "--hold") {
            opts.hold = true;
        }
    }
    if (opts.url.isEmpty() || opts.projectId == 0) {
        std::cerr << "usage: ladder_kanban_headless --url <ws> --project <id> [--token t] "
                     "[--comments n] [--hold]\n";
        return 2;
    }

    auto backend = std::make_unique<morph::qt::QtWebSocketBackend>(
        QUrl{opts.url}, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(),
        std::nullopt);
    if (!backend->waitForConnected(5000)) {
        std::cerr << "headless: connect failed\n";
        return 10;
    }

    morph::qt::QtExecutor executor;
    morph::bridge::Bridge bridge{std::move(backend)};

    // The server installs a KanbanAuthorizer, so every envelope must carry a
    // signed session. Set once on the Bridge rather than per call: Bridge
    // stamps `call.session` from its default for every execute.
    morph::session::Context session;
    session.principal = opts.principal;
    session.token = opts.token;
    bridge.setDefaultSession(session);

    kanban::gui::BoardPresenter presenter{bridge, &executor};

    bool opened = false;
    bool failed = false;
    std::string failure;
    kanban::GetBoardResult board;
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::boardOpened,
                     [&](const kanban::GetBoardResult& result) {
                         board = result;
                         opened = true;
                     });
    QObject::connect(&presenter, &kanban::gui::BoardPresenter::failed, [&](const QString& message) {
        failure = message.toStdString();
        failed = true;
    });

    presenter.openBoard(kanban::ProjectId{opts.projectId});
    if (!pumpUntil([&] { return opened || failed; }, std::chrono::seconds{10})) {
        std::cerr << "headless: openBoard timed out\n";
        return 12;
    }
    if (failed) {
        std::cerr << "headless: openBoard failed: " << failure << "\n";
        return 11;
    }

    if (opts.hold) {
        // Attached, and staying attached. The parent waits for this line
        // before killing us, so that the kill lands on a client the server
        // has a live connection scope for -- killing before the attach would
        // test nothing.
        std::cout << "ATTACHED\n";
        std::cout.flush();
        for (;;) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    for (int done = 0; done < opts.comments; ++done) {
        if (board.tasks.empty()) {
            std::cerr << "headless: --comments given but the board has no task\n";
            return 13;
        }
        bool acked = false;
        failed = false;
        auto conn = QObject::connect(&presenter, &kanban::gui::BoardPresenter::commentAdded,
                                     [&](const QString&) { acked = true; });
        presenter.addComment(board.tasks.front().id, QStringLiteral("from a separate process"));
        const bool settled = pumpUntil([&] { return acked || failed; }, std::chrono::seconds{10});
        QObject::disconnect(conn);
        if (!settled || failed) {
            std::cerr << "headless: addComment failed: " << (failed ? failure : "timed out") << "\n";
            return 14;
        }
    }

    return 0;
}
