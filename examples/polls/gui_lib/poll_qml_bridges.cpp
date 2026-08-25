// SPDX-License-Identifier: Apache-2.0
#include "poll_qml_bridges.hpp"
#include "gui/error_text.hpp"
#include "gui/id_qml.hpp"

#include "poll_schemas.hpp"

#include <morph/session/session.hpp>

#include <QString>
#include <QVariant>

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polls::gui {

namespace {

// An `OptionId`/`PollEventId` as the plain number QML rows carry, `kNoId`
// when unengaged. Both are zero-sentinel ids (`core/types.hpp`: the payload is
// a bare `std::int64_t` whose own "not entered" value is `0`), so `id{0}` is
// already the empty state before it reaches the conversion -- see
// `gui/id_qml.hpp`'s "What an id is here".
using ::morph::ladder::gui::idNumber;

/// @brief A `Count` rendered via `morph::units::toString` — an integer text,
///        since `polls::Count` is always a whole number (`units.hpp`).
///
/// `morph::units::toString`, not `std::format("{}", count)`: see
/// `pastebin::gui::readsText`'s identical note (`paste_qml_bridges.cpp`) —
/// Emscripten's bundled libc++ fails to compile the `std::format` call for
/// this `Quantity`-family type outright.
[[nodiscard]] QString countText(const Count& count) {
    return QString::fromStdString(morph::units::toString(count));
}

[[nodiscard]] QString choiceText(VoteChoice choice) {
    switch (choice) {
        case VoteChoice::Yes:
            return QStringLiteral("Yes");
        case VoteChoice::IfNeedBe:
            return QStringLiteral("IfNeedBe");
        case VoteChoice::No:
            return QStringLiteral("No");
        default:
            return QStringLiteral("No");
    }
}

/// @brief Parses one of `VoteView.qml`'s picker strings back into a
///        `VoteChoice`. Anything not `"Yes"`/`"IfNeedBe"` is `No` — the same
///        fail-safe default a missing/garbled radio selection should have,
///        never silently dropping the vote row entirely.
/// @param text One of `"Yes"`/`"IfNeedBe"`/`"No"`.
/// @return The matching `VoteChoice`.
[[nodiscard]] VoteChoice parseChoice(const QString& text) {
    if (text == QStringLiteral("Yes")) {
        return VoteChoice::Yes;
    }
    if (text == QStringLiteral("IfNeedBe")) {
        return VoteChoice::IfNeedBe;
    }
    return VoteChoice::No;
}

/// @brief `votes` (as `submitVotes`/`updateVotes` receive it from QML) into
///        the typed `OneVote` vector both `SubmitVotes`/`UpdateVotes` need.
/// @param votes `{optionId, choice}` maps.
/// @return The decoded vote set, in the same order.
[[nodiscard]] std::vector<OneVote> decodeVotes(const QVariantList& votes) {
    std::vector<OneVote> out;
    out.reserve(static_cast<std::size_t>(votes.size()));
    for (const QVariant& entry : votes) {
        const QVariantMap row = entry.toMap();
        // Deliberately not `OptionId::fromRowId`: this is QML-supplied input,
        // not a stored row id. A missing or non-numeric `optionId` yields 0
        // from toLongLong(), which is exactly the "not entered" state the
        // action's validate() is there to reject -- a clean ValidationError,
        // rather than the exception fromRowId raises for a corrupt *stored*
        // id (morph#215).
        out.push_back(OneVote{.optionId = OptionId{.value = row.value(QStringLiteral("optionId")).toLongLong()},
                              .choice = parseChoice(row.value(QStringLiteral("choice")).toString())});
    }
    return out;
}

[[nodiscard]] QVariantMap toVariantMap(const PollOptionView& option) {
    return QVariantMap{
        {"id", idNumber(option.id)},
        {"label", QString::fromStdString(option.label)},
        {"yesCount", countText(option.yesCount)},
        {"ifNeedBeCount", countText(option.ifNeedBeCount)},
        {"noCount", countText(option.noCount)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const ParticipantVoteView& vote) {
    return QVariantMap{
        {"participantName", QString::fromStdString(vote.participantName)},
        {"optionId", idNumber(vote.optionId)},
        {"choice", choiceText(vote.choice)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const CommentView& comment) {
    return QVariantMap{
        {"participantName", QString::fromStdString(comment.participantName)},
        {"body", QString::fromStdString(comment.body)},
    };
}

template <typename Rows>
[[nodiscard]] QVariantList toVariantList(const Rows& rows) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows.size()));
    for (const auto& row : rows) {
        out.append(toVariantMap(row));
    }
    return out;
}

/// @brief An opaque token newtype (`AdminToken`/`ParticipantToken`) as the
///        plain string a QML row carries — empty when unengaged, the same
///        "empty means absent" convention every other string field in these
///        maps already uses.
template <typename TokenT>
[[nodiscard]] QString tokenText(const TokenT& token) {
    return token.hasValue() ? QString::fromStdString(*token) : QString{};
}

[[nodiscard]] QVariantMap toVariantMap(const CreatePollResult& result) {
    return QVariantMap{
        {"pollId", QString::fromStdString(result.pollId)},
        {"adminToken", tokenText(result.adminToken)},
        {"participantToken", tokenText(result.participantToken)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const GetPollStateResult& state) {
    return QVariantMap{
        {"pollId", QString::fromStdString(state.pollId)},
        {"title", QString::fromStdString(state.title)},
        // Projected to a plain bool for QML, which has no notion of a C++
        // enum class: `Finalized` is the DTO's own two-state type, this map
        // is the GUI-facing view of it.
        {"finalized", state.finalized == Finalized::Yes},
        {"finalizedOptionId", idNumber(state.finalizedOptionId)},
        {"options", toVariantList(state.options)},
        {"votes", toVariantList(state.votes)},
        {"comments", toVariantList(state.comments)},
        {"lastEventId", idNumber(state.lastEventId)},
    };
}

[[nodiscard]] QVariantMap toVariantMap(const PollEvent& event) {
    return QVariantMap{
        {"id", idNumber(event.id)},
        {"kind", QString::fromStdString(event.kind)},
        {"summary", QString::fromStdString(event.summary)},
    };
}


}  // namespace

PollBridge::PollBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent},
      _presenter{bridge, executor},
      _forms{bridge, executor, pollSchemasJson()},
      _bridge{bridge},
      _executor{executor} {
    connect(&_presenter, &PollPresenter::created, this,
            [this](CreatePollResult result) { emit created(toVariantMap(result)); });
    connect(&_presenter, &PollPresenter::failed, this, &PollBridge::failed);

    _refreshDebounce.setSingleShot(true);
    _refreshDebounce.setInterval(0);
    connect(&_refreshDebounce, &QTimer::timeout, this, &PollBridge::refresh);
}

QString PollBridge::schemasJson() const {
    return QString::fromStdString(_forms.schemasJson());
}

void PollBridge::createPoll(const QString& title, const QVariantList& optionLabels) {
    CreatePoll action;
    action.title = title.toStdString();
    action.options.reserve(static_cast<std::size_t>(optionLabels.size()));
    for (const QVariant& label : optionLabels) {
        action.options.push_back(CreatePollOption{.label = label.toString().toStdString()});
    }
    _presenter.createPoll(std::move(action));
}

void PollBridge::openPoll(const QString& pollId) {
    const std::string pollIdStd = pollId.toStdString();
    _forms.openPoll(pollIdStd)
        .then([this, alive = std::weak_ptr<const void>{_liveness}](GetPollStateResult result) {
            if (alive.expired()) {
                return;
            }
            const PollEventId cursor = result.lastEventId;
            emit opened(toVariantMap(result));
            startPolling(cursor);
        })
        .onError([this, alive = std::weak_ptr<const void>{_liveness}](const std::exception_ptr& err) {
            if (alive.expired()) {
                return;
            }
            emit failed(::morph::ladder::gui::errorText(err));
        });
}

void PollBridge::refresh() {
    _forms.getPollState()
        .then([this, alive = std::weak_ptr<const void>{_liveness}](GetPollStateResult result) {
            if (alive.expired()) {
                return;
            }
            emit stateChanged(toVariantMap(result));
        })
        .onError([this, alive = std::weak_ptr<const void>{_liveness}](const std::exception_ptr& err) {
            if (alive.expired()) {
                return;
            }
            emit failed(::morph::ladder::gui::errorText(err));
        });
}

void PollBridge::submitVotes(const QString& participantName, const QVariantList& votes) {
    _forms.submitVotes(SubmitVotes{.participantName = participantName.toStdString(), .votes = decodeVotes(votes)})
        .then([this, alive = std::weak_ptr<const void>{_liveness}](GetPollStateResult result) {
            if (alive.expired()) {
                return;
            }
            emit stateChanged(toVariantMap(result));
        })
        .onError([this, alive = std::weak_ptr<const void>{_liveness}](const std::exception_ptr& err) {
            if (alive.expired()) {
                return;
            }
            emit failed(::morph::ladder::gui::errorText(err));
        });
}

void PollBridge::updateVotes(const QString& participantName, const QVariantList& votes) {
    _forms.updateVotes(UpdateVotes{.participantName = participantName.toStdString(), .votes = decodeVotes(votes)})
        .then([this, alive = std::weak_ptr<const void>{_liveness}](GetPollStateResult result) {
            if (alive.expired()) {
                return;
            }
            emit stateChanged(toVariantMap(result));
        })
        .onError([this, alive = std::weak_ptr<const void>{_liveness}](const std::exception_ptr& err) {
            if (alive.expired()) {
                return;
            }
            emit failed(::morph::ladder::gui::errorText(err));
        });
}

void PollBridge::setAdminToken(const QString& token) {
    ::morph::session::Context session;
    session.token = token.toStdString();
    _bridge.setDefaultSession(session);
}

void PollBridge::submitIfValid(const QString& actionType, const QString& bodyJson) {
    _forms.submitIfValid(
        actionType.toStdString(), bodyJson.toStdString(),
        [this, actionType, alive = std::weak_ptr<const void>{_liveness}](std::string resultJson) {
            if (alive.expired()) {
                return;
            }
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        },
        [this, actionType, alive = std::weak_ptr<const void>{_liveness}](const std::exception_ptr& err) {
            if (alive.expired()) {
                return;
            }
            emit replyReceived(actionType, false, ::morph::ladder::gui::errorText(err));
        });
}

void PollBridge::stopPolling() {
    if (_poller) {
        _poller->stop();
    }
}

void PollBridge::startPolling(PollEventId cursor) {
    // Declaration-order note in poll_qml_bridges.hpp explains why `_poller`
    // may safely outlive individual ticks of `_forms`'s handler but must
    // itself be torn down before `_forms` is.
    _poller = std::make_unique<Poller>(
        _bridge, cursor,
        [this, alive = std::weak_ptr<const void>{_liveness}](PollEventId lastEventId, Poller::OnSuccess onSuccess,
                                                              Poller::OnError onError) {
            if (alive.expired()) {
                return;
            }
            // The production-safe Dispatch shape event_poller.hpp's own doc
            // comment asks for: built directly over one call's own
            // Completion, never over a Presenter's shared failed(QString)
            // signal. PollFormsController::getEventsSince returns a fresh,
            // independent Completion<GetEventsSinceResult> per call — see
            // that method's own doc comment. onSuccess/onError are
            // EventPoller's own callbacks, already guarded on its own
            // _liveness token (see event_poller.hpp) — nothing further to
            // add here beyond not touching `_forms` past this object's own
            // lifetime, which the `alive` check above already covers.
            _forms.getEventsSince(GetEventsSince{.lastEventId = lastEventId})
                .then([lastEventId, onSuccess](GetEventsSinceResult result) {
                    const PollEventId newLastEventId =
                        result.events.empty() ? lastEventId : result.events.back().id;
                    onSuccess(std::move(result.events), newLastEventId);
                })
                .onError([onError](const std::exception_ptr& err) { onError(err); });
        },
        [this, alive = std::weak_ptr<const void>{_liveness}](const PollEvent& event) {
            if (alive.expired()) {
                return;
            }
            onEventApplied(event);
        },
        [this, alive = std::weak_ptr<const void>{_liveness}](const QString& message) {
            if (alive.expired()) {
                return;
            }
            emit pollingStopped(message);
        });
}

void PollBridge::onEventApplied(const PollEvent& event) {
    emit eventReceived(toVariantMap(event));
    // Coalesces a whole tick's worth of events into one refresh() rather
    // than one per event — QTimer::start() on an already-running singleShot
    // timer restarts it, so a burst within the same event-loop turn still
    // fires refresh() exactly once, on the next turn.
    _refreshDebounce.start();
}

}  // namespace polls::gui
