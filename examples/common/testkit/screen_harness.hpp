// SPDX-License-Identifier: Apache-2.0
#pragma once

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testkit/fake_app_context.hpp"
#include "testkit/wait.hpp"

/// @file
/// The screen harness of spec 4 §4: an application's screen, opened through the same source and shell as the
/// generic client, bound to one of the rig's backends and mounted on `RecordingBackend`, driven by test id. Every
/// example's screen tests use it, so no example writes a client rig of its own.

namespace morph::examples::testing {

/// @brief Which backend the screen's models run behind.
enum class RigMode : std::uint8_t {
    Local,      ///< In process, over `LocalBackend`, as a local-mode client runs them.
    Simulated,  ///< Behind a `RemoteServer`, over `SimulatedRemoteBackend`: every call and every `ui-*` request is
                ///< encoded, authorised and decoded as on the wire.
};

/// @brief The name of a mode, for test output.
/// @param mode The mode.
/// @return `local` or `simulated`.
[[nodiscard]] inline std::string_view nameOf(RigMode mode) { return mode == RigMode::Local ? "local" : "simulated"; }

/// @brief A source behind a `RemoteServer` in process: the bundle served by its `UiService`, the models by the
///        server, both reached through the server's own envelope handling.
class SimulatedAppSource final : public ::morph::client::Source {
public:
    /// @brief A source for @p app.
    /// @param app The application.
    explicit SimulatedAppSource(::morph::client::LocalApp app) : _app{std::move(app)} {}
    ~SimulatedAppSource() override { close(); }
    SimulatedAppSource(SimulatedAppSource const&) = delete;
    SimulatedAppSource& operator=(SimulatedAppSource const&) = delete;
    SimulatedAppSource(SimulatedAppSource&&) = delete;
    SimulatedAppSource& operator=(SimulatedAppSource&&) = delete;

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& /*context*/) override {
        auto bundle = _app.bundle();
        if (!bundle) {
            return std::unexpected(
                ui::ConnectError{.kind = ui::ConnectError::Kind::Unsupported, .message = bundle.error().message()});
        }
        if (_app.setUp) {
            _app.setUp(::morph::client::LocalSetup{});
        }
        _workers = std::make_unique<exec::ThreadPoolExecutor>(2);
        backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(std::move(*bundle)));
        _server = _app.serverAuthorizer
                      ? std::make_shared<backend::RemoteServer>(*_workers, _app.serverAuthorizer(), config)
                      : std::make_shared<backend::RemoteServer>(*_workers, config);
        ui::Exchange exchange = [server = _server](wire::Envelope const& request,
                                                   std::function<void(ui::ExchangeReply)> const& done) {
            try {
                done(wire::decode(server->handleInline(wire::encode(request))));
            } catch (std::exception const& error) {
                done(std::unexpected(std::string{error.what()}));
            }
        };
        return Transport{.backend = std::make_unique<backend::SimulatedRemoteBackend>(*_server),
                         .exchange = std::move(exchange)};
    }

    void disconnect() override {
        _server.reset();
        _workers.reset();
    }

private:
    ::morph::client::LocalApp _app;
    std::unique_ptr<exec::ThreadPoolExecutor> _workers;
    std::shared_ptr<backend::RemoteServer> _server;
};

