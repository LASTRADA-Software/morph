// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

namespace {

// A button of the chrome: its caption, emphasis and press, shown and enabled as bound.
Node chromeButton(std::string testId, std::string label, ButtonEmphasis emphasis, Action onClick,
                  Prop<bool> visible = Prop<bool>{true}, Prop<bool> enabled = Prop<bool>{true}) {
    Button button;
    button.label = Prop<std::string>{std::move(label)};
    button.emphasis = emphasis;
    button.onClick = std::move(onClick);
    button.common.testId = std::move(testId);
    button.common.visible = std::move(visible);
    button.common.enabled = std::move(enabled);
    return ui::button(std::move(button));
}

// What the refusal says: the line its pointer names in @p text, then the reason.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a refusal's pointer, then its reason.
std::string refusalLabel(std::string_view text, std::string const& pointer, std::string const& reason) {
    if (std::optional<std::size_t> const offset = pointerOffset(text, pointer)) {
        return "Line " + std::to_string(lineAt(text, *offset)) + ": " + reason;
    }
    return pointer.empty() ? reason : "At " + pointer + ": " + reason;
}

void finish(std::function<void(bool)> const& done, bool outcome) {
    if (done) {
        done(outcome);
    }
}

// An Open as entry as the menu shows it: its title, who saved it when not the person, and whether it is broken.
std::string choiceLabel(CustomScreenChoice const& choice) {
    std::string label = choice.title;
    if (!choice.mine) {
        label += " · by " + choice.savedBy;
    }
    if (choice.broken) {
        label += " · broken";
    }
    return label;
}

// ": " and @p detail, or nothing for an empty detail.
std::string because(std::string const& detail) { return detail.empty() ? std::string{} : ": " + detail; }

// The options, refused before anything is made of them.
CustomScreenChromeOptions checked(CustomScreenChromeOptions options) {
    if (options.runtime == nullptr || options.backend == nullptr || options.host == nullptr ||
        options.sync == nullptr) {
        throw std::invalid_argument{"morph::ui::CustomScreenChrome: a null runtime, backend, host or sync"};
    }
    if (!options.documents || !options.sourceOf || !options.catalog || !options.openTab) {
        throw std::invalid_argument{
            "morph::ui::CustomScreenChrome: documents, sourceOf, catalog and openTab are required"};
    }
    return options;
}

}  // namespace

std::string CustomScreenChrome::describe(CustomScreenOutcome const& outcome) {
    std::string saved = (outcome.save == CustomScreenSaveKind::SaveAs ? "Saved as " : "Saved ") + outcome.title;
    switch (outcome.event) {
        case CustomScreenEvent::Saved:
            return saved;
        case CustomScreenEvent::SavedTabClosed:
            return saved + ", but its tab was closed first.";
        case CustomScreenEvent::SavedDraftReplaced:
            return saved + ". The draft now open is a different one and is not saved.";
        case CustomScreenEvent::SavedDraftGone:
            return saved + ", and the draft it was saved from is gone.";
        case CustomScreenEvent::SavedNotOpened:
            return saved + ", but it could not be opened" + because(outcome.detail);
        case CustomScreenEvent::SavedNotReread:
            return saved + ", but the stored screen could not be read again" + because(outcome.detail);
        case CustomScreenEvent::SavedStaysDraft:
            return saved + ", but it stays a draft here until this screen can close" + because(outcome.detail);
        case CustomScreenEvent::SaveRefused:
            return "Not saved" + because(outcome.detail);
        case CustomScreenEvent::Removed:
            return "Removed " + outcome.title;
        case CustomScreenEvent::RemovedStaysOpen:
            return outcome.title + " is removed, but stays open in its tab until it can close" +
                   because(outcome.detail);
        case CustomScreenEvent::OpenAsCancelled:
            return "Open as cancelled: " + outcome.title + " stayed open";
        case CustomScreenEvent::OpenFailed:
            return "Cannot open " + outcome.title + because(outcome.detail);
        case CustomScreenEvent::SourceUnreadable:
            return "The source of " + outcome.title + " could not be read" + because(outcome.detail);
        case CustomScreenEvent::StoredSourceUnreadable:
            return "Cannot open the source of " + outcome.title + because(outcome.detail);
        case CustomScreenEvent::ProposalNotShown:
            return "The assistant's draft could not be shown" + because(outcome.detail);
        default:
            return outcome.title + because(outcome.detail);
    }
}

