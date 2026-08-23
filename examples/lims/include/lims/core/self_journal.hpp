// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

/// @file
/// Self-journaling for a plain-constructed model.
///
/// morph appends a `LogEntry` automatically only on the registry/dispatcher
/// path (`morph::model::detail::IModelHolder::recordIfAttached`). A model
/// constructed directly — which is how every ladder unit test constructs one,
/// and how `examples/IMPLEMENTATION.md` rule 5 requires models to be tested —
/// never goes through a holder, so nothing appends for it. Every rung that
/// wanted a journal has therefore hand-rolled the same
/// `attachActionLog`/`logAction` pair inside the model
/// (`ledger::LedgerModel`, `ledger::BudgetModel`, `ledger::RuleModel`,
/// `kanban::BoardModel`, `lims::AnalysisCatalogModel`). This header is *not* a
/// sixth copy of that shape: it is the one place this rung's two models share
/// it, so `SampleModel` adds no second copy of its own.
///
/// It also records **failures**, which none of the hand-rolled copies do. A
/// 21 CFR Part 11-style audit trail has to show the rejected attempt — "who
/// tried to publish an unverified sample, and when" is exactly the question
/// such a trail exists to answer — so a rejected transition appends an entry
/// with `Outcome::Failed` and the exception text rather than vanishing.

namespace lims {

/// @brief A model's own durable action log plus the identity to stamp on
///        every entry it appends.
///
/// Held by value as a model member. Inert until `attach()` is called: a model
/// with no log attached records nothing and costs nothing.
class SelfJournal {
public:
    /// @brief Attaches @p log and stamps @p entityKey onto every later entry.
    /// @param log Sink entries are forwarded to. May be null, which disables
    ///        recording again.
    /// @param entityKey Stable identity of the model instance (for this rung,
    ///        the sample id, or the empty string for the lab-wide catalogue).
    void attach(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
        _log = std::move(log);
        _entityKey = std::move(entityKey);
    }

    /// @brief Re-points the attached log at a different entity.
    ///
    /// A keyed model instance learns its identity when it attaches to an
    /// entity, which can happen *after* a log was handed to it.
    /// @param entityKey The new stable identity.
    void rekey(std::string entityKey) { _entityKey = std::move(entityKey); }

    /// @brief Whether a log is attached (i.e. whether recording does anything).
    /// @return `true` when a non-null sink was attached.
    [[nodiscard]] bool attached() const noexcept { return static_cast<bool>(_log); }

    /// @brief Appends a successful execution of @p action.
    /// @tparam Model Model type the action ran against; supplies `modelType`.
    /// @tparam Action Concrete action type.
    /// @tparam Result The action's result type.
    /// @param action The executed action.
    /// @param result Its result.
    /// @param timestampMs Wall-clock instant of execution, epoch milliseconds.
    template <typename Model, typename Action, typename Result>
    void recordSuccess(const Action& action, const Result& result, std::int64_t timestampMs) const {
        if (!_log) {
            return;
        }
        auto entry = makeEntry<Model, Action>(action, timestampMs);
        entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
        entry.outcome = ::morph::journal::Outcome::Succeeded;
        append(std::move(entry));
    }

    /// @brief Appends a rejected execution of @p action.
    ///
    /// The entry carries no `result` (there was none) and the thrown
    /// exception's text in `error`, matching `LogEntry`'s own documented
    /// success/failure shape.
    /// @tparam Model Model type the action ran against; supplies `modelType`.
    /// @tparam Action Concrete action type.
    /// @param action The rejected action.
    /// @param error The rejecting exception's `what()`.
    /// @param timestampMs Wall-clock instant of the attempt, epoch milliseconds.
    template <typename Model, typename Action>
    void recordFailure(const Action& action, const std::string& error, std::int64_t timestampMs) const {
        if (!_log) {
            return;
        }
        auto entry = makeEntry<Model, Action>(action, timestampMs);
        entry.error = error;
        entry.outcome = ::morph::journal::Outcome::Failed;
        append(std::move(entry));
    }

private:
    /// @brief Fills the fields both outcomes share.
    /// @tparam Model Model type the action ran against.
    /// @tparam Action Concrete action type.
    /// @param action The action to encode into the entry's payload.
    /// @param timestampMs Wall-clock instant, epoch milliseconds.
    /// @return The partially-filled entry.
    template <typename Model, typename Action>
    [[nodiscard]] ::morph::journal::LogEntry makeEntry(const Action& action, std::int64_t timestampMs) const {
        ::morph::journal::LogEntry entry;
        entry.modelType = std::string{::morph::model::ModelTraits<Model>::typeId()};
        entry.entityKey = _entityKey;
        entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
        entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
        if (const auto* ctx = ::morph::session::current()) {
            entry.principal = ctx->principal;
        }
        entry.timestampMs = timestampMs;
        return entry;
    }

    /// @brief Appends @p entry and flushes, so a crash cannot lose the tail of
    ///        an audit trail whose whole purpose is to survive one.
    /// @param entry The entry to append.
    void append(::morph::journal::LogEntry entry) const {
        _log->append(std::move(entry));
        _log->flush();
    }

    /// @brief Stable identity stamped onto every entry; empty when unset.
    std::string _entityKey;

    /// @brief The attached sink, or null when this journal is inert.
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace lims
