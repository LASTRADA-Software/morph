// The process-wide state that header code reaches through a function. Each
// piece lives here, in the library, so that a shared build has one copy: a
// function-local static in an inline header function is instantiated once per
// module, and the library and an executable would each register into, and read
// from, their own.

#include <morph/core/bridge.hpp>
#include <morph/core/detail/owner_probe.hpp>
#include <morph/core/detail/result_walker.hpp>
#include <morph/core/detail/task_handler.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/observability.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/views.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/table/runtime.hpp>
#include <morph/table/view_cache.hpp>
#include <morph/ui/author.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>

namespace morph::bridge {

ActionExecuteRegistry& ActionExecuteRegistry::instance() {
    static ActionExecuteRegistry inst;
    return inst;
}

}  // namespace morph::bridge

namespace morph::model {

std::atomic<std::size_t>& detail::ActionGate::overlapCounter() noexcept {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

}  // namespace morph::model

namespace morph::model::detail {

ActionResultSlotMap& mutableActionResultSlots() {
    static ActionResultSlotMap entries;
    return entries;
}

std::vector<std::string>& mutableActionResultConflicts() {
    static std::vector<std::string> ids;
    return ids;
}

ModelRegistryFactory& defaultRegistry() {
    static ModelRegistryFactory inst;
    return inst;
}

ActionDispatcher& defaultDispatcher() {
    static ActionDispatcher inst;
    return inst;
}

std::atomic<bool>& registrationPhaseFlag() noexcept {
    static std::atomic<bool> closed{false};
    return closed;
}

}  // namespace morph::model::detail

namespace morph::exec::detail {

IExecutor& inlineExecutor() {
    static InlineExecutor executor;
    return executor;
}

std::atomic<OwnerProbe>& ownerProbe() noexcept {
    static std::atomic<OwnerProbe> probe{nullptr};
    return probe;
}

}  // namespace morph::exec::detail

namespace morph::journal {

PayloadMigrationRegistry& defaultPayloadMigrations() {
    static PayloadMigrationRegistry registry;
    return registry;
}

}  // namespace morph::journal

namespace morph::journal::detail {

bool& tlsIsReplaying() {
    thread_local bool tls = false;
    return tls;
}

std::pair<std::mutex, std::shared_ptr<IActionLog>>& defaultActionLogState() {
    static std::pair<std::mutex, std::shared_ptr<IActionLog>> state;
    return state;
}

}  // namespace morph::journal::detail

namespace morph::log::detail {

LogState& logState() {
    static LogState state;
    return state;
}

}  // namespace morph::log::detail

#ifndef MORPH_OBSERVABILITY_DISABLED
namespace morph::observe::detail {

ObserveState& observeState() {
    static ObserveState state;
    return state;
}

}  // namespace morph::observe::detail
#endif

namespace morph::session {

std::shared_ptr<IAuthorizer> allowAllAuthorizer() {
    static auto const instance = std::make_shared<AllowAllAuthorizer>();
    return instance;
}

const SigningAuthorizer::Verification*& SigningAuthorizer::active() noexcept {
    thread_local const Verification* current = nullptr;
    return current;
}

}  // namespace morph::session

namespace morph::session::detail {

const Context*& tlsCurrent() {
    thread_local const Context* tls = nullptr;
    return tls;
}

const RunOrigin*& tlsOrigin() {
    thread_local const RunOrigin* tls = nullptr;
    return tls;
}

}  // namespace morph::session::detail

namespace morph::table {

TableRuntime const*& detail::tableRuntimeSlot() noexcept {
    thread_local TableRuntime const* tls = nullptr;
    return tls;
}

TableRuntime const* currentTableRuntime() noexcept { return detail::tableRuntimeSlot(); }

std::shared_ptr<ViewCache> makeViewCache(ViewCacheOptions options) {
    return std::make_shared<ViewCache>(std::move(options));
}

ViewCacheStats viewCacheStats(ViewCache const& cache) { return cache.stats(); }

}  // namespace morph::table

namespace morph::effects::detail {

EffectsMap& mutableEffectsRegistry() {
    static EffectsMap entries;
    return entries;
}

std::vector<DeclaredChangeTime>& mutableChangeTimesRegistry() {
    static std::vector<DeclaredChangeTime> entries;
    return entries;
}

AddedEffectsMap& mutableAddedEffectsRegistry() {
    static AddedEffectsMap entries;
    return entries;
}

Resolved const& resolved() {
    model::detail::noteRegistryRead(true);
    struct State {
        std::once_flag once;
        std::optional<Resolved> value;
        std::optional<model::EffectsRegistrationError> error;
    };
    static State state;
    std::call_once(state.once, [] {
        try {
            state.value.emplace(resolve(record::detail::recordRegistry(), model::detail::actionResultSlots(),
                                        mutableEffectsRegistry(), model::detail::actionResultConflicts(),
                                        mutableAddedEffectsRegistry(), mutableChangeTimesRegistry()));
        } catch (model::EffectsRegistrationError const& error) {
            state.error.emplace(error);
        }
    });
    if (state.error) {
        // The error is stored once and rethrown on every call, so it is copied each time.
        // NOLINTNEXTLINE(cert-err09-cpp,cert-err61-cpp,misc-throw-by-value-catch-by-reference,bugprone-exception-copy-constructor-throws,cert-err60-cpp)
        throw model::EffectsRegistrationError{*state.error};
    }
    // call_once either filled the value or recorded the error thrown above.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return state.value.value();
}

}  // namespace morph::effects::detail

namespace morph::record::detail {

std::vector<RecordInfo>& mutableRecordRegistry() {
    static std::vector<RecordInfo> entries;
    return entries;
}

}  // namespace morph::record::detail

namespace morph::time::detail {

std::pair<std::mutex, std::function<DateTime()>>& nowOverrideState() {
    static std::pair<std::mutex, std::function<DateTime()>> state;
    return state;
}

}  // namespace morph::time::detail

namespace morph::math::detail {

std::size_t& wireClampCounter() noexcept {
    static thread_local std::size_t clamped = 0;
    return clamped;
}

}  // namespace morph::math::detail

namespace morph::ui::author {

ScreenRegistry& screenRegistry() noexcept {
    static ScreenRegistry registry;
    return registry;
}

}  // namespace morph::ui::author

namespace morph::views {

ViewRegistry& ViewRegistry::instance() {
    static ViewRegistry inst;
    return inst;
}

}  // namespace morph::views
