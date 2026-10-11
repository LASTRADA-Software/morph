// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file
/// @brief The harness of the interpreter corpora: a scripted dispatcher whose replies a test sends in any order, a
///        host that records what it was asked, and a fixture that loads a document and mounts it on
///        `RecordingBackend`.

namespace morph::ui::testing {

/// @brief A failure with an error kind the harness's classifier reads back.
struct KindedError : std::runtime_error {
    /// @brief A failure.
    /// @param errorKind The error kind.
    /// @param text The text.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the only caller passes a kind and a text it names.
    KindedError(std::string errorKind, std::string const& text)
        : std::runtime_error{text}, kind{std::move(errorKind)} {}
    /// @brief The error kind.
    std::string kind;
};

/// @brief A dispatcher whose calls wait until the test answers them.
class ScriptedDispatcher final : public Dispatcher {
public:
    /// @brief One call.
    struct Call {
        std::string typeId;
        bridge::BindSharing sharing = bridge::BindSharing::Private;
        std::string instanceKey;
        std::size_t channel = 0;
        std::string action;
        std::string body;
        CallOptions options;
        async::Completion<std::string>::Promise promise;
        bool answered = false;
    };

    /// @brief A dispatcher whose replies are posted on @p owner.
    /// @param owner The runtime's owner.
    explicit ScriptedDispatcher(exec::IExecutor& owner) : _owner{&owner} {}

    [[nodiscard]] std::shared_ptr<ModelChannel> bind(std::string const& typeId, bridge::BindSharing sharing,
                                                     std::string const& instanceKey) override {
        binds.push_back(typeId + (sharing == bridge::BindSharing::Shared ? " shared " : " private ") + instanceKey);
        return std::make_shared<Channel>(*this, typeId, sharing, instanceKey, binds.size());
    }

    /// @brief Every call sent, oldest first.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double.
    std::vector<Call> calls;
    /// @brief Every bind: `type shared|private key`.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double.
    std::vector<std::string> binds;
    /// @brief Replies sent at once, by action.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double.
    std::map<std::string, std::string, std::less<>> autoReply;

    /// @brief The calls of an action.
    /// @param action The action.
    /// @return How many were sent.
    [[nodiscard]] std::size_t count(std::string_view action) const {
        std::size_t total = 0;
        for (Call const& call : calls) {
            if (call.action == action) {
                ++total;
            }
        }
        return total;
    }

    /// @brief The index of the last call of an action.
    /// @param action The action.
    /// @return The index.
    [[nodiscard]] std::size_t last(std::string_view action) const {
        std::size_t index = calls.size();
        while (index > 0 && calls.at(index - 1).action != action) {
            --index;
        }
        if (index == 0) {
            FAIL("no call of " << action);
        }
        return index - 1;
    }

    /// @brief Answers a call.
    /// @param index The call.
    /// @param json  The reply.
    void reply(std::size_t index, std::string json) {
        Call& call = calls.at(index);
        REQUIRE_FALSE(call.answered);
        call.answered = true;
        call.promise.resolve(std::move(json));
    }

    /// @brief Fails a call.
    /// @param index The call.
    /// @param kind  The error kind.
    /// @param text  The text.
    void fail(std::size_t index, std::string kind = "error", std::string const& text = "it failed") {
        Call& call = calls.at(index);
        REQUIRE_FALSE(call.answered);
        call.answered = true;
        call.promise.reject(std::make_exception_ptr(KindedError{std::move(kind), text}));
    }

private:
    class Channel final : public ModelChannel {
    public:
        Channel(ScriptedDispatcher& owner, std::string typeId, bridge::BindSharing sharing, std::string key,
                std::size_t number)
            : _owner{&owner}, _typeId{std::move(typeId)}, _sharing{sharing}, _key{std::move(key)}, _number{number} {}

        [[nodiscard]] async::Completion<std::string> execute(std::string const& actionId, std::string const& bodyJson,
                                                             CallOptions options) override {
            auto [completion, promise] = async::Completion<std::string>::makeSettleable(_owner->_owner);
            _owner->calls.push_back(Call{.typeId = _typeId,
                                         .sharing = _sharing,
                                         .instanceKey = _key,
                                         .channel = _number,
                                         .action = actionId,
                                         .body = bodyJson,
                                         .options = options,
                                         .promise = std::move(promise),
                                         .answered = false});
            if (auto const found = _owner->autoReply.find(actionId); found != _owner->autoReply.end()) {
                _owner->reply(_owner->calls.size() - 1, found->second);
            }
            return std::move(completion);
        }

    private:
        ScriptedDispatcher* _owner;
        std::string _typeId;
        bridge::BindSharing _sharing;
        std::string _key;
        std::size_t _number;
    };

