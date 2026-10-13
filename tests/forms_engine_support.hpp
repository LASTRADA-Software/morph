// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms_engine_support.hpp
/// @brief A fake server for the forms engine's Submitter and ChoiceFetcher, and the harness
///        (owner, runtime, server) every engine test builds sessions in.

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/runtime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace morph::test::formsengine {

/// @brief One call the engine made.
struct Call {
    std::string action;
    std::string body;
};

/// @brief Records submits and options fetches and answers them on the owner.
///
/// A responder returning a value resolves the call at once (delivered on the next drain); one
/// returning `std::nullopt` leaves it pending until `resolve…`/`reject…` settles it by index.
class FakeServer {
public:
    using Responder = std::function<std::optional<std::string>(Call const&)>;

    explicit FakeServer(exec::IExecutor& owner) : _owner{&owner} {}

    void replyWith(Responder responder) { _submitResponder = std::move(responder); }
    void optionsWith(Responder responder) { _fetchResponder = std::move(responder); }

    [[nodiscard]] forms::Submitter submitter() {
        return [this](std::string_view action, std::string body) {
            return record(_submits, _submitPromises, _submitResponder, action, std::move(body));
        };
    }

    [[nodiscard]] forms::ChoiceFetcher fetcher() {
        return [this](std::string_view action, std::string body) {
            return record(_fetches, _fetchPromises, _fetchResponder, action, std::move(body));
        };
    }

    [[nodiscard]] std::vector<Call> const& submits() const noexcept { return _submits; }
    [[nodiscard]] std::vector<Call> const& fetches() const noexcept { return _fetches; }
    [[nodiscard]] std::string const& lastBody() const { return _submits.back().body; }

    void resolveSubmit(std::size_t index, std::string reply) { _submitPromises.at(index)->resolve(std::move(reply)); }
    void rejectSubmit(std::size_t index, std::string const& message) {
        _submitPromises.at(index)->reject(std::make_exception_ptr(std::runtime_error{message}));
    }
    void resolveFetch(std::size_t index, std::string reply) { _fetchPromises.at(index)->resolve(std::move(reply)); }
    void rejectFetch(std::size_t index, std::string const& message) {
        _fetchPromises.at(index)->reject(std::make_exception_ptr(std::runtime_error{message}));
    }

private:
    using Promise = async::Completion<std::string>::Promise;

    async::Completion<std::string> record(std::vector<Call>& calls, std::vector<std::shared_ptr<Promise>>& promises,
                                          Responder const& responder, std::string_view action, std::string body) {
        calls.push_back(Call{.action = std::string{action}, .body = std::move(body)});
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(_owner);
        auto const shared = std::make_shared<Promise>(std::move(promise));
        promises.push_back(shared);
        if (responder) {
            if (auto reply = responder(calls.back())) {
                shared->resolve(*std::move(reply));
            }
        }
        return std::move(completion);
    }

    exec::IExecutor* _owner;
    Responder _submitResponder = [](Call const&) { return std::optional<std::string>{R"({"ok":true})"}; };
    Responder _fetchResponder = [](Call const&) { return std::optional<std::string>{"[]"}; };
    std::vector<Call> _submits;
    std::vector<Call> _fetches;
    std::vector<std::shared_ptr<Promise>> _submitPromises;
    std::vector<std::shared_ptr<Promise>> _fetchPromises;
};

/// @brief Reads a schema that must be well-formed.
[[nodiscard]] inline forms::FormModel modelOf(std::string_view action, std::string_view schema) {
    auto model = forms::FormModel::fromSchema(action, schema);
    INFO((model ? std::string{} : model.error().path + ": " + model.error().message));
    REQUIRE(model.has_value());
    return *std::move(model);
}

/// @brief The owner, runtime and server a test builds sessions over. Declare sessions after it.
struct Harness {
    testing::StepExecutor owner{};
    reactive::Runtime rt{owner};
    FakeServer server{owner};

    [[nodiscard]] std::unique_ptr<forms::FormSession> session(std::string_view action, std::string_view schema,
                                                              forms::FormSessionOptions options = {}) {
        return std::make_unique<forms::FormSession>(rt, modelOf(action, schema), server.submitter(), server.fetcher(),
                                                    std::move(options));
    }

    /// @brief Runs every posted flush and delivery.
    void settle() { owner.runAll(); }

    /// @brief A user edit: sets a field's draft, then settles.
    void type(forms::FormSession& session, std::string_view path, std::string_view text) {
        session.field(path).text().set(std::string{text});
        settle();
    }
};

}  // namespace morph::test::formsengine