/// @brief One screen of an application, opened and mounted on `RecordingBackend`, driven by test id.
///
/// Everything runs on the harness's owner executor, which only the harness's waits pump.
class ScreenHarness {
public:
    /// @brief Opens @p app in @p mode and mounts @p screenId.
    /// @param app      The application.
    /// @param screenId The screen; empty for the first the catalog lists.
    /// @param mode     The backend the models run behind.
    /// @param params   The screen's params.
    /// @param setup    Local mode's options, such as the user its calls run as; simulated mode ignores them.
    /// @param widthClass The width class the backend reports from the first mount: `compact`, `medium` or
    ///                   `expanded`.
    /// @throws std::runtime_error when the application does not open or the screen does not mount.
    ScreenHarness(::morph::client::LocalApp const& app, std::string screenId, RigMode mode, ui::ParamMap params = {},
                  ::morph::client::LocalSetup setup = {}, std::string widthClass = "expanded") {
        _backend.setWidthClass(std::move(widthClass));
        if (mode == RigMode::Local) {
            _source = std::make_unique<::morph::client::LocalSource>(app, std::move(setup));
        } else {
            _source = std::make_unique<SimulatedAppSource>(app);
        }
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        _source->open(_context,
                      [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        if (!pumpUntil(_context.owner(), [&opened] { return opened.has_value(); }) || !opened.has_value()) {
            throw std::runtime_error{"ScreenHarness: the application did not open in time"};
        }
        std::expected<ui::Bundle, ui::ConnectError> bundle = std::move(opened).value();
        if (!bundle) {
            throw std::runtime_error{"ScreenHarness: " + bundle.error().message};
        }
        _shell = ::morph::client::mountShell(
            _context, _backend, std::move(bundle), *_source,
            ::morph::client::ShellOptions{
                .startScreen = std::move(screenId),
                .startParams = std::move(params),
                .onProblem = [this](std::string const& problem) { _problems.push_back(problem); },
                .onMounted = [this](std::string const& mounted) { _mounted.push_back(mounted); },
                .downloads = _downloads});
        if (_mounted.empty()) {
            throw std::runtime_error{"ScreenHarness: no screen mounted" +
                                     (_problems.empty() ? std::string{} : ": " + _problems.front())};
        }
    }

    ~ScreenHarness() {
        _shell.reset();
        _source->close();
        std::error_code ignored;
        std::filesystem::remove_all(_downloads, ignored);
    }
    ScreenHarness(ScreenHarness const&) = delete;
    ScreenHarness& operator=(ScreenHarness const&) = delete;
    ScreenHarness(ScreenHarness&&) = delete;
    ScreenHarness& operator=(ScreenHarness&&) = delete;

    /// @brief The widgets.
    /// @return The backend.
    [[nodiscard]] ui::testing::RecordingBackend& backend() noexcept { return _backend; }

    /// @brief Changes the width class the backend reports, as a resize would; the shell hands it to the screen.
    /// @param widthClass `compact`, `medium` or `expanded`.
    void setWidthClass(std::string widthClass) { _backend.setWidthClass(std::move(widthClass)); }

    /// @brief Where a screen's downloads are saved: a directory of this harness's own, removed with it.
    /// @return It.
    [[nodiscard]] std::filesystem::path const& downloads() const noexcept { return _downloads; }

    /// @brief Every problem reported so far: a document that did not load, a refused mount, a diagnostic.
    /// @return The problems, in order.
    [[nodiscard]] std::vector<std::string> const& problems() const noexcept { return _problems; }

    /// @brief The screens mounted so far, in order.
    /// @return Their ids.
    [[nodiscard]] std::vector<std::string> const& mounted() const noexcept { return _mounted; }

    /// @brief The widget of @p kind whose test id is @p testId.
    /// @param kind   The widget kind, as `RecordingBackend` names it (`Text`, `Button`, …).
    /// @param testId The test id.
    /// @return Its id, or nothing.
    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string_view testId) const {
        return _backend.find(kind, "testId", testId);
    }

    /// @brief The text a `Text` with @p testId shows.
    /// @param testId The test id.
    /// @return The text, or nothing when no such widget is mounted.
    [[nodiscard]] std::optional<std::string> text(std::string_view testId) const {
        auto const found = find("Text", testId);
        if (!found) {
            return std::nullopt;
        }
        std::string const plain = _backend.prop(*found, "text");
        // A text with runs is recorded as its runs; its text is their characters, joined.
        return plain.empty() ? morph::ui::testing::detail::spansCharacters(_backend.prop(*found, "spans")) : plain;
    }

    /// @brief Clicks the button with @p testId.
    /// @param testId The test id.
    void click(std::string_view testId) { _backend.click(require("Button", testId)); }

    /// @brief Types @p value into the text input with @p testId, as the user would.
    /// @param testId The test id.
    /// @param value  The new text.
    void edit(std::string_view testId, std::string value) {
        _backend.edit(require("TextInput", testId), std::move(value));
    }

    /// @brief Toggles the checkbox with @p testId.
    /// @param testId The test id.
    void toggle(std::string_view testId) { _backend.toggle(require("Checkbox", testId)); }

    /// @brief Selects tab @p index of the tab bar with @p testId. The bar marks the pick at once; the page is mounted,
    ///        the first time, and shown when the screen next settles, which `shownUnder` tells.
    /// @param testId The tab bar's test id.
    /// @param index  The tab, from 0.
    void selectTab(std::string_view testId, std::size_t index) {
        _backend.chooseIndex(require("Tabs", testId), index);
    }

    /// @brief Whether @p widget is under @p root with nothing hidden between them, itself included: what a user sees.
    /// @param root   The container to look from.
    /// @param widget The widget.
    /// @return Whether it is there and shown.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the container, then the widget inside it.
    [[nodiscard]] bool shownUnder(int root, int widget) const {
        std::vector<std::pair<int, bool>> pending{{root, true}};
        while (!pending.empty()) {
            auto const [current, shownSoFar] = pending.back();
            pending.pop_back();
            bool const shown = shownSoFar && _backend.prop(current, "visible") != "false";
            if (current == widget) {
                return shown;
            }
            for (int const child : _backend.children(current)) {
                pending.emplace_back(child, shown);
            }
        }
        return false;
    }

    /// @brief Chooses the option @p key in the select with @p testId.
    /// @param testId The test id.
    /// @param key    The option's key.
    void choose(std::string_view testId, std::string key) {
        _backend.choose(require("Select", testId), ui::Key{std::move(key)});
    }

    /// @brief A widget under another: the first of @p kind with @p testId inside the `Column` with @p rootTestId, such
    ///        as a field of one form when a screen has several, whose fields repeat their test ids.
    /// @param rootTestId The container's test id, such as a form node's.
    /// @param kind       The widget kind.
    /// @param testId     The widget's test id.
    /// @return Its id, or nothing.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the container, then the widget's kind and test id.
    [[nodiscard]] std::optional<int> findIn(std::string_view rootTestId, std::string_view kind,
                                            std::string_view testId) const {
        auto const root = find("Column", rootTestId);
        if (!root) {
            return std::nullopt;
        }
        std::vector<int> pending{*root};
        while (!pending.empty()) {
            int const widget = pending.back();
            pending.pop_back();
            if (_backend.kindOf(widget) == kind && _backend.prop(widget, "testId") == testId) {
                return widget;
            }
            for (int const child : _backend.children(widget)) {
                pending.push_back(child);
            }
        }
        return std::nullopt;
    }

    /// @brief Types @p value into field @p field of the form node with @p formTestId, as the user would.
    /// @param formTestId The form node's test id.
    /// @param field      The field's wire name.
    /// @param value      The new text.
    /// @throws std::runtime_error when the form or the field is not mounted.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the form, then its field.
    void editField(std::string_view formTestId, std::string const& field, std::string value) {
        auto const input = findIn(formTestId, "TextInput", "field_" + field);
        if (!input) {
            throw std::runtime_error{"ScreenHarness: no field " + field + " in form " + std::string{formTestId}};
        }
        _backend.edit(*input, std::move(value));
    }

    /// @brief Chooses option @p key, the option's JSON literal, in select field @p field of the form node with
    ///        @p formTestId, as the user would.
    /// @param formTestId The form node's test id.
    /// @param field      The field's wire name.
    /// @param key        The option's JSON literal, such as `"\"EUR\""` for an enum.
    /// @throws std::runtime_error when the form or the field is not mounted.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the form, its field, then the option.
    void chooseField(std::string_view formTestId, std::string const& field, std::string key) {
        auto const select = findIn(formTestId, "Select", "field_" + field);
        if (!select) {
            throw std::runtime_error{"ScreenHarness: no select " + field + " in form " + std::string{formTestId}};
        }
        _backend.choose(*select, ui::Key{std::move(key)});
    }

    /// @brief Sets date field @p field of the form node with @p formTestId to @p value, as the user would.
    /// @param formTestId The form node's test id.
    /// @param field      The field's wire name.
    /// @param value      The instant.
    /// @throws std::runtime_error when the form or the field is not mounted.
    void setDateField(std::string_view formTestId, std::string const& field, morph::time::Timestamp value) {
        auto const input = findIn(formTestId, "DateTimeInput", "field_" + field);
        if (!input) {
            throw std::runtime_error{"ScreenHarness: no date field " + field + " in form " + std::string{formTestId}};
        }
        _backend.setDateTime(*input, value);
    }

    /// @brief Clicks the button with @p testId inside the container with @p rootTestId, such as a form's add-row
    ///        button.
    /// @param rootTestId The container's test id.
    /// @param testId     The button's test id.
    /// @throws std::runtime_error when the button is not mounted there.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the container, then the button.
    void clickIn(std::string_view rootTestId, std::string_view testId) {
        auto const button = findIn(rootTestId, "Button", testId);
        if (!button) {
            throw std::runtime_error{"ScreenHarness: no button " + std::string{testId} + " in " +
                                     std::string{rootTestId}};
        }
        _backend.click(*button);
    }

    /// @brief Submits the form node with @p formTestId once its submit button is enabled, which it is once the edits
    ///        have settled into the form.
    /// @param formTestId The form node's test id.
    /// @return Whether the button was enabled in time and clicked.
    [[nodiscard]] bool submitForm(std::string_view formTestId) {
        auto const button = findIn(formTestId, "Button", "form_submit");
        if (!button || !waitFor([this, &button] { return _backend.prop(*button, "enabled") != "false"; })) {
            if (auto const status = findIn(formTestId, "Text", "form_status")) {
                _problems.push_back(std::string{formTestId} + " not ready: " + _backend.prop(*status, "text"));
            }
            return false;
        }
        _backend.click(*button);
        return true;
    }

    /// @brief Advances the scheduler's clock by @p delta, firing the timers then due, such as a query's
    ///        `refreshEvery`. The harness's scheduler is manual: no timer fires unless a test advances it.
    /// @param delta How far.
    void advance(std::chrono::milliseconds delta) { _context.clock().advance(delta); }

    /// @brief Pumps the owner until @p pred holds.
    /// @param pred   The condition.
    /// @param budget The unscaled budget.
    /// @return Whether it held in time.
    template <std::predicate Pred>
    [[nodiscard]] bool waitFor(Pred pred, std::chrono::milliseconds budget = kWaitBudget) {
        return pumpUntil(_context.owner(), std::move(pred), budget);
    }

    /// @brief Pumps the owner until the `Text` with @p testId shows @p expected.
    /// @param testId   The test id.
    /// @param expected The text.
    /// @return Whether it did in time.
    [[nodiscard]] bool waitForText(std::string_view testId, std::string_view expected) {
        return waitFor([this, testId, expected] { return text(testId) == expected; });
    }

private:
    [[nodiscard]] int require(std::string_view kind, std::string_view testId) const {
        auto const found = find(kind, testId);
        if (!found) {
            throw std::runtime_error{"ScreenHarness: no " + std::string{kind} + " with test id " +
                                     std::string{testId}};
        }
        return *found;
    }

    FakeAppContext _context;
    ui::testing::RecordingBackend _backend;
    std::unique_ptr<::morph::client::Source> _source;
    std::unique_ptr<ui::MountedShell> _shell;
    // One directory per harness, so two tests never read each other's downloads.
    std::filesystem::path _downloads = [] {
        static std::atomic<int> made{0};
        // Unique across processes too: ctest runs several test binaries at once.
        static unsigned const run = std::random_device{}();
        auto directory = std::filesystem::temp_directory_path() /
                         ("morph-harness-downloads-" + std::to_string(run) + "-" + std::to_string(made++));
        std::filesystem::create_directories(directory);
        return directory;
    }();
    std::vector<std::string> _problems;
    std::vector<std::string> _mounted;
};

}  // namespace morph::examples::testing
