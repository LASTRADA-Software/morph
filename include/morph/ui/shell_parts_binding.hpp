// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../reactive/signal.hpp"
#include "interpreter.hpp"
#include "shell_frame.hpp"
#include "shell_parts.hpp"
#include "shell_parts_model.hpp"
#include "shell_parts_view.hpp"

/// @file
/// @brief `morph::ui::ShellPartsBinding`: joins an application's declared shell parts, read by a `ShellPartsModel`,
///        to the `ShellFrame` that draws them, and carries the frame's choices back.
///
/// Specified in `docs/spec/ui/shell_parts.md`.

namespace morph::ui {

/// @brief What a binding asks of its client.
struct ShellBindingOptions {
    /// @brief What the model asks; `confirmDiscard` is the binding's own (a dialog of the frame) and is ignored here.
    ShellModelOptions model;
    /// @brief Opens a screen as a tab: a chosen hit, or the inbox.
    std::function<void(ShellPartsModel::Target const&)> open;
    /// @brief Opens several hits as tabs, in order, and leaves the first one the active tab (Shift Enter). Empty:
    ///        Shift Enter opens nothing.
    std::function<void(std::vector<ShellPartsModel::Target> const&)> openAll;
    /// @brief The person's menu changed the language, after the session took it.
    std::function<void()> localeChosen;
    /// @brief Called each time the switcher's readiness changes, and once at the start.
    std::function<void(bool ready)> contextReady;
};

/// @brief The declared parts of a shell, read and drawn.
///
/// It makes the model, the effects that follow it and the frame's handlers, and takes them away in its destructor.
/// Neither copyable nor movable; it keeps its own copy of the parts, and borrows the session, the app root and the
/// frame, and must go before any of them.
class ShellPartsBinding {
public:
    /// @brief The binding.
    /// @param session The session. Borrowed.
    /// @param root    The app root whose queries the parts are. Borrowed.
    /// @param parts   The parts; copied.
    /// @param frame   The frame that draws them. Borrowed.
    /// @param options What it asks of its client.
    ShellPartsBinding(Session& session MORPH_LIFETIMEBOUND, AppRoot& root MORPH_LIFETIMEBOUND,
                      ShellParts const& parts MORPH_LIFETIMEBOUND, ShellFrame& frame MORPH_LIFETIMEBOUND,
                      ShellBindingOptions options)
        : _session{&session}, _frame{&frame}, _parts{parts}, _options{std::move(options)} {
        ShellModelOptions modelOptions = _options.model;
        modelOptions.confirmDiscard = [this](std::size_t tabs, std::function<void(bool)> done) {
            ask(tabs, std::move(done));
        };
        _model = std::make_unique<ShellPartsModel>(session, root, _parts, std::move(modelOptions));
        ShellPartsModel* const model = _model.get();
        ShellPartsHandlers handlers;
        handlers.onContext = [model](std::size_t index) { model->chooseContext(index); };
        handlers.onSearchText = [model](std::string const& text) { model->setSearchText(text); };
        handlers.onSearchOpen = [this, model](std::size_t source, std::size_t hit) {
            if (auto const target = model->hit(source, hit); target && _options.open) {
                _options.open(*target);
            }
        };
        handlers.onSearchOpenAll = [this, model] {
            if (_options.openAll) {
                _options.openAll(model->allHits());
            }
        };
        handlers.onInbox = [this, model] {
            std::string const screen = model->inboxScreen();
            if (!screen.empty() && _options.open) {
                _options.open(ShellPartsModel::Target{.screen = screen, .params = {}});
            }
        };
        handlers.onLocale = [this, model](std::size_t index) {
            model->chooseLocale(index);
            if (_options.localeChosen) {
                _options.localeChosen();
            }
        };
        handlers.onFormat = [model](std::size_t index) { model->chooseFormat(index); };
        handlers.onHelp = [this, model] {
            if (auto const target = model->helpScreen()) {
                if (_options.open) {
                    _options.open(*target);
                }
            } else {
                static_cast<void>(model->runHelp());
            }
        };
        _frame->setPartsHandlers(std::move(handlers));
        if (!_parts.modules.empty()) {
            _frame->navigator().setModuleGroups(true);
        }
        reactive::Runtime& runtime = session.runtime();
        _view = std::make_unique<reactive::Effect>(runtime, [this, &runtime] {
            ShellPartsView view = _model->view();
            runtime.untracked([this, &view] { _frame->setParts(std::move(view)); });
        });
        _counts = std::make_unique<reactive::Effect>(runtime, [this, &runtime] {
            auto counts = _model->moduleCounts();
            ShellLabels const labels = shellLabels(_session->formatting());
            runtime.untracked([this, &counts, &labels] { showCounts(counts, labels); });
        });
        _ready = std::make_unique<reactive::Effect>(runtime, [this, &runtime] {
            bool const ready = _model->contextReady();
            runtime.untracked([this, ready] {
                if (_options.contextReady) {
                    _options.contextReady(ready);
                }
            });
        });
    }