    exec::IExecutor* _owner;
};

/// @brief A host that records every request and answers as told.
class RecordingHost final : public InterpreterHost {
public:
    /// @brief What it was asked, one line each.
    std::vector<std::string> log;
    /// @brief The host commands it has.
    std::vector<std::string> commands;
    /// @brief The backends it lists.
    std::vector<std::string> backends;
    /// @brief The last `navigate`'s result and cancel callbacks.
    std::function<void(Value)> onResult;
    /// @brief The last `navigate`'s cancel callback.
    std::function<void()> onCancel;
    /// @brief The last upload's callbacks.
    std::function<void(Value)> uploadProgress;
    /// @brief The last upload's completion.
    std::function<void(std::expected<Value, std::string>)> uploadDone;
    /// @brief The last download's completion.
    std::function<void(std::expected<Value, std::string>)> downloadDone;
    /// @brief The values `signIn` installed.
    std::vector<Value> signedIn;
    /// @brief How often permissions went stale.
    int stale = 0;
    /// @brief Whether `upload` and `download` answer "not supported".
    bool refuseTransfers = false;
    /// @brief Whether every other request answers "not supported".
    bool refuseAll = false;

    bool navigate(NavigateRequest const& request, std::function<void(Value)> result,
                  std::function<void()> cancel) override {
        log.push_back("navigate " + request.screen + " as " + request.as + " " +
                      glz::write_json(valueToJson(request.params)).value_or(""));
        onResult = std::move(result);
        onCancel = std::move(cancel);
        return !refuseAll;
    }
    bool closeSelf(std::string const& screen, std::optional<Value> result) override {
        log.push_back("close " + screen + (result ? " " + glz::write_json(valueToJson(*result)).value_or("") : ""));
        return !refuseAll;
    }
    bool focus(std::string const& screen, std::string const& element) override {
        log.push_back("focus " + screen + " " + element);
        return !refuseAll;
    }
    bool hostCommand(std::string const& name, Value const& args) override {
        log.push_back("host " + name + " " + glz::write_json(valueToJson(args)).value_or(""));
        return std::ranges::find(commands, name) != commands.end();
    }
    bool switchBackend(std::string const& name) override {
        log.push_back("switchBackend " + name);
        return std::ranges::find(backends, name) != backends.end();
    }
    void resetPrefs(std::string const& screen, std::string const& point) override {
        log.push_back("resetPrefs " + screen + " " + point);
    }
    void setLocale(std::string const& locale) override { log.push_back("setLocale " + locale); }
    bool notify(std::string const& text, std::string const& role) override {
        log.push_back("notify " + role + " " + text);
        return !refuseAll;
    }
    bool signIn(Value const& reply) override {
        signedIn.push_back(reply);
        log.emplace_back("signIn");
        return !refuseAll;
    }
    void signOut() override { log.emplace_back("signOut"); }
    bool setAppValue(std::string const& name, Value const& value) override {
        log.push_back("app " + name + " " + glz::write_json(valueToJson(value)).value_or(""));
        return !refuseAll;
    }
    bool upload(Value const& file, std::function<void(Value)> progress,
                std::function<void(std::expected<Value, std::string>)> done) override {
        log.push_back("upload " + glz::write_json(valueToJson(file)).value_or(""));
        uploadProgress = std::move(progress);
        uploadDone = std::move(done);
        return !refuseTransfers;
    }
    bool download(Value const& file, std::string const& option, Value const& name,
                  std::function<void(std::expected<Value, std::string>)> done) override {
        log.push_back("download " + glz::write_json(valueToJson(file)).value_or("") + " " + option + " " +
                      glz::write_json(valueToJson(name)).value_or(""));
        downloadDone = std::move(done);
        return !refuseTransfers;
    }
    void preferenceChanged(std::string const& screen, std::string const& point, Value const& value) override {
        log.push_back("pref " + screen + " " + point + " " + glz::write_json(valueToJson(value)).value_or(""));
    }
    void permissionsStale() override { ++stale; }
};

/// @brief Loads a document from JSON text, failing the test on a refusal.
/// @param text    The document.
/// @param catalog The catalog facts.
/// @return The document.
inline std::shared_ptr<Document const> loadOrFail(std::string_view text, Catalog catalog = {}) {
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = std::move(catalog)});
    if (!loaded) {
        FAIL(loaded.error().message());
    }
    return std::make_shared<Document const>(std::move(*loaded));
}