CustomScreenChrome::CustomScreenChrome(CustomScreenChromeOptions options)
    : _options{checked(std::move(options))},
      _controller{CustomScreenControllerOptions{
          .runtime = _options.runtime,
          .host = _options.host,
          .sync = _options.sync,
          .documents = _options.documents,
          .sourceOf = _options.sourceOf,
          .catalog = _options.catalog,
          .titleOf = _options.titleOf,
          .openTab = _options.openTab,
          .onOutcome = [this](CustomScreenOutcome const& outcome) { notify(describe(outcome), outcome.tone); },
          .onListChanged = _options.onListChanged,
      }} {}

CustomScreenChrome::~CustomScreenChrome() {
    *_alive = false;
    _remove.reset();
    _saveAs.reset();
    _source.reset();
}

void CustomScreenChrome::attach(WorkspaceModel* model) {
    _remove.reset();
    _saveAs.reset();
    _source.reset();
    _controller.attach(model);
}

void CustomScreenChrome::setOffered(bool offered) { _controller.setOffered(offered); }

void CustomScreenChrome::listed(CustomScreensListed const& listed) { _controller.listed(listed); }

Node CustomScreenChrome::chromeFor(WorkspaceTabId tab, std::string_view screenId) {
    std::string const custom{screenId};
    if (!isCustomScreenId(custom) && !_options.documents(custom)) {
        return nullptr;
    }
    _controller.follow(tab);
    Column column;
    column.gap = 1;
    column.common.testId = "screen_chrome_" + custom;
    column.children.push_back(draftBar(tab, custom));
    column.children.push_back(toolsRow(tab, custom));
    return ui::column(std::move(column));
}

Node CustomScreenChrome::draftBar(WorkspaceTabId tab, std::string const& screenId) {
    Row bar;
    bar.gap = 2;
    bar.wrap = true;
    bar.align = Align::Center;
    bar.common.testId = "draft_bar_" + screenId;
    bar.common.visible = Prop<bool>{[this, tab] { return _controller.drafted(tab); }};
    Badge badge;
    badge.text = Prop<std::string>{std::string{"Draft — not saved"}};
    badge.tone = Prop<Tone>{Tone::Warn};
    bar.children.push_back(ui::badge(std::move(badge)));
    Text note;
    note.text = Prop<std::string>{std::string{"Forms on a draft change real data."}};
    note.role = Prop<TextRole>{TextRole::Muted};
    note.common.testId = "draft_note_" + screenId;
    bar.children.push_back(ui::text(std::move(note)));
    Text refused;
    refused.text = Prop<std::string>{[this, tab] { return _controller.message(tab); }};
    refused.role = Prop<TextRole>{TextRole::Error};
    refused.common.testId = "draft_message_" + screenId;
    refused.common.visible = Prop<bool>{[this, tab] { return !_controller.message(tab).empty(); }};
    bar.children.push_back(ui::text(std::move(refused)));
    Prop<bool> const notSaving{[this, tab] { return !_controller.saving(tab); }};
    ActionBar actions;
    actions.children.push_back(
        chromeButton("draft_save_" + screenId, "Save", ButtonEmphasis::Primary, guarded([this, tab] { save(tab); }),
                     Prop<bool>{[this, screenId] { return _controller.canChange(screenId); }}, notSaving));
    actions.children.push_back(chromeButton("draft_save_as_" + screenId, "Save as…", ButtonEmphasis::Normal,
                                            guarded([this, tab] { askSaveAs(tab, false); }),
                                            Prop<bool>{[this] { return _controller.offered(); }}, notSaving));
    actions.children.push_back(chromeButton("draft_source_" + screenId, "Source", ButtonEmphasis::Normal,
                                            guarded([this, tab] { showSource(tab); })));
    actions.children.push_back(chromeButton("draft_discard_" + screenId, "Discard", ButtonEmphasis::Normal,
                                            guarded([this, tab] { discard(tab); })));
    bar.children.push_back(ui::actionBar(std::move(actions)));
    return ui::row(std::move(bar));
}

