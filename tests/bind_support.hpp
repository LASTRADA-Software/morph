// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace morph::testing {

/// @brief Binds through @p backend's `bindModel` and waits for the id.
///
/// What a test that drives a backend directly writes instead of the removed
/// synchronous acquire verbs: the request's shape selects private,
/// register-or-attach, or re-point (see `backend::detail::BindRequest`).
/// @param backend The backend.
/// @param request The bind request.
/// @return The bound id; the bind's failure is rethrown.
inline ::morph::exec::detail::ModelId bindNow(::morph::backend::detail::IBackend& backend,
                                              ::morph::backend::detail::BindRequest request) {
    return awaitAnswer([&backend, &request](::morph::exec::IExecutor& reply) {
        return backend.bindModel(std::move(request), reply);
    });
}

/// @brief Register-or-attach the shared instance for (@p typeId, @p primary).
/// @param backend    The backend.
/// @param typeId     Model type id.
/// @param factory    Factory for a fresh instance.
/// @param primary    Canonical primary key.
/// @param contextKey Entity key; defaults to none.
/// @return The bound id.
inline ::morph::exec::detail::ModelId bindShared(
    ::morph::backend::detail::IBackend& backend, std::string typeId,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory, std::string primary,
    std::string contextKey = {}) {
    return bindNow(backend, ::morph::backend::detail::BindRequest{.typeId = std::move(typeId),
                                                                  .factory = std::move(factory),
                                                                  .contextKey = std::move(contextKey),
                                                                  .primary = std::move(primary),
                                                                  .current = {}});
}

/// @brief Re-point from @p current to the shared instance for (@p typeId, @p primary).
/// @param backend    The backend.
/// @param typeId     Model type id.
/// @param factory    Factory for a fresh instance.
/// @param primary    Canonical primary key.
/// @param current    The instance currently held.
/// @param contextKey Entity key; defaults to none.
/// @return The bound id.
inline ::morph::exec::detail::ModelId bindAttach(
    ::morph::backend::detail::IBackend& backend, std::string typeId,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory, std::string primary,
    ::morph::exec::detail::ModelId current, std::string contextKey = {}) {
    return bindNow(backend, ::morph::backend::detail::BindRequest{.typeId = std::move(typeId),
                                                                  .factory = std::move(factory),
                                                                  .contextKey = std::move(contextKey),
                                                                  .primary = std::move(primary),
                                                                  .current = current});
}

/// What a hand-settled backend does with its next bind or promote.
enum class GateReply : std::uint8_t {
    Hold,    ///< keep the reply; the test settles it
    Inline,  ///< settle before returning, as `LocalBackend` does
    Throw,   ///< throw from the verb itself
    Empty,   ///< return a completion with no state, a backend that forgot to build one
};

/// A `LocalBackend` whose bind and promote replies are held until the test
/// settles them, and whose dispatch can be made to throw.
class GateBackend : public ::morph::backend::LocalBackend {
public:
    explicit GateBackend(::morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(::morph::backend::detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override {
        if (bindMode == GateReply::Throw) {
            throw std::runtime_error{"bind threw"};
        }
        if (bindMode == GateReply::Inline) {
            return LocalBackend::bindModel(std::move(request), cbExec);
        }
        if (bindMode == GateReply::Empty) {
            return {};
        }
        auto [completion, promise] =
            ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);
        // The instance exists now; only the reply is held.
        auto local = LocalBackend::bindModel(std::move(request), _settledOn);
        _binds->push_back(Held{.id = ::morph::bridge::detail::takeSettled(local)->id, .promise = std::move(promise)});
        return std::move(completion);
    }

    ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(
        ::morph::backend::detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) override {
        if (promoteMode == GateReply::Inline) {
            return LocalBackend::promoteModel(std::move(request), cbExec);
        }
        auto [completion, promise] =
            ::morph::async::Completion<::morph::exec::detail::ModelId>::makeSettleable(&cbExec);
        _promotes.push_back(Held{.id = request.mid, .promise = std::move(promise)});
        return std::move(completion);
    }

    void executeInto(::morph::exec::detail::ModelId mid, ::morph::backend::detail::ActionCall call,
                     ::morph::exec::IExecutor* cbExec,
                     std::shared_ptr<::morph::async::detail::ISettleSink> sink) override {
        if (throwOnExecute) {
            throw std::runtime_error{"dispatch threw"};
        }
        LocalBackend::executeInto(mid, std::move(call), cbExec, std::move(sink));
    }

    void deregisterModel(::morph::exec::detail::ModelId mid) override {
        ++(*_released);
        LocalBackend::deregisterModel(mid);
    }

    void setReconnectHandler(std::function<void()> handler, ::morph::exec::IExecutor* exec) override {
        _reconnect = std::move(handler);
        _reconnectExec = exec;
    }

    /// Fires the installed reconnect handler on the executor it was given.
    void fireReconnect() const {
        if (_reconnect && _reconnectExec != nullptr) {
            _reconnectExec->post(_reconnect);
        }
    }

    [[nodiscard]] std::size_t heldBinds() const { return _binds->size(); }
    [[nodiscard]] std::size_t heldPromotes() const { return _promotes.size(); }
    /// Survives the backend, for a test that destroys the bridge that owns it.
    [[nodiscard]] std::shared_ptr<const int> releasedCounter() const { return _released; }

    /// Settles the oldest held bind with its instance.
    void resolveBind() { resolveFirst(*_binds); }
    /// Settles the oldest held bind with a failure.
    void rejectBind(const std::string& what = "bind refused") {
        auto held = std::move(_binds->front());
        _binds->erase(_binds->begin());
        held.promise.reject(std::make_exception_ptr(std::runtime_error{what}));
    }
    /// Settles the oldest held promote with its instance.
    void resolvePromote() {
        auto held = std::move(_promotes.front());
        _promotes.erase(_promotes.begin());
        held.promise.resolve(held.id);
    }
    /// Settles the oldest held promote with a failure.
    void rejectPromote() {
        auto held = std::move(_promotes.front());
        _promotes.erase(_promotes.begin());
        held.promise.reject(std::make_exception_ptr(std::runtime_error{"promote refused"}));
    }

    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    GateReply bindMode = GateReply::Hold;
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    GateReply promoteMode = GateReply::Hold;
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    bool throwOnExecute = false;

    struct Held {
        ::morph::exec::detail::ModelId id;
        ::morph::async::Completion<::morph::exec::detail::ModelId>::Promise promise;
    };
    using Stash = std::vector<Held>;

    /// The held bind replies, shared so a test can settle one after the backend
    /// that issued it is gone.
    [[nodiscard]] std::shared_ptr<Stash> bindStash() const { return _binds; }

    /// Settles the oldest reply of @p stash with its instance.
    static void resolveFirst(Stash& stash, std::optional<::morph::exec::detail::ModelId> as = std::nullopt) {
        auto held = std::move(stash.front());
        stash.erase(stash.begin());
        held.promise.resolve(as.value_or(held.id));
    }

private:
    std::shared_ptr<Stash> _binds = std::make_shared<Stash>();
    std::vector<Held> _promotes;
    std::shared_ptr<int> _released{std::make_shared<int>(0)};
    // Owns the completions of the instance-creating local binds, which are
    // read through takeSettled and never delivered.
    ::morph::exec::MainThreadExecutor _settledOn;
    std::function<void()> _reconnect;
    ::morph::exec::IExecutor* _reconnectExec = nullptr;
};

}  // namespace morph::testing
