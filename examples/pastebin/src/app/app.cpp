// SPDX-License-Identifier: Apache-2.0
#include "pastebin/app/app.hpp"

// examples/common is on every ladder target's include path as a root (see
// examples/common/CMakeLists.txt's target_include_directories), so the
// ladder clock is "clock.hpp" -- the same spelling paste_model.cpp and
// testkit/test_clock.cpp use.
#include "clock.hpp"
#include "pastebin/dto/paste_dto.hpp"
#include "pastebin/models/paste_model.hpp"

#include <morph/core/logger.hpp>

#include <Lightweight/SqlStatement.hpp>

#include <cstdint>
#include <vector>

namespace pastebin::app {

namespace {

/// @brief The current instant, in epoch milliseconds. Mirrors
///        `paste_model.cpp`'s private `nowMs()` helper exactly (same
///        `morph::ladder::now().value` dereference this session's earlier
///        research confirmed against `examples/common/testkit/test_clock.cpp`
///        and `paste_model.cpp`'s own usage) -- duplicated rather than
///        shared because that helper is `paste_model.cpp`'s own anonymous-
///        namespace implementation detail, not part of any public header.
[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

}  // namespace

App::App(std::filesystem::path actionLogPath, std::chrono::milliseconds sweepInterval, std::size_t workers,
         QObject* parent)
    // Initialiser order follows the declaration order in app.hpp, which is
    // itself chosen for teardown safety — see that header's comment.
    : QObject{parent},
      _actionLog{std::make_shared<::morph::journal::FileActionLog>(std::move(actionLogPath))},
      _pool{workers},
      _server{std::make_shared<::morph::backend::RemoteServer>(_pool)},
      _sweepBridge{std::make_unique<::morph::backend::SimulatedRemoteBackend>(*_server)} {
    ::morph::journal::setActionLog(_actionLog);
    connect(&_sweepTimer, &QTimer::timeout, this, &App::sweepExpiredOnce);
    _sweepTimer.start(sweepInterval);
}

App::~App() {
    // Stop first: a tick landing while the members below are being torn down
    // would dispatch a sweep into a half-destroyed App.
    _sweepTimer.stop();
    ::morph::journal::setActionLog(nullptr);
}

void App::sweepExpiredOnce() {
    std::vector<std::string> expiredIds;
    {
        ::Lightweight::SqlStatement stmt;
        stmt.Prepare("SELECT id FROM pastes WHERE expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
        auto cursor = stmt.Execute(nowMs());
        while (cursor.FetchRow()) {
            expiredIds.push_back(cursor.GetColumn<std::string>(1));
        }
    }
    if (expiredIds.empty()) {
        return;
    }

    // `handler` is kept alive by every dispatched call's own completion, not
    // by this function's stack frame. `BridgeHandler::execute()` posts to the
    // worker pool (`SimulatedRemoteBackend::execute()` -> `RemoteServer::handle()`
    // -> `_pool.post(...)`) and returns immediately, so this loop -- and this
    // function -- routinely returns before RemoteServer has so much as looked
    // up the model instance for the *first* dispatched ExpirePaste, let alone
    // run it. A `handler` destroyed synchronously right here (e.g. as a plain
    // local, going out of scope at the end of this function) would deregister
    // its model instance -- via a synchronous `RemoteServer::handleInline`
    // "deregister" call in `~BridgeHandler` -- and race those still-pending
    // dispatches: `RemoteServer::dispatchExecute` would then find the
    // (already-erased) instance missing and reply "model not found" instead of
    // ever running `PasteModel::execute(ExpirePaste)`, silently dropping that
    // sweep pass's reclaim. `RemoteServer`'s own "safe to deregister while an
    // execute is in flight" guarantee (docs/spec/concurrency_and_lifetimes.md)
    // protects an execute already admitted to the model's strand -- not one
    // still sitting in the worker pool's queue, which is exactly the state
    // every one of this loop's dispatches is in immediately after `execute()`
    // returns. Nothing is corrupted or leaked either way -- a dropped pass
    // just means the paste stays expired-but-unreclaimed until the next timer
    // tick tries again (`PasteModel::execute(GetPaste)` already excludes an
    // expired row on its own) -- but every dropped pass is a spurious "expiry
    // sweep: ExpirePaste failed" log line and a wasted round trip. Capturing
    // `handler` in every completion below closes the window: the handler --
    // and the model instance it registered -- is deregistered only once every
    // dispatch issued by this pass has actually settled, whichever of
    // `.then()`/`.onError()` that turns out to be for each one.
    auto handler = std::make_shared<::morph::bridge::BridgeHandler<PasteModel>>(_sweepBridge, &_sweepExecutor);
    // `inFlight` is captured by value, never through `this`: the callbacks
    // below can outlive this App (see sweepInFlight()'s doc comment), and a
    // late one must still be able to decrement the counter safely.
    auto inFlight = _sweepInFlight;
    for (const auto& id : expiredIds) {
        inFlight->fetch_add(1);
        handler->execute(ExpirePaste{.id = PasteId{id}})
            .then([handler, inFlight](Ack) { inFlight->fetch_sub(1); })
            .onError([handler, inFlight, id](const std::exception_ptr&) {
                inFlight->fetch_sub(1);
                ::morph::log::logError("[pastebin::App] expiry sweep: ExpirePaste failed for " + id);
            });
    }
}

}  // namespace pastebin::app