Node CustomScreenChrome::toolsRow(WorkspaceTabId tab, std::string const& screenId) {
    Row row;
    row.gap = 2;
    row.align = Align::Center;
    row.justify = Justify::End;
    row.common.testId = "screen_tools_" + screenId;
    row.common.visible = Prop<bool>{[this, tab] { return !_controller.drafted(tab); }};
    row.children.push_back(openAsMenu(tab, screenId));
    row.children.push_back(chromeButton("source_button_" + screenId, "Source", ButtonEmphasis::Ghost,
                                        guarded([this, tab] { showSource(tab); })));
    row.children.push_back(chromeButton("remove_" + screenId, "Remove", ButtonEmphasis::Ghost,
                                        guarded([this, tab] { askRemove(tab); }),
                                        Prop<bool>{[this, screenId] { return _controller.canChange(screenId); }}));
    return ui::row(std::move(row));
}

void CustomScreenChrome::showSource(WorkspaceTabId tab) {
    _controller.source(tab, guarded([this, tab](CustomScreenSource const& source) { openSource(source, tab); }));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one dialog, built in reading order.
void CustomScreenChrome::openSource(CustomScreenSource const& shown, std::optional<WorkspaceTabId> tab) {
    _source.reset();
    // A check still running for the view this one replaces answers nothing to it.
    _controller.dropSourceAnswers();
    auto made = std::make_unique<Source>();
    Source* const source = made.get();
    source->stored = shown.stored;
    source->tab = tab;
    source->screenId = shown.screenId;
    source->readOnly = shown.readOnly;
    source->opened = shown.text;
    source->text = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, shown.text);
    source->refusal = std::make_unique<reactive::Signal<std::optional<ShownRefusal>>>(*_options.runtime, std::nullopt);
    source->status = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    source->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    Prop<bool> const notBusy{[source] { return !source->busy->get(); }};

    Dialog dialog;
    dialog.open = Prop<bool>{true};
    // A sheet, so the editor uses the window: a document is read across, and a small dialog would give it a narrow
    // column.
    dialog.sheet = Prop<bool>{true};
    dialog.title = Prop<std::string>{"Source of " + _controller.titleOf(source->screenId)};
    if (source->readOnly) {
        dialog.description = Prop<std::string>{std::string{"Read only: this server does not store custom screens."}};
    }
    dialog.onDismiss = guarded([this] { closeSourceLater(); });
    dialog.common.testId = "source_dialog";

    Column body;
    body.gap = 2;
    TextInput input;
    input.value = Prop<std::string>{[source] { return source->text->get(); }};
    input.onChange = [source](std::string changed) {
        source->text->set(std::move(changed));
        source->refusal->set(std::nullopt);
    };
    input.mode = TextInputMode::Multiline;
    input.mono = true;
    // A document is read across: its lines stay whole and the editor scrolls sideways, which also costs less to lay
    // out than a wrapped text of this size.
    input.wrap = false;
    input.label = Prop<std::string>{std::string{"Document source"}};
    // Read only while a check runs, so the draft that mounts is exactly the text the check saw.
    input.field.readonly = Prop<bool>{[source] { return source->readOnly || source->busy->get(); }};
    input.common.testId = "source_text";
    input.common.focusId = "source_text";
    input.common.layout.width = Sizing::stretch();
    body.children.push_back(ui::textInput(std::move(input)));

    Button refusal;
    refusal.label = Prop<std::string>{[source] {
        std::optional<ShownRefusal> const& refused = source->refusal->get();
        return refused ? refusalLabel(refused->text, refused->pointer, refused->reason) : std::string{};
    }};
    refusal.emphasis = ButtonEmphasis::DangerOutline;
    refusal.onClick = guarded([this] { moveToRefusal(); });
    refusal.common.testId = "source_refusal";
    refusal.common.visible = Prop<bool>{[source] { return source->refusal->get().has_value(); }};
    body.children.push_back(ui::button(std::move(refusal)));

    Text status;
    status.text = Prop<std::string>{[source] { return source->status->get(); }};
    status.role = Prop<TextRole>{TextRole::Muted};
    status.common.testId = "source_status";
    status.common.visible = Prop<bool>{[source] { return !source->status->get().empty(); }};
    body.children.push_back(ui::text(std::move(status)));

    ActionBar actions;
    if (!source->readOnly && !source->stored) {
        actions.children.push_back(chromeButton("source_preview", "Preview", ButtonEmphasis::Primary,
                                                guarded([this] { preview(); }), Prop<bool>{true}, notBusy));
        actions.children.push_back(chromeButton("source_save_as", "Save as…", ButtonEmphasis::Normal,
                                                guarded([this, tab] {
                                                    if (tab) {
                                                        askSaveAs(*tab, true);
                                                    }
                                                }),
                                                Prop<bool>{true}, notBusy));
    }
    if (!source->readOnly && source->stored) {
        actions.children.push_back(chromeButton("source_save", "Check and save", ButtonEmphasis::Primary,
                                                guarded([this] { saveFromSource(); }), Prop<bool>{true}, notBusy));
    }
    actions.children.push_back(
        chromeButton("source_close", "Close", ButtonEmphasis::Normal, guarded([this] { closeSourceLater(); })));
    body.children.push_back(ui::actionBar(std::move(actions)));
    dialog.child = ui::column(std::move(body));

    _source = std::move(made);
    source->view.emplace(*_options.runtime, *_options.backend, ui::dialog(std::move(dialog)), _options.overlayParent);
    if (shown.broken) {
        refuse(shown.broken->pointer, shown.broken->reason);
    }
}