    ShellPartsBinding(ShellPartsBinding const&) = delete;
    ShellPartsBinding& operator=(ShellPartsBinding const&) = delete;
    ShellPartsBinding(ShellPartsBinding&&) = delete;
    ShellPartsBinding& operator=(ShellPartsBinding&&) = delete;

    /// @brief Stops following and takes the frame's handlers away.
    ~ShellPartsBinding() {
        _ready.reset();
        _counts.reset();
        _view.reset();
        _frame->setPartsHandlers({});
        _frame->setParts(ShellPartsView{});
        if (!_parts.modules.empty()) {
            Navigator& navigator = _frame->navigator();
            for (ModuleCount const& declared : _parts.modules) {
                navigator.setModuleBadge(declared.module, std::nullopt);
            }
            navigator.setModuleGroups(false);
        }
        _model.reset();
    }

    /// @brief The model.
    /// @return It.
    [[nodiscard]] ShellPartsModel& model() noexcept { return *_model; }

    /// @brief Tells the frame the client's connection state, which its foot draws.
    /// @param state The state.
    void setConnection(ConnectionState state) { _model->setConnection(state); }

    /// @brief Whether the switcher has answered, failed, or none is declared. A tracked read.
    /// @return Whether the start screen may mount.
    [[nodiscard]] bool contextReady() const { return _model->contextReady(); }

private:
    // Asks once whether to discard the unsaved work of @p tabs tabs.
    void ask(std::size_t tabs, std::function<void(bool)> done) {
        FormatContext const& format = _session->formattingNow();
        std::string message = shellText("morph.shell.contextDirty", format);
        if (auto const at = message.find("{n}"); at != std::string::npos) {
            message.replace(at, 3, std::to_string(tabs));
        }
        _frame->confirm(message, shellText("morph.shell.contextDiscard", format),
                        shellText("morph.shell.contextCancel", format), std::move(done));
    }

    // Puts each module's count on its navigator row, and the words its accessible name carries.
    void showCounts(std::vector<ShellPartsModel::ModuleState> const& counts, ShellLabels const& labels) {
        Navigator& navigator = _frame->navigator();
        FormatContext const& format = _session->formattingNow();
        navigator.setModuleTexts(shellText("morph.shell.moduleClosed", format),
                                 shellText("morph.shell.moduleOpen", format), labels.countFailed);
        std::set<std::string, std::less<>> shown;
        for (ShellPartsModel::ModuleState const& state : counts) {
            shown.insert(state.module);
            std::optional<NavigatorBadge> badge;
            if (state.count && *state.count > 0) {
                badge = NavigatorBadge{.text = shellCountText(*state.count), .tone = Tone::Neutral};
            }
            // The model leaves out every module the catalog does not list, so a module the navigator then does not
            // list means the two disagree about the catalog: reported, once per module.
            if (!navigator.setModuleBadge(state.module, badge, state.failed) && _options.model.onProblem &&
                _unlisted.insert(state.module).second) {
                _options.model.onProblem("the shell's module count names " + state.module +
                                         ", which the catalog lists and the navigator does not");
            }
        }
        for (ModuleCount const& declared : _parts.modules) {
            if (!shown.contains(declared.module)) {
                navigator.setModuleBadge(declared.module, std::nullopt);
            }
        }
    }

    Session* _session;
    ShellFrame* _frame;
    ShellParts _parts;
    ShellBindingOptions _options;
    std::unique_ptr<ShellPartsModel> _model;
    std::set<std::string, std::less<>> _unlisted;
    std::unique_ptr<reactive::Effect> _view;
    std::unique_ptr<reactive::Effect> _counts;
    std::unique_ptr<reactive::Effect> _ready;
};

}  // namespace morph::ui
