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

}  // namespace

CustomScreenChrome::CustomScreenChrome(CustomScreenChromeOptions options) : _options{std::move(options)} {
    if (_options.runtime == nullptr || _options.backend == nullptr || _options.host == nullptr ||
        _options.sync == nullptr) {
        throw std::invalid_argument{"morph::ui::CustomScreenChrome: a null runtime, backend, host or sync"};
    }
    if (!_options.documents || !_options.sourceOf || !_options.catalog || !_options.openTab) {
        throw std::invalid_argument{
            "morph::ui::CustomScreenChrome: documents, sourceOf, catalog and openTab are required"};
    }
    _offered = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    _listVersion = std::make_unique<reactive::Signal<std::uint64_t>>(*_options.runtime, 0);
}

CustomScreenChrome::~CustomScreenChrome() {
    *_alive = false;
    _saveAs.reset();
    _source.reset();
}

void CustomScreenChrome::attach(WorkspaceModel* model) {
    _saveAs.reset();
    _source.reset();
    _model = model;
    // The rows of the old workspace's tabs read this state; they are gone once a new workspace is attached.
    if (model != nullptr) {
        _tabs.clear();
    }
}

void CustomScreenChrome::setOffered(bool offered) {
    _options.runtime->batch([this, offered] { _offered->set(offered); });
    if (_options.onListChanged) {
        _options.onListChanged();
    }
}

void CustomScreenChrome::listed(CustomScreensListed const& listed) {
    _listed = listed;
    _options.runtime->batch([this] {
        _offered->set(true);
        _listVersion->set(_listVersion->peek() + 1);
    });
    if (_options.onListChanged) {
        _options.onListChanged();
    }
}

Node CustomScreenChrome::chromeFor(WorkspaceTabId tab, std::string_view screenId) {
    std::string const id{screenId};
    if (!isCustomScreenId(id) && !_options.documents(id)) {
        return nullptr;
    }
    TabState& state = stateOf(tab);
    Column column;
    column.gap = 1;
    column.common.testId = "screen_chrome_" + id;
    column.children.push_back(draftBar(tab, id, state));
    column.children.push_back(toolsRow(tab, id));
    return ui::column(std::move(column));
}

Node CustomScreenChrome::draftBar(WorkspaceTabId tab, std::string const& screenId, TabState& state) {
    Row bar;
    bar.gap = 2;
    bar.wrap = true;
    bar.align = Align::Center;
    bar.common.testId = "draft_bar_" + screenId;
    bar.common.visible = Prop<bool>{[this, tab] { return draftedTracked(tab); }};
    Badge badge;
    badge.text = Prop<std::string>{std::string{"Draft — not saved"}};
    badge.tone = Prop<Tone>{Tone::Warn};
    bar.children.push_back(ui::badge(std::move(badge)));
    Text note;
    note.text = Prop<std::string>{std::string{"Forms on a draft change real data."}};
    note.role = Prop<TextRole>{TextRole::Muted};
    note.common.testId = "draft_note_" + screenId;
    bar.children.push_back(ui::text(std::move(note)));
    reactive::Signal<std::string> const* const message = state.message.get();
    Text refused;
    refused.text = Prop<std::string>{[message] { return message->get(); }};
    refused.role = Prop<TextRole>{TextRole::Error};
    refused.common.testId = "draft_message_" + screenId;
    refused.common.visible = Prop<bool>{[message] { return !message->get().empty(); }};
    bar.children.push_back(ui::text(std::move(refused)));
    ActionBar actions;
    actions.children.push_back(chromeButton("draft_save_" + screenId, "Save", ButtonEmphasis::Primary,
                                            guarded([this, tab] { save(tab); }),
                                            Prop<bool>{[this, screenId] { return canSave(screenId); }}));
    actions.children.push_back(chromeButton("draft_save_as_" + screenId, "Save as…", ButtonEmphasis::Normal,
                                            guarded([this, tab] { askSaveAs(tab, false); }),
                                            Prop<bool>{[this] { return _offered->get(); }}));
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
    row.common.visible = Prop<bool>{[this, tab] { return !draftedTracked(tab); }};
    row.children.push_back(chromeButton("source_button_" + screenId, "Source", ButtonEmphasis::Ghost,
                                        guarded([this, tab] { showSource(tab); })));
    return ui::row(std::move(row));
}