void CustomScreenChrome::refuse(std::string pointer, std::string reason) {
    if (Source* const source = _source.get()) {
        _options.runtime->batch([source, &pointer, &reason] {
            source->busy->set(false);
            source->status->set(std::string{});
            source->refusal->set(ShownRefusal{
                .pointer = std::move(pointer), .reason = std::move(reason), .text = source->text->peek()});
        });
    }
}

void CustomScreenChrome::moveToRefusal() {
    Source* const source = _source.get();
    if (source == nullptr || !source->view) {
        return;
    }
    std::optional<ShownRefusal> const& shown = source->refusal->peek();
    // A refusal about the document as a whole names no place to go to.
    if (!shown || shown->pointer.empty()) {
        return;
    }
    if (std::optional<std::size_t> const offset = pointerOffset(shown->text, shown->pointer)) {
        static_cast<void>(source->view->placeCursor("source_text", *offset));
    }
}

void CustomScreenChrome::closeSourceLater() {
    Source const* const shown = _source.get();
    // Posted: this runs inside a handler of the dialog it destroys.
    _options.runtime->owner().post(guarded([this, shown] {
        if (_source.get() == shown) {
            _source.reset();
            _controller.dropSourceAnswers();
        }
    }));
}

void CustomScreenChrome::closeSaveAsLater() {
    SaveAs const* const shown = _saveAs.get();
    _options.runtime->owner().post(guarded([this, shown] {
        if (_saveAs.get() == shown) {
            _saveAs.reset();
        }
    }));
}

void CustomScreenChrome::closeRemoveLater() {
    RemoveConfirm const* const shown = _remove.get();
    _options.runtime->owner().post(guarded([this, shown] {
        if (_remove.get() == shown) {
            _remove.reset();
        }
    }));
}

void CustomScreenChrome::preview(std::function<void(bool drafted)> const& done) {
    Source* const source = _source.get();
    if (source == nullptr || source->stored || source->readOnly || !source->tab || source->busy->peek()) {
        finish(done, false);
        return;
    }
    _options.runtime->batch([source] {
        source->busy->set(true);
        source->status->set(std::string{"Checking…"});
    });
    _controller.preview(*source->tab, source->text->peek(),
                        guarded([this, done](CustomScreenCheckResult const& result) {
                            switch (result.status) {
                                case CustomScreenCheckStatus::Done:
                                    closeSourceLater();
                                    finish(done, true);
                                    return;
                                case CustomScreenCheckStatus::Refused:
                                    refuse(result.refusal.pointer, result.refusal.reason);
                                    finish(done, false);
                                    return;
                                case CustomScreenCheckStatus::Dropped:
                                default:
                                    // The view that asked is gone, or the tab's draft changed: nothing here is the
                                    // answer's.
                                    finish(done, false);
                                    return;
                            }
                        }));
}

