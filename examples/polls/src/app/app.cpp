// SPDX-License-Identifier: Apache-2.0
#include "polls/app/app.hpp"

// This rung registers exactly one model type. `BRIDGE_REGISTER_MODEL`/
// `BRIDGE_REGISTER_ACTION` (poll_model.hpp) place their registrars in the
// *header*, so a translation unit that includes it both registers
// "PollModel" with the process-wide registry/dispatcher and emits a
// reference to `PollModel::execute`'s bodies -- which is what pulls
// PollModel's object file out of a static library for a binary (a server
// `main()`) whose own code names nothing but `App`. Without this include,
// such a binary would either fail to link or come up serving no models at
// all -- identical rationale to `bookmarks::app::App`'s own model includes
// (`examples/bookmarks/src/app/app.cpp`), just for one model instead of
// four.
#include "polls/models/poll_model.hpp"

#include <utility>

namespace polls::app {

namespace {

/// @brief Live-instance cap this server installs.
///
/// This rung's `authorizeRegister` is unconditionally permissive by design
/// (the framework can now gate registration on identity -- register/attach
/// envelopes carry the caller's session -- but polls' attach-by-id model
/// deliberately doesn't use it), so an unauthenticated client *can* make the
/// server create model instances even though `PollModel::execute()`'s own
/// admin/participant checks still gate every state-changing call on them --
/// `auth::PollsAuthorizer` leaves both `authorize()` and its two
/// instance-lifecycle hooks permissive by design (see that file's own
/// `@file` comment). `maxLiveModels` is the framework's own answer to that
/// shape of churn: past the cap a
/// `register`/keyed-attach is answered `err "too many models"` and no
/// instance is constructed.
///
/// This rung registers exactly one model type, `PollModel`, shared/keyed by
/// `pollId` (`BRIDGE_MODEL_KEY`, `poll_model.hpp`) -- unlike bookmarks'
/// per-client-owned instances, one live `PollModel` instance is shared by
/// every participant currently viewing that poll, so the relevant count
/// here is concurrent *polls with at least one attached viewer*, not
/// concurrent clients. `256` is generous relative to that: it matches
/// rung 2's own cap (`bookmarks::app::kMaxLiveModels`,
/// `examples/bookmarks/src/app/app.cpp`) chosen for a comparable
/// single-server-instance shape, and is far beyond the concurrency this
/// rung's own harness (a handful of simulated participants converging on
/// one shared poll, `examples/polls/README.md`) ever exercises at once.
constexpr std::size_t kMaxLiveModels = 256;

}  // namespace

App::App(std::filesystem::path actionLogPath, std::size_t workers)
    : _actionLog{std::make_shared<::morph::journal::FileActionLog>(std::move(actionLogPath))},
      _pool{workers},
      _server{std::make_shared<::morph::backend::RemoteServer>(_pool, std::make_shared<auth::PollsAuthorizer>())} {
    ::morph::journal::setActionLog(_actionLog);

    ::morph::backend::LimitPolicy limits;
    limits.maxLiveModels = kMaxLiveModels;
    _server->setLimitPolicy(limits);
}

App::~App() {
    // Matches setActionLog's own clear-on-destruction discipline
    // (`bookmarks::app::App`/`pastebin::app::App`'s identical `~App`): a
    // later test (or a second App in the same process) must see the action
    // log cleared rather than a previous App's still-live instance.
    ::morph::journal::setActionLog(nullptr);
}

}  // namespace polls::app
