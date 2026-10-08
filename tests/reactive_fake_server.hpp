// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace morph::testing {

/// @brief Stands in for a server behind a `Completion`-returning fetcher: every call is held until the
///        test settles it, in any order.
///
/// Each call carries a stop source, as a bridge call to a `Task` handler does, so a test can see whether
/// whoever attached to the completion asked the call to stop.
/// @tparam R The result type.
template <typename R = std::string>
class FakeServer {
public:
    /// @param owner The executor every completion delivers on.
    explicit FakeServer(::morph::exec::IExecutor& owner) : _owner{&owner} {}

    /// @brief Starts one call.
    /// @tparam A The action type; its `id`, when it has one, is recorded.
    /// @param action The action.
    /// @return The call's completion, settled by `resolve()` or `reject()`.
    template <typename A>
    ::morph::async::Completion<R> fetch(A const& action) {
        auto [completion, promise] = ::morph::async::Completion<R>::makeSettleable(_owner);
        auto stop = std::make_shared<::core::async::StopSource>();
        completion.state()->stopSource = stop;
        int id = -1;
        if constexpr (requires { action.id; }) {
            id = action.id;
        }
        _calls.push_back(Call{.id = id, .promise = std::move(promise), .stop = std::move(stop)});
        return std::move(completion);
    }

    /// @brief A fetcher that calls `fetch()`.
    /// @tparam A The action type.
    /// @return A callable taking `A const&`; it refers to this server, which must outlive it.
    template <typename A>
    auto via() {
        return [this](A const& action) { return fetch(action); };
    }

    /// @brief Settles call @p index with @p value.
    /// @param index The call, in issue order.
    /// @param value The result.
    void resolve(std::size_t index, R value) { _calls.at(index).promise.resolve(std::move(value)); }

    /// @brief Fails call @p index with a `std::runtime_error`.
    /// @param index The call, in issue order.
    /// @param what The error's message.
    void reject(std::size_t index, std::string const& what) {
        _calls.at(index).promise.reject(std::make_exception_ptr(std::runtime_error{what}));
    }

    /// @brief How many calls were started.
    /// @return The call count.
    [[nodiscard]] std::size_t calls() const { return _calls.size(); }

    /// @brief The `id` of call @p index's action.
    /// @param index The call, in issue order.
    /// @return The id, or -1 for an action without one.
    [[nodiscard]] int idOf(std::size_t index) const { return _calls.at(index).id; }

    /// @brief Whether call @p index was asked to stop.
    /// @param index The call, in issue order.
    /// @return True once stop was requested on the call's stop source.
    [[nodiscard]] bool stopRequested(std::size_t index) const { return _calls.at(index).stop->stop_requested(); }

private:
    struct Call {
        int id = 0;
        ::morph::async::Completion<R>::Promise promise;
        std::shared_ptr<::core::async::StopSource> stop;
    };
    ::morph::exec::IExecutor* _owner;
    std::vector<Call> _calls;
};

}  // namespace morph::testing