std::optional<std::string> CustomScreenChrome::draftOf(WorkspaceTabId tab) const { return _controller.draftOf(tab); }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen's id and the document's text are told apart by name.
bool CustomScreenChrome::proposeDraft(WorkspaceTabId tab, std::string const& screenId, std::string const& text) {
    if (!_controller.proposeDraft(tab, screenId, text)) {
        return false;
    }
    // A source view of this tab shows text the draft has replaced, and a check still running for it would mount over
    // the draft when it answers; its answers are dropped now. A view the person has edited stays open with those
    // edits, since closing it would throw them away unseen.
    if (Source* const source = _source.get(); source != nullptr && source->tab == tab) {
        _controller.dropSourceAnswers();
        if (source->text->peek() == source->opened) {
            closeSourceLater();
        } else {
            _options.runtime->batch([source] {
                source->busy->set(false);
                source->status->set(std::string{});
            });
            notify("The assistant's draft is mounted. Your edits are still in the source view.", Tone::Warn);
        }
    }
    return true;
}

void CustomScreenChrome::askSaveAs(WorkspaceTabId tab, bool fromSource) {
    // One save at a time: a dialog already saving is the one whose answer is awaited, and a tab with a save in flight
    // has its answer to wait for.
    if (!_controller.offered() || !_controller.screenOf(tab) || _controller.saving(tab) ||
        (_saveAs != nullptr && _saveAs->busy->peek())) {
        return;
    }
    _saveAs.reset();
    auto made = std::make_unique<SaveAs>();
    SaveAs* const dialog = made.get();
    dialog->tab = tab;
    dialog->fromSource = fromSource;
    dialog->title = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    dialog->message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    dialog->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);

    Dialog node;
    node.open = Prop<bool>{true};
    node.title = Prop<std::string>{std::string{"Save as"}};
    node.onDismiss = guarded([this] { closeSaveAsLater(); });
    node.common.testId = "save_as_dialog";
    Column body;
    body.gap = 2;
    TextInput input;
    input.value = Prop<std::string>{[dialog] { return dialog->title->get(); }};
    input.onChange = [dialog](std::string typed) { dialog->title->set(std::move(typed)); };
    input.onSubmit = guarded([this](std::string const& /*typed*/) { confirmSaveAs(); });
    input.label = Prop<std::string>{std::string{"Name"}};
    input.showLabel = true;
    input.common.testId = "save_as_title";
    input.common.autofocus = true;
    body.children.push_back(ui::textInput(std::move(input)));
    Text message;
    message.text = Prop<std::string>{[dialog] { return dialog->message->get(); }};
    message.role = Prop<TextRole>{TextRole::Error};
    message.common.testId = "save_as_message";
    message.common.visible = Prop<bool>{[dialog] { return !dialog->message->get().empty(); }};
    body.children.push_back(ui::text(std::move(message)));
    ActionBar actions;
    actions.children.push_back(chromeButton("save_as_confirm", "Save", ButtonEmphasis::Primary,
                                            guarded([this] { confirmSaveAs(); }), Prop<bool>{true},
                                            Prop<bool>{[dialog] { return !dialog->busy->get(); }}));
    actions.children.push_back(
        chromeButton("save_as_cancel", "Cancel", ButtonEmphasis::Normal, guarded([this] { closeSaveAsLater(); })));
    body.children.push_back(ui::actionBar(std::move(actions)));
    node.child = ui::column(std::move(body));

    _saveAs = std::move(made);
    dialog->view.emplace(*_options.runtime, *_options.backend, ui::dialog(std::move(node)), _options.overlayParent);
}

void CustomScreenChrome::confirmSaveAs() {
    SaveAs* const dialog = _saveAs.get();
    if (dialog == nullptr || dialog->busy->peek()) {
        return;
    }
    std::string const title = dialog->title->peek();
    if (title.empty()) {
        dialog->message->set(std::string{"Give the screen a name."});
        return;
    }
    _options.runtime->batch([dialog] {
        dialog->busy->set(true);
        dialog->message->set(std::string{});
    });
    WorkspaceTabId const tab = dialog->tab;
    auto const saveDraft = [this, tab, title] {
        _controller.saveAs(tab, title, std::nullopt,
                           guarded([this, tab](std::expected<void, std::string> const& saved) {
                               if (!saved) {
                                   failSaveAs(tab, saved.error());
                                   return;
                               }
                               closeSaveAsLater();
                           }));
    };
    if (!dialog->fromSource) {
        saveDraft();
        return;
    }
    preview(guarded([this, saveDraft](bool drafted) {
        if (drafted) {
            saveDraft();
            return;
        }
        if (SaveAs* const shown = _saveAs.get()) {
            _options.runtime->batch([shown] {
                shown->busy->set(false);
                shown->message->set(std::string{"Not saved: the source view shows why."});
            });
        }
    }));
}