/// @brief The catalog of the corpora: which models are keyed or stateless, and which fields are calculated or
///        secret.
/// @return The catalog.
inline Catalog corpusCatalog() {
    Catalog catalog;
    catalog.model = [](std::string_view type) -> std::optional<ModelInfo> {
        if (type == "Lims_SampleModel") {
            return ModelInfo{.stateless = false, .keyed = true};
        }
        if (type == "Lims_ClientModel") {
            return ModelInfo{.stateless = true, .keyed = false};
        }
        return ModelInfo{};
    };
    catalog.action = [](std::string_view action) -> std::optional<ActionInfo> {
        ActionInfo info;
        info.modelType = "Lims_SampleModel";
        if (action.starts_with("Lims_Client")) {
            info.modelType = "Lims_ClientModel";
        } else if (action.starts_with("Auth_")) {
            info.modelType = "Auth_SessionModel";
        } else if (action.starts_with("Lims_Cat")) {
            info.modelType = "Lims_CatalogModel";
        }
        if (action == "Auth_Token") {
            info.secretFields = {"token"};
        }
        if (action == "Auth_Login") {
            info.credential = true;
        }
        if (action == "Lims_ListResults") {
            info.calculatedFields = {"concentration"};
        }
        return info;
    };
    return catalog;
}

/// @brief One client: an owner, a runtime, a clock, a session, a dispatcher, a host and a backend.
struct Harness {
    morph::testing::StepExecutor owner;
    reactive::Runtime runtime{owner};
    reactive::testing::ManualScheduler scheduler;
    ui::Session session{runtime, SessionOptions{.scheduler = &scheduler,
                                                .clock = [this] { return scheduler.now(); },
                                                .keepUnused = std::chrono::minutes{5},
                                                .catalog = corpusCatalog(),
                                                .classify = [](std::exception_ptr const& error) -> std::string {
                                                    try {
                                                        std::rethrow_exception(error);
                                                    } catch (KindedError const& kinded) {
                                                        return kinded.kind;
                                                    } catch (...) {
                                                        return "error";
                                                    }
                                                }}};
    ScriptedDispatcher dispatcher{owner};
    RecordingHost host;
    ui::testing::RecordingBackend backend;
    std::vector<Refusal> diagnostics;
    Interpreter interpreter{session, dispatcher,
                            InterpreterOptions{.host = &host, .diagnostics = [this](Refusal const& refusal) {
                                                   diagnostics.push_back(refusal);
                                               }}};

    /// @brief Loads and mounts a document, then lets the owner run.
    /// @param text    The document.
    /// @param params  The params.
    /// @param options The mount options.
    /// @return The screen.
    std::unique_ptr<Screen> mount(std::string_view text, ValueRecord const& params = {},
                                  MountOptions const& options = {}) {
        auto screen = interpreter.mount(loadOrFail(text, corpusCatalog()), params, backend, options);
        if (!screen) {
            FAIL(screen.error().message());
        }
        settle();
        return std::move(*screen);
    }

    /// @brief Runs everything posted to the owner.
    void settle() { owner.runAll(); }

    /// @brief Advances the clock, then settles.
    /// @param delta How far.
    void advance(std::chrono::milliseconds delta) {
        scheduler.advance(delta);
        settle();
    }

    /// @brief Answers a call, then settles.
    /// @param index The call.
    /// @param json  The reply.
    void reply(std::size_t index, std::string json) {
        dispatcher.reply(index, std::move(json));
        settle();
    }

    /// @brief Fails a call, then settles.
    /// @param index The call.
    /// @param kind  The error kind.
    void fail(std::size_t index, std::string kind = "error") {
        dispatcher.fail(index, std::move(kind));
        settle();
    }

    /// @brief The first widget of a kind whose property has a value.
    /// @param kind  The kind.
    /// @param name  The property.
    /// @param value The value.
    /// @return Its id.
    int find(std::string_view kind, std::string_view name, std::string_view value) const {
        auto const found = backend.find(kind, name, value);
        if (!found) {
            FAIL("no " << kind << " with " << name << "=" << value << " in\n" << backend.dump());
        }
        return *found;
    }

    /// @brief The widget whose test id is @p testId.
    /// @param kind   The kind.
    /// @param testId The test id.
    /// @return Its id.
    int byTestId(std::string_view kind, std::string_view testId) const { return find(kind, "testId", testId); }

    /// @brief A widget's text.
    /// @param testId The text widget's test id.
    /// @return Its text.
    std::string text(std::string_view testId) const { return backend.prop(byTestId("Text", testId), "text"); }
};

/// @brief Reads an expression in a screen.
/// @param screen The screen.
/// @param json   The expression's JSON.
/// @return The value's JSON text.
inline std::string readJson(Screen const& screen, std::string_view json) {
    auto parsed = parseJson(json);
    REQUIRE(parsed);
    return glz::write_json(valueToJson(screen.read(*parsed))).value_or("");
}

}  // namespace morph::ui::testing