CustomScreenChrome::TabState& CustomScreenChrome::stateOf(WorkspaceTabId tab) {
    auto [found, inserted] = _tabs.try_emplace(tab.value);
    if (inserted) {
        found->second.message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    }
    return found->second;
}

ScreenHandle* CustomScreenChrome::handleOf(WorkspaceTabId tab) const {
    return _model == nullptr ? nullptr : _model->handleOf(tab);
}

ScreenHandle* CustomScreenChrome::trackedHandleOf(WorkspaceTabId tab) const {
    if (_model == nullptr) {
        return nullptr;
    }
    static_cast<void>(_model->tabs().get());
    return _model->handleOf(tab);
}

bool CustomScreenChrome::draftedTracked(WorkspaceTabId tab) const {
    ScreenHandle const* const handle = trackedHandleOf(tab);
    return handle != nullptr && DocumentScreenHost::drafted(*handle);
}

bool CustomScreenChrome::canSave(std::string const& screenId) const {
    static_cast<void>(_listVersion->get());
    if (!_offered->get() || !isCustomScreenId(screenId)) {
        return false;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary.has_value() && summary->mine;
}

std::optional<CustomScreenSummary> CustomScreenChrome::summaryOf(std::string_view screenId) const {
    for (CustomScreenSummary const& screen : _listed.screens) {
        if (screen.id == screenId) {
            return screen;
        }
    }
    if (CustomDocuments::Entry const* const entry = _options.sync->documents().entry(screenId)) {
        return entry->summary;
    }
    return std::nullopt;
}

std::string CustomScreenChrome::baseOf(std::string const& screenId) const {
    if (!isCustomScreenId(screenId)) {
        return screenId;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary ? summary->basedOn : std::string{};
}

std::string CustomScreenChrome::currentText(std::string const& screenId) const {
    if (!isCustomScreenId(screenId)) {
        return _options.sourceOf(screenId);
    }
    CustomDocuments::Entry const* const entry = _options.sync->documents().entry(screenId);
    return entry == nullptr ? std::string{} : entry->text;
}

std::string CustomScreenChrome::titleOf(std::string const& screenId) const {
    if (isCustomScreenId(screenId)) {
        std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
        return summary ? summary->title : screenId;
    }
    return _options.titleOf ? _options.titleOf(screenId) : screenId;
}

void CustomScreenChrome::showSource(WorkspaceTabId tab) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId) {
        return;
    }
    std::string const text = DocumentScreenHost::drafted(*handle) ? stateOf(tab).draftText : currentText(*screenId);
    openSource(SourceMode::Draft, tab, *screenId, prettyJson(text), !_offered->peek());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one dialog, built in reading order.
void CustomScreenChrome::openSource(SourceMode mode, std::optional<WorkspaceTabId> tab, std::string screenId,
                                    std::string text, bool readOnly) {
    _source.reset();
    auto made = std::make_unique<Source>();
    Source* const source = made.get();
    source->mode = mode;
    source->tab = tab;
    source->screenId = std::move(screenId);
    source->readOnly = readOnly;
    source->text = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::move(text));
    source->refusal = std::make_unique<reactive::Signal<std::optional<ShownRefusal>>>(*_options.runtime, std::nullopt);
    source->status = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    source->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    Prop<bool> const notBusy{[source] { return !source->busy->get(); }};

    Dialog dialog;
    dialog.open = Prop<bool>{true};
    dialog.title = Prop<std::string>{"Source of " + titleOf(source->screenId)};
    if (readOnly) {
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
    input.label = Prop<std::string>{std::string{"Document source"}};
    input.field.readonly = Prop<bool>{readOnly};
    input.common.testId = "source_text";
    input.common.focusId = "source_text";
    input.common.layout.width = Sizing::stretch();
    body.children.push_back(ui::textInput(std::move(input)));

    Button refusal;
    refusal.label = Prop<std::string>{[source] {
        std::optional<ShownRefusal> const& shown = source->refusal->get();
        return shown ? refusalLabel(source->text->peek(), shown->pointer, shown->reason) : std::string{};
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
    if (!readOnly && mode == SourceMode::Draft) {
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
    actions.children.push_back(
        chromeButton("source_close", "Close", ButtonEmphasis::Normal, guarded([this] { closeSourceLater(); })));
    body.children.push_back(ui::actionBar(std::move(actions)));
    dialog.child = ui::column(std::move(body));

    _source = std::move(made);
    source->view.emplace(*_options.runtime, *_options.backend, ui::dialog(std::move(dialog)), _options.overlayParent);
}

void CustomScreenChrome::refuse(std::string pointer, std::string reason) {
    if (Source* const source = _source.get()) {
        _options.runtime->batch([source, &pointer, &reason] {
            source->busy->set(false);
            source->status->set(std::string{});
            source->refusal->set(ShownRefusal{.pointer = std::move(pointer), .reason = std::move(reason)});
        });
    }
}

void CustomScreenChrome::moveToRefusal() {
    Source* const source = _source.get();
    if (source == nullptr || !source->view) {
        return;
    }
    std::optional<ShownRefusal> const& shown = source->refusal->peek();
    if (!shown) {
        return;
    }
    std::size_t const offset = pointerOffset(source->text->peek(), shown->pointer).value_or(0);
    static_cast<void>(source->view->placeCursor("source_text", offset));
}

void CustomScreenChrome::closeSourceLater() {
    Source const* const shown = _source.get();
    // Posted: this runs inside a handler of the dialog it destroys.
    _options.runtime->owner().post(guarded([this, shown] {
        if (_source.get() == shown) {
            _source.reset();
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the client's load, the server's check, then the mount.
void CustomScreenChrome::preview(std::function<void(bool drafted)> done) {
    Source* const source = _source.get();
    if (source == nullptr || source->mode != SourceMode::Draft || source->readOnly || !source->tab) {
        finish(done, false);
        return;
    }
    WorkspaceTabId const tab = *source->tab;
    std::string const text = source->text->peek();
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = _options.catalog()});
    if (!loaded) {
        refuse(loaded.error().path, loaded.error().reason);
        finish(done, false);
        return;
    }
    if (loaded->id != source->screenId) {
        refuse("/id", "a draft keeps the screen's id, " + source->screenId);
        finish(done, false);
        return;
    }
    std::string const basedOn = baseOf(source->screenId);
    if (basedOn.empty()) {
        refuse("", "the base of " + source->screenId + " is not known");
        finish(done, false);
        return;
    }
    auto const document = std::make_shared<Document const>(std::move(*loaded));
    _options.runtime->batch([source] {
        source->busy->set(true);
        source->status->set(std::string{"Checking…"});
    });
    _options.sync->check(
        basedOn, text,
        guarded([this, tab, text, document, done](std::expected<std::optional<CheckRefusal>, std::string> checked) {
            Source* const shown = _source.get();
            if (shown == nullptr || !shown->tab || shown->tab->value != tab.value) {
                finish(done, false);
                return;
            }
            if (!checked) {
                refuse("", checked.error());
                finish(done, false);
                return;
            }
            if (*checked) {
                refuse((*checked)->pointer, (*checked)->reason);
                finish(done, false);
                return;
            }
            ScreenHandle* const handle = handleOf(tab);
            if (handle == nullptr) {
                refuse("", "the screen is closed");
                finish(done, false);
                return;
            }
            if (auto mounted = _options.host->mountDraft(*handle, document); !mounted) {
                refuse("", mounted.error().reason);
                finish(done, false);
                return;
            }
            TabState& state = stateOf(tab);
            state.draftText = text;
            state.message->set(std::string{});
            closeSourceLater();
            finish(done, true);
        }));
}

void CustomScreenChrome::askSaveAs(WorkspaceTabId tab, bool fromSource) {
    if (!_offered->peek()) {
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
    if (!dialog->fromSource) {
        saveAs(tab, title);
        return;
    }
    preview(guarded([this, tab, title](bool drafted) {
        if (drafted) {
            saveAs(tab, title);
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

void CustomScreenChrome::saveAs(WorkspaceTabId tab, std::string const& title) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId) {
        failSaveAs(tab, "the screen is closed");
        return;
    }
    std::string const base = baseOf(*screenId);
    std::string const text = DocumentScreenHost::drafted(*handle) ? stateOf(tab).draftText : currentText(*screenId);
    if (base.empty() || text.empty()) {
        failSaveAs(tab, "the screen's base or document is not known");
        return;
    }
    _options.sync->save(
        CustomScreensSave{.app = {}, .id = std::nullopt, .title = title, .basedOn = base, .document = text},
        guarded([this, tab, title](std::expected<std::string, std::string> saved) {
            if (!saved) {
                failSaveAs(tab, saved.error());
                return;
            }
            closeSaveAsLater();
            openSaved(tab, *saved, title);
        }));
}

void CustomScreenChrome::failSaveAs(WorkspaceTabId tab, std::string const& reason) {
    stateOf(tab).message->set(reason);
    if (SaveAs* const dialog = _saveAs.get()) {
        _options.runtime->batch([dialog, &reason] {
            dialog->busy->set(false);
            dialog->message->set(reason);
        });
    }
}

void CustomScreenChrome::openSaved(WorkspaceTabId tab, std::string const& id, std::string const& title) {
    _options.sync->open(
        id, guarded([this, tab, id, title](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            ScreenHandle* const handle = handleOf(tab);
            if (!loaded || handle == nullptr || _model == nullptr) {
                notify("Saved as " + title + ", but it could not be opened" +
                           (loaded ? std::string{} : ": " + loaded.error()),
                       Tone::Warn);
                return;
            }
            _options.host->endDraft(*handle);
            stateOf(tab).draftText.clear();
            ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
            _model->openInPlace(
                tab, id, params, guarded([this, title](std::expected<WorkspaceTabId, MountError> opened) {
                    if (!opened) {
                        notify("Saved as " + title + ", but it could not be opened: " + opened.error().reason,
                               Tone::Warn);
                        return;
                    }
                    notify("Saved as " + title, Tone::Ok);
                }));
        }));
}

void CustomScreenChrome::save(WorkspaceTabId tab) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || !DocumentScreenHost::drafted(*handle) || !canSave(*screenId)) {
        return;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(*screenId);
    std::string const id = *screenId;
    _options.sync->save(
        CustomScreensSave{.app = {},
                          .id = id,
                          .title = summary->title,
                          .basedOn = summary->basedOn,
                          .document = stateOf(tab).draftText},
        guarded([this, tab, id](std::expected<std::string, std::string> saved) {
            if (!saved) {
                stateOf(tab).message->set(saved.error());
                return;
            }
            _options.sync->fetch(
                id, guarded([this, tab](std::expected<std::shared_ptr<Document const>, std::string> const& loaded) {
                    if (ScreenHandle* const shown = handleOf(tab)) {
                        _options.host->endDraft(*shown);
                    }
                    TabState& state = stateOf(tab);
                    state.draftText.clear();
                    state.message->set(std::string{});
                    if (!loaded) {
                        notify("Saved, but the stored screen could not be read again: " + loaded.error(), Tone::Warn);
                    }
                }));
        }));
}

void CustomScreenChrome::discard(WorkspaceTabId tab) {
    ScreenHandle* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || !DocumentScreenHost::drafted(*handle)) {
        return;
    }
    auto const remount = [this, tab] {
        ScreenHandle* const shown = handleOf(tab);
        if (shown == nullptr) {
            return;
        }
        TabState& state = stateOf(tab);
        if (auto discarded = _options.host->discardDraft(*shown); !discarded) {
            state.message->set(discarded.error().reason);
            return;
        }
        state.draftText.clear();
        state.message->set(std::string{});
    };
    if (isCustomScreenId(*screenId) && _options.sync->documents().find(*screenId) == nullptr) {
        _options.sync->open(
            *screenId,
            guarded([this, tab, remount](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
                if (!loaded) {
                    stateOf(tab).message->set(loaded.error());
                    return;
                }
                remount();
            }));
        return;
    }
    // Posted: the press that asked runs in a handler, and a remount must not run inside one.
    _options.runtime->owner().post(guarded(remount));
}

void CustomScreenChrome::notify(std::string const& text, Tone tone) const {
    if (_options.notify) {
        _options.notify(text, tone);
    }
}

}  // namespace morph::ui