void CustomScreenChrome::failSaveAs(WorkspaceTabId tab, std::string const& reason) {
    if (SaveAs* const dialog = _saveAs.get(); dialog != nullptr && dialog->tab == tab) {
        _options.runtime->batch([dialog, &reason] {
            dialog->busy->set(false);
            dialog->message->set(reason);
        });
    }
}

void CustomScreenChrome::save(WorkspaceTabId tab) { _controller.save(tab); }

void CustomScreenChrome::discard(WorkspaceTabId tab) { _controller.discard(tab); }

Node CustomScreenChrome::openAsMenu(WorkspaceTabId tab, std::string const& screenId) {
    return ui::forEach<OpenAsShape>(
        [this, screenId] {
            OpenAsShape shape{.key = {}, .choices = _controller.openAsChoices(screenId)};
            if (shape.choices.empty()) {
                return std::vector<OpenAsShape>{};
            }
            for (CustomScreenChoice const& choice : shape.choices) {
                shape.key += choice.id + '\x1f' + choiceLabel(choice) + (choice.current ? "\x1f*" : "") + '\x1e';
            }
            return std::vector<OpenAsShape>{std::move(shape)};
        },
        [](OpenAsShape const& shape) { return Key{shape.key}; },
        [this, tab, screenId](reactive::Signal<OpenAsShape> const& shape) {
            Menu menu;
            menu.style = MenuStyle::Button;
            menu.label = Prop<std::string>{std::string{"Open as"}};
            menu.common.testId = "open_as_" + screenId;
            for (CustomScreenChoice const& choice : shape.peek().choices) {
                MenuItem item;
                item.label = Prop<std::string>{choiceLabel(choice)};
                item.checked = Prop<std::optional<bool>>{std::optional<bool>{choice.current}};
                item.onSelect = guarded([this, tab, target = choice.id] { openAs(tab, target); });
                menu.items.push_back(std::move(item));
            }
            return ui::menu(std::move(menu));
        },
        Axis::Horizontal, 0);
}

void CustomScreenChrome::openAs(WorkspaceTabId tab, std::string const& screenId) {
    std::optional<std::string> const shown = _controller.screenOf(tab);
    if (!shown || *shown == screenId) {
        return;
    }
    // A broken screen is never mounted: its source opens instead, and the tab stays as it is.
    if (_controller.isBroken(screenId)) {
        showStoredSource(screenId);
        return;
    }
    _controller.openAs(tab, screenId);
}

std::vector<NavigatorCustomEntry> CustomScreenChrome::navigatorEntries() const {
    return _controller.navigatorEntries();
}

bool CustomScreenChrome::isBroken(std::string_view screenId) const { return _controller.isBroken(screenId); }

void CustomScreenChrome::openCustom(std::string const& screenId, ParamMap params,
                                    std::function<void(std::expected<void, std::string>)> const& done) {
    if (_controller.isBroken(screenId)) {
        // Only the source view opens: the screen itself is never mounted, which the caller is told.
        showStoredSource(screenId);
        if (done) {
            done(std::unexpected(_controller.titleOf(screenId) + " is broken; its source is open"));
        }
        return;
    }
    _controller.openCustom(screenId, std::move(params), done);
}

void CustomScreenChrome::showStoredSource(std::string const& screenId) {
    _controller.storedSource(screenId,
                             guarded([this](CustomScreenSource const& source) { openSource(source, std::nullopt); }));
}

void CustomScreenChrome::saveFromSource() {
    Source* const source = _source.get();
    if (source == nullptr || !source->stored || source->readOnly || source->busy->peek()) {
        return;
    }
    _options.runtime->batch([source] {
        source->busy->set(true);
        source->status->set(std::string{"Checking…"});
    });
    _controller.checkAndSave(source->screenId, source->text->peek(), guarded([this] {
                                 // The check passed for this view: a view opened since would have dropped its answer.
                                 if (_source != nullptr) {
                                     _source->status->set(std::string{"Saving…"});
                                 }
                             }),
                             guarded([this](CustomScreenCheckResult const& result) {
                                 switch (result.status) {
                                     case CustomScreenCheckStatus::Done:
                                         closeSourceLater();
                                         return;
                                     case CustomScreenCheckStatus::Refused:
                                         refuse(result.refusal.pointer, result.refusal.reason);
                                         return;
                                     case CustomScreenCheckStatus::Dropped:
                                     default:
                                         return;
                                 }
                             }));
}

void CustomScreenChrome::askRemove(WorkspaceTabId tab) {
    std::optional<std::string> const screenId = _controller.screenOf(tab);
    // One removal at a time: a confirmation already removing is the one whose answer is awaited.
    if (!screenId || !_controller.canChange(*screenId) || (_remove != nullptr && _remove->busy->peek())) {
        return;
    }
    // `canChange` held, so the screen is listed and the default is never read.
    CustomScreenSummary const summary = _controller.summaryOf(*screenId).value_or(CustomScreenSummary{});
    _remove.reset();
    auto made = std::make_unique<RemoveConfirm>();
    RemoveConfirm* const confirm = made.get();
    confirm->generation = ++_lastRemove;
    confirm->screenId = *screenId;
    confirm->message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    confirm->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);

    Dialog node;
    node.open = Prop<bool>{true};
    node.title = Prop<std::string>{"Remove " + summary.title + "?"};
    node.description = Prop<std::string>{"It is removed for good. A tab that shows it opens " +
                                         _controller.titleOf(summary.basedOn) + " instead."};
    node.onDismiss = guarded([this] { closeRemoveLater(); });
    node.common.testId = "remove_dialog";
    Column body;
    body.gap = 2;
    Text message;
    message.text = Prop<std::string>{[confirm] { return confirm->message->get(); }};
    message.role = Prop<TextRole>{TextRole::Error};
    message.common.testId = "remove_message";
    message.common.visible = Prop<bool>{[confirm] { return !confirm->message->get().empty(); }};
    body.children.push_back(ui::text(std::move(message)));
    ActionBar actions;
    actions.children.push_back(chromeButton("remove_confirm", "Remove", ButtonEmphasis::Danger,
                                            guarded([this] { confirmRemove(); }), Prop<bool>{true},
                                            Prop<bool>{[confirm] { return !confirm->busy->get(); }}));
    actions.children.push_back(
        chromeButton("remove_cancel", "Cancel", ButtonEmphasis::Normal, guarded([this] { closeRemoveLater(); })));
    body.children.push_back(ui::actionBar(std::move(actions)));
    node.child = ui::column(std::move(body));

    _remove = std::move(made);
    confirm->view.emplace(*_options.runtime, *_options.backend, ui::dialog(std::move(node)), _options.overlayParent);
}

void CustomScreenChrome::confirmRemove() {
    RemoveConfirm* const confirm = _remove.get();
    if (confirm == nullptr || confirm->busy->peek()) {
        return;
    }
    _options.runtime->batch([confirm] {
        confirm->busy->set(true);
        confirm->message->set(std::string{});
    });
    _controller.remove(
        confirm->screenId,
        guarded([this, generation = confirm->generation](std::expected<void, std::string> const& removed) {
            // The confirmation the removal was asked from, and not one opened since, takes the answer.
            RemoveConfirm* const open =
                _remove != nullptr && _remove->generation == generation ? _remove.get() : nullptr;
            if (removed) {
                if (open != nullptr) {
                    closeRemoveLater();
                }
                return;
            }
            if (open == nullptr) {
                notify("Not removed: " + removed.error(), Tone::Err);
                return;
            }
            _options.runtime->batch([open, &removed] {
                open->busy->set(false);
                open->message->set(removed.error());
            });
        }));
}

void CustomScreenChrome::notify(std::string const& text, Tone tone) const {
    if (_options.notify) {
        _options.notify(text, tone);
    }
}

}  // namespace morph::ui
