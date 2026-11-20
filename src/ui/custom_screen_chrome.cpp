// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
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
    _remove.reset();
    _saveAs.reset();
    _source.reset();
}

void CustomScreenChrome::attach(WorkspaceModel* model) {
    _remove.reset();
    _saveAs.reset();
    _source.reset();
    // Tab ids are issued per workspace: a reply asked under the old one must not find a tab of the new one by id.
    if (model != _model) {
        ++_epoch;
    }
    _model = model;
    // The old workspace's rows keep the state they read alive themselves.
    _tabs.clear();
    // The list belongs to the frame that is gone: the new session's list arrives with its own sync.
    if (model == nullptr) {
        _listed = CustomScreensListed{};
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
    std::string const custom{screenId};
    if (!isCustomScreenId(custom) && !_options.documents(custom)) {
        return nullptr;
    }
    std::shared_ptr<TabState> const state = ensureState(tab);
    prune();
    Column column;
    column.gap = 1;
    column.common.testId = "screen_chrome_" + custom;
    column.children.push_back(draftBar(tab, custom, state));
    column.children.push_back(toolsRow(tab, custom));
    return ui::column(std::move(column));
}

Node CustomScreenChrome::draftBar(WorkspaceTabId tab, std::string const& screenId,
                                  std::shared_ptr<TabState> const& state) {
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
    Text refused;
    refused.text = Prop<std::string>{[state] { return state->message->get(); }};
    refused.role = Prop<TextRole>{TextRole::Error};
    refused.common.testId = "draft_message_" + screenId;
    refused.common.visible = Prop<bool>{[state] { return !state->message->get().empty(); }};
    bar.children.push_back(ui::text(std::move(refused)));
    ActionBar actions;
    actions.children.push_back(chromeButton("draft_save_" + screenId, "Save", ButtonEmphasis::Primary,
                                            guarded([this, tab] { save(tab); }),
                                            Prop<bool>{[this, screenId] { return canSave(screenId); }},
                                            Prop<bool>{[state] { return !state->pending->get(); }}));
    actions.children.push_back(chromeButton("draft_save_as_" + screenId, "Save as…", ButtonEmphasis::Normal,
                                            guarded([this, tab] { askSaveAs(tab, false); }),
                                            Prop<bool>{[this] { return _offered->get(); }},
                                            Prop<bool>{[state] { return !state->pending->get(); }}));
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
    row.children.push_back(openAsMenu(tab, screenId));
    row.children.push_back(chromeButton("source_button_" + screenId, "Source", ButtonEmphasis::Ghost,
                                        guarded([this, tab] { showSource(tab); })));
    row.children.push_back(chromeButton("remove_" + screenId, "Remove", ButtonEmphasis::Ghost,
                                        guarded([this, tab] { askRemove(tab); }),
                                        Prop<bool>{[this, screenId] { return canSave(screenId); }}));
    return ui::row(std::move(row));
}

std::shared_ptr<CustomScreenChrome::TabState> CustomScreenChrome::ensureState(WorkspaceTabId tab) {
    std::shared_ptr<TabState>& slot = _tabs[tab.value];
    if (slot == nullptr) {
        slot = std::make_shared<TabState>();
        slot->message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
        slot->pending = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    }
    return slot;
}

std::shared_ptr<CustomScreenChrome::TabState> CustomScreenChrome::stateOf(WorkspaceTabId tab) const {
    auto const found = _tabs.find(tab.value);
    return found == _tabs.end() ? nullptr : found->second;
}

void CustomScreenChrome::prune() {
    for (auto shown = _tabs.begin(); shown != _tabs.end();) {
        TabState& state = *shown->second;
        bool const held = handleOf(WorkspaceTabId{shown->first}) != nullptr;
        if (held) {
            state.seen = true;
        }
        // A state made while its tab was opening has not been seen with a handle yet, and one with a save in flight
        // is still read by the reply.
        if (!held && state.seen && !state.pending->peek()) {
            shown = _tabs.erase(shown);
        } else {
            ++shown;
        }
    }
}

void CustomScreenChrome::settled(WorkspaceTabId tab) {
    if (std::shared_ptr<TabState> const state = stateOf(tab)) {
        state->pending->set(false);
    }
    prune();
}

bool CustomScreenChrome::holds(WorkspaceTabId tab, std::uint64_t generation) const {
    std::shared_ptr<TabState> const state = stateOf(tab);
    return state != nullptr && handleOf(tab) != nullptr && state->generation == generation;
}

void CustomScreenChrome::saveOutcome(WorkspaceTabId tab, std::uint64_t generation, std::string const& saved) {
    if (holds(tab, generation)) {
        notify(saved, Tone::Ok);
        return;
    }
    ScreenHandle const* const handle = handleOf(tab);
    if (handle == nullptr) {
        notify(saved + ", but its tab was closed first.", Tone::Warn);
    } else if (DocumentScreenHost::drafted(*handle)) {
        notify(saved + ". The draft now open is a different one and is not saved.", Tone::Warn);
    } else {
        notify(saved + ", and the draft it was saved from is gone.", Tone::Warn);
    }
}

void CustomScreenChrome::tell(WorkspaceTabId tab, std::string text) {
    if (std::shared_ptr<TabState> const state = stateOf(tab)) {
        state->message->set(std::move(text));
    }
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
    ScreenHandle const* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || state == nullptr) {
        return;
    }
    bool const drafted = DocumentScreenHost::drafted(*handle);
    if (!drafted && isCustomScreenId(*screenId) && _options.sync->documents().entry(*screenId) == nullptr) {
        // The cache dropped the document (a list that no longer has it): it is fetched again before it is shown.
        _options.sync->open(*screenId,
                            guarded([this, tab, stored = *screenId](
                                        std::expected<std::shared_ptr<Document const>, std::string> loaded) {
                                if (loaded && _options.sync->documents().entry(stored) != nullptr) {
                                    showSource(tab);
                                    return;
                                }
                                notify("The source of " + titleOf(stored) + " could not be read" +
                                           (loaded ? std::string{} : ": " + loaded.error()),
                                       Tone::Warn);
                            }));
        return;
    }
    std::string const text = drafted ? state->draftText : currentText(*screenId);
    openSource(SourceMode::Draft, tab, *screenId, prettyJson(text), !_offered->peek());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one dialog, built in reading order.
void CustomScreenChrome::openSource(SourceMode mode, std::optional<WorkspaceTabId> tab, std::string screenId,
                                    std::string text, bool readOnly) {
    _source.reset();
    auto made = std::make_unique<Source>();
    Source* const source = made.get();
    source->mode = mode;
    source->generation = ++_lastSource;
    source->tab = tab;
    source->screenId = std::move(screenId);
    source->readOnly = readOnly;
    source->opened = text;
    source->text = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::move(text));
    source->refusal = std::make_unique<reactive::Signal<std::optional<ShownRefusal>>>(*_options.runtime, std::nullopt);
    source->status = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    source->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    Prop<bool> const notBusy{[source] { return !source->busy->get(); }};

    Dialog dialog;
    dialog.open = Prop<bool>{true};
    // A sheet, so the editor uses the window: a document is read across, and a small dialog would give it a narrow
    // column.
    dialog.sheet = Prop<bool>{true};
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
        std::optional<ShownRefusal> const& shown = source->refusal->get();
        return shown ? refusalLabel(shown->text, shown->pointer, shown->reason) : std::string{};
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
    if (!readOnly && mode == SourceMode::Stored) {
        actions.children.push_back(chromeButton("source_save", "Check and save", ButtonEmphasis::Primary,
                                                guarded([this] { saveFromSource(); }), Prop<bool>{true}, notBusy));
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
    if (!shown) {
        return;
    }
    // A refusal about the document as a whole names no place to go to.
    if (shown->pointer.empty()) {
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
void CustomScreenChrome::preview(std::function<void(bool drafted)> const& done) {
    Source* const source = _source.get();
    if (source == nullptr || source->mode != SourceMode::Draft || source->readOnly || !source->tab ||
        source->busy->peek()) {
        finish(done, false);
        return;
    }
    WorkspaceTabId const tab = *source->tab;
    std::uint64_t const generation = source->generation;
    std::string const text = source->text->peek();
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = _options.catalog()});
    if (!loaded) {
        refuse(loaded.error().path, loaded.error().reason);
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
    _options.sync->check(basedOn, text,
                         guarded([this, tab, generation, text, document,
                                  done](std::expected<std::optional<CheckRefusal>, std::string> checked) {
                             // The view this check was made for, and not one opened since, takes the answer.
                             Source const* const shown = _source.get();
                             if (shown == nullptr || shown->generation != generation) {
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
                             if (handleOf(tab) == nullptr || stateOf(tab) == nullptr) {
                                 refuse("", "the screen is closed");
                                 finish(done, false);
                                 return;
                             }
                             if (auto mounted = mountAsDraft(tab, document, text); !mounted) {
                                 refuse(mounted.error().pointer, mounted.error().reason);
                                 finish(done, false);
                                 return;
                             }
                             closeSourceLater();
                             finish(done, true);
                         }));
}

std::expected<void, MountError> CustomScreenChrome::mountAsDraft(WorkspaceTabId tab,
                                                                 std::shared_ptr<Document const> const& document,
                                                                 std::string const& text) {
    ScreenHandle* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    if (handle == nullptr || state == nullptr) {
        return std::unexpected(MountError{.screenId = {}, .reason = "the screen is closed", .pointer = {}});
    }
    if (auto mounted = _options.host->mountDraft(*handle, document); !mounted) {
        return std::unexpected(std::move(mounted.error()));
    }
    ++state->generation;
    state->draftText = text;
    state->message->set(std::string{});
    return {};
}

std::optional<std::string> CustomScreenChrome::draftOf(WorkspaceTabId tab) const {
    ScreenHandle const* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    if (handle == nullptr || state == nullptr || !DocumentScreenHost::draftedNow(*handle)) {
        return std::nullopt;
    }
    return state->draftText;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen's id and the document's text are told apart by name.
bool CustomScreenChrome::proposeDraft(WorkspaceTabId tab, std::string const& screenId, std::string const& text) {
    ScreenHandle const* const handle = handleOf(tab);
    std::optional<std::string> const shown =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!_offered->peek() || stateOf(tab) == nullptr || shown != screenId) {
        return false;
    }
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = _options.catalog()});
    if (!loaded) {
        notify("The assistant's draft could not be shown: " + loaded.error().reason, Tone::Warn);
        return false;
    }
    if (auto mounted = mountAsDraft(tab, std::make_shared<Document const>(std::move(*loaded)), text); !mounted) {
        notify("The assistant's draft could not be shown: " + mounted.error().reason, Tone::Warn);
        return false;
    }
    // A source view of this tab shows text the draft has replaced, and a check still running for it would mount over
    // the draft when it answers; a new generation, taken now, drops that answer. A view the person has edited stays
    // open with those edits, since closing it would throw them away unseen.
    if (Source* const source = _source.get(); source != nullptr && source->tab == tab) {
        source->generation = ++_lastSource;
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
    std::shared_ptr<TabState> const state = stateOf(tab);
    // One save at a time: a dialog already saving is the one whose reply is awaited, and a tab with a save in flight
    // has its answer to wait for.
    if (!_offered->peek() || state == nullptr || state->pending->peek() ||
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
    ScreenHandle const* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || state == nullptr) {
        failSaveAs(tab, 0, "the screen is closed");
        return;
    }
    std::string const base = baseOf(*screenId);
    std::string const text = DocumentScreenHost::drafted(*handle) ? state->draftText : currentText(*screenId);
    if (base.empty() || text.empty()) {
        failSaveAs(tab, state->generation, "the screen's base or document is not known");
        return;
    }
    if (state->pending->peek()) {
        failSaveAs(tab, state->generation, "another save is still running");
        return;
    }
    std::uint64_t const generation = state->generation;
    state->pending->set(true);
    _options.sync->save(CustomScreensSave{.app = {},
                                          .id = std::nullopt,
                                          .title = title,
                                          .basedOn = base,
                                          .document = text,
                                          .attributes = std::nullopt},
                        guarded([this, tab, generation, title](std::expected<std::string, std::string> saved) {
                            if (!saved) {
                                settled(tab);
                                failSaveAs(tab, generation, saved.error());
                                return;
                            }
                            closeSaveAsLater();
                            openSaved(tab, generation, *saved, title);
                        }));
}

void CustomScreenChrome::failSaveAs(WorkspaceTabId tab, std::uint64_t generation, std::string const& reason) {
    // The bar speaks for the draft the save was asked about, not for one that replaced it.
    if (holds(tab, generation)) {
        tell(tab, reason);
    }
    if (SaveAs* const dialog = _saveAs.get(); dialog != nullptr && dialog->tab == tab) {
        _options.runtime->batch([dialog, &reason] {
            dialog->busy->set(false);
            dialog->message->set(reason);
        });
    }
}

void CustomScreenChrome::openSaved(WorkspaceTabId tab, std::uint64_t generation, std::string const& screenId,
                                   std::string const& title) {
    _options.sync->open(
        screenId, guarded([this, tab, generation, screenId,
                           title](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            std::string const saved = "Saved as " + title;
            ScreenHandle* const handle = handleOf(tab);
            std::shared_ptr<TabState> const state = stateOf(tab);
            // The tab is replaced only while it still holds the draft that was saved: a newer draft is the person's
            // to keep, and the close guard is what asks before one is dropped.
            if (!holds(tab, generation)) {
                settled(tab);
                saveOutcome(tab, generation, saved);
                return;
            }
            if (!loaded) {
                settled(tab);
                notify(saved + ", but it could not be opened: " + loaded.error(), Tone::Warn);
                return;
            }
            // The draft mark ends for the replacement and stays ended only if the tab is replaced: the saved document
            // is stored, so the old screen may close without asking when the draft was all that kept it dirty, and
            // otherwise its own close guard decides. A screen that stays open still shows the draft, so the mark
            // returns with the draft's text and generation untouched.
            DocumentScreenHost::endDraft(*handle);
            ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
            _model->openInPlace(
                tab, screenId, params,
                guarded([this, tab, generation, saved](std::expected<WorkspaceTabId, MountError> opened) {
                    settled(tab);
                    if (!opened) {
                        ScreenHandle* const shown = handleOf(tab);
                        if (shown != nullptr && holds(tab, generation)) {
                            DocumentScreenHost::resumeDraft(*shown);
                        }
                        std::string const why =
                            opened.error().reason == kStayedOpenReason ? std::string{} : ": " + opened.error().reason;
                        notify(saved + ", but it stays a draft here until this screen can close" + why, Tone::Warn);
                        return;
                    }
                    if (std::shared_ptr<TabState> const replaced = stateOf(tab); replaced != nullptr) {
                        ++replaced->generation;
                        replaced->draftText.clear();
                    }
                    notify(saved, Tone::Ok);
                }));
        }));
}

void CustomScreenChrome::save(WorkspaceTabId tab) {
    ScreenHandle const* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || state == nullptr || state->pending->peek() || !DocumentScreenHost::drafted(*handle) ||
        !canSave(*screenId)) {
        return;
    }
    // `canSave` held, so the screen is listed and the default is never read.
    CustomScreenSummary const summary = summaryOf(*screenId).value_or(CustomScreenSummary{});
    std::string const& savedId = *screenId;
    std::string const title = summary.title;
    std::uint64_t const generation = state->generation;
    state->pending->set(true);
    _options.sync->save(
        CustomScreensSave{.app = {},
                          .id = savedId,
                          .title = title,
                          .basedOn = summary.basedOn,
                          .document = state->draftText,
                          .attributes = summary.attributes},
        guarded([this, tab, savedId, title, generation](std::expected<std::string, std::string> saved) {
            if (!saved) {
                settled(tab);
                if (holds(tab, generation)) {
                    tell(tab, saved.error());
                }
                return;
            }
            if (!holds(tab, generation)) {
                settled(tab);
                saveOutcome(tab, generation, "Saved " + title);
                return;
            }
            _options.sync->fetch(
                savedId, guarded([this, tab, generation,
                                  title](std::expected<std::shared_ptr<Document const>, std::string> const& loaded) {
                    settled(tab);
                    if (!holds(tab, generation)) {
                        saveOutcome(tab, generation, "Saved " + title);
                        return;
                    }
                    DocumentScreenHost::endDraft(*handleOf(tab));
                    std::shared_ptr<TabState> const shown = stateOf(tab);
                    ++shown->generation;
                    shown->draftText.clear();
                    shown->message->set(std::string{});
                    if (!loaded) {
                        notify("Saved " + title + ", but the stored screen could not be read again: " + loaded.error(),
                               Tone::Warn);
                        return;
                    }
                    notify("Saved " + title, Tone::Ok);
                }));
        }));
}

void CustomScreenChrome::discard(WorkspaceTabId tab) {
    ScreenHandle const* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    if (!screenId || !DocumentScreenHost::drafted(*handle)) {
        return;
    }
    auto const remount = [this, tab] {
        ScreenHandle* const shown = handleOf(tab);
        std::shared_ptr<TabState> const state = stateOf(tab);
        if (shown == nullptr || state == nullptr) {
            return;
        }
        if (auto discarded = _options.host->discardDraft(*shown); !discarded) {
            state->message->set(discarded.error().reason);
            return;
        }
        ++state->generation;
        state->draftText.clear();
        state->message->set(std::string{});
    };
    if (isCustomScreenId(*screenId) && _options.sync->documents().find(*screenId) == nullptr) {
        _options.sync->open(
            *screenId,
            guarded([this, tab, remount](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
                if (!loaded) {
                    tell(tab, loaded.error());
                    return;
                }
                remount();
            }));
        return;
    }
    // Posted: the press that asked runs in a handler, and a remount must not run inside one.
    _options.runtime->owner().post(guarded(remount));
}

Node CustomScreenChrome::openAsMenu(WorkspaceTabId tab, std::string const& screenId) {
    return ui::forEach<OpenAsShape>(
        [this, screenId] {
            OpenAsShape shape = openAsShape(screenId);
            return shape.choices.empty() ? std::vector<OpenAsShape>{} : std::vector<OpenAsShape>{std::move(shape)};
        },
        [](OpenAsShape const& shape) { return Key{shape.key}; },
        [this, tab, screenId](reactive::Signal<OpenAsShape> const& shape) {
            Menu menu;
            menu.style = MenuStyle::Button;
            menu.label = Prop<std::string>{std::string{"Open as"}};
            menu.common.testId = "open_as_" + screenId;
            for (OpenAsChoice const& choice : shape.peek().choices) {
                MenuItem item;
                item.label = Prop<std::string>{choice.label};
                item.checked = Prop<std::optional<bool>>{std::optional<bool>{choice.current}};
                item.onSelect = guarded([this, tab, target = choice.id] { openAs(tab, target); });
                menu.items.push_back(std::move(item));
            }
            return ui::menu(std::move(menu));
        },
        Axis::Horizontal, 0);
}

CustomScreenChrome::OpenAsShape CustomScreenChrome::openAsShape(std::string const& screenId) const {
    static_cast<void>(_listVersion->get());
    OpenAsShape shape;
    if (!_offered->get()) {
        return shape;
    }
    std::string const base = baseOf(screenId);
    std::shared_ptr<Document const> const baseDocument = base.empty() ? nullptr : _options.documents(base);
    // A base without params opens from the navigator; Open as is for screens opened with params.
    if (!baseDocument || baseDocument->params.empty()) {
        return shape;
    }
    std::vector<OpenAsChoice> customs;
    for (CustomScreenSummary const& screen : _listed.screens) {
        if (screen.basedOn != base) {
            continue;
        }
        std::string label = screen.title;
        if (!screen.mine) {
            label += " · by " + screen.savedBy;
        }
        if (screen.broken) {
            label += " · broken";
        }
        customs.push_back(OpenAsChoice{.id = screen.id, .label = std::move(label), .current = screen.id == screenId});
    }
    if (customs.empty()) {
        return shape;
    }
    shape.choices.push_back(OpenAsChoice{.id = base, .label = titleOf(base), .current = base == screenId});
    shape.choices.insert(shape.choices.end(), customs.begin(), customs.end());
    for (OpenAsChoice const& choice : shape.choices) {
        shape.key += choice.id + '\x1f' + choice.label + (choice.current ? "\x1f*" : "") + '\x1e';
    }
    return shape;
}

void CustomScreenChrome::openAs(WorkspaceTabId tab, std::string const& screenId) {
    ScreenHandle const* const handle = handleOf(tab);
    if (_model == nullptr || handle == nullptr || DocumentScreenHost::screenIdOf(*handle) == screenId) {
        return;
    }
    if (std::optional<CustomScreenSummary> const summary = summaryOf(screenId); summary && summary->broken) {
        showStoredSource(screenId);
        return;
    }
    ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
    std::string const from = DocumentScreenHost::screenIdOf(*handle).value_or(std::string{});
    auto const replace = [this, tab, screenId, params, from] {
        if (_model == nullptr || _model->handleOf(tab) == nullptr) {
            return;
        }
        _model->openInPlace(tab, screenId, params,
                            guarded([this, screenId, from](std::expected<WorkspaceTabId, MountError> opened) {
                                if (opened) {
                                    return;
                                }
                                if (opened.error().reason == kStayedOpenReason) {
                                    // The person kept the screen open: a choice, not a failure.
                                    notify("Open as cancelled: " + titleOf(from) + " stayed open", Tone::Info);
                                    return;
                                }
                                notify("Cannot open " + titleOf(screenId) + ": " + opened.error().reason, Tone::Err);
                            }));
    };
    if (!isCustomScreenId(screenId)) {
        // Posted: the menu that asked runs in a handler, and the tab's screen is about to be replaced.
        _options.runtime->owner().post(guarded(replace));
        return;
    }
    _options.sync->open(
        screenId,
        guarded([this, screenId, replace](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            if (!loaded) {
                notify("Cannot open " + titleOf(screenId) + ": " + loaded.error(), Tone::Err);
                return;
            }
            replace();
        }));
}

std::vector<NavigatorCustomEntry> CustomScreenChrome::navigatorEntries() const {
    std::vector<NavigatorCustomEntry> entries;
    if (!_offered->peek()) {
        return entries;
    }
    for (CustomScreenSummary const& screen : _listed.screens) {
        // Read from the base, not `opensWithoutParams`, which is false for a broken screen: a broken screen whose base
        // takes no params is still listed, so its source can be opened and fixed.
        std::shared_ptr<Document const> const base = _options.documents(screen.basedOn);
        if (!base || !base->params.empty()) {
            continue;
        }
        entries.push_back(NavigatorCustomEntry{.id = screen.id,
                                               .title = screen.title,
                                               .savedBy = screen.savedBy,
                                               .mine = screen.mine,
                                               .broken = screen.broken ? screen.broken->reason : std::string{}});
    }
    return entries;
}

bool CustomScreenChrome::isBroken(std::string_view screenId) const {
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary && summary->broken;
}

void CustomScreenChrome::openCustom(std::string const& screenId, ParamMap params,
                                    std::function<void(std::expected<void, std::string>)> const& done) {
    if (isBroken(screenId)) {
        // Only the source view opens: the screen itself is never mounted, which the caller is told.
        showStoredSource(screenId);
        if (done) {
            done(std::unexpected(titleOf(screenId) + " is broken; its source is open"));
        }
        return;
    }
    _options.sync->open(screenId, guarded([this, screenId, params = std::move(params),
                                           done](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
                            if (!loaded) {
                                if (done) {
                                    done(std::unexpected(loaded.error()));
                                } else {
                                    notify("Cannot open " + titleOf(screenId) + ": " + loaded.error(), Tone::Err);
                                }
                                return;
                            }
                            std::expected<void, std::string> const opened = _options.openTab(screenId, params);
                            if (done) {
                                done(opened);
                            } else if (!opened) {
                                notify("Cannot open " + titleOf(screenId) + ": " + opened.error(), Tone::Err);
                            }
                        }));
}

void CustomScreenChrome::showStoredSource(std::string const& screenId) {
    _options.sync->get(
        screenId, guarded([this, screenId](std::expected<CustomScreenFound, std::string> found) {
            if (!found) {
                notify("Cannot open the source of " + titleOf(screenId) + ": " + found.error(), Tone::Err);
                return;
            }
            bool const editable = _offered->peek() && found->summary.mine;
            openSource(SourceMode::Stored, std::nullopt, screenId, prettyJson(found->document), !editable);
            if (found->summary.broken) {
                refuse(found->summary.broken->pointer, found->summary.broken->reason);
            }
        }));
}

void CustomScreenChrome::saveFromSource() {
    Source* const source = _source.get();
    if (source == nullptr || source->mode != SourceMode::Stored || source->readOnly || source->busy->peek()) {
        return;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(source->screenId);
    if (!summary) {
        refuse("", "the custom screen is not listed");
        return;
    }
    std::string const text = source->text->peek();
    std::string const customId = source->screenId;
    std::uint64_t const generation = source->generation;
    _options.runtime->batch([source] {
        source->busy->set(true);
        source->status->set(std::string{"Checking…"});
    });
    _options.sync->check(
        summary->basedOn, text,
        guarded([this, customId, text, generation,
                 listed = *summary](std::expected<std::optional<CheckRefusal>, std::string> checked) {
            // The view the check was made for takes the answer, not one opened since.
            Source const* const shown = _source.get();
            if (shown == nullptr || shown->generation != generation) {
                return;
            }
            if (!checked) {
                refuse("", checked.error());
                return;
            }
            if (*checked) {
                refuse((*checked)->pointer, (*checked)->reason);
                return;
            }
            shown->status->set(std::string{"Saving…"});
            _options.sync->save(
                CustomScreensSave{.app = {},
                                  .id = customId,
                                  .title = listed.title,
                                  .basedOn = listed.basedOn,
                                  .document = text,
                                  .attributes = listed.attributes},
                guarded([this, generation, title = listed.title](std::expected<std::string, std::string> saved) {
                    if (!saved) {
                        if (_source != nullptr && _source->generation == generation) {
                            refuse("", saved.error());
                        } else {
                            notify("Not saved: " + saved.error(), Tone::Err);
                        }
                        return;
                    }
                    if (_source != nullptr && _source->generation == generation) {
                        closeSourceLater();
                    }
                    notify("Saved " + title, Tone::Ok);
                }));
        }));
}

void CustomScreenChrome::askRemove(WorkspaceTabId tab) {
    ScreenHandle const* const handle = handleOf(tab);
    std::optional<std::string> const screenId =
        handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
    // One removal at a time: a confirmation already removing is the one whose reply is awaited.
    if (!screenId || !canSave(*screenId) || (_remove != nullptr && _remove->busy->peek())) {
        return;
    }
    // `canSave` held, so the screen is listed and the default is never read.
    CustomScreenSummary const summary = summaryOf(*screenId).value_or(CustomScreenSummary{});
    _remove.reset();
    auto made = std::make_unique<RemoveConfirm>();
    RemoveConfirm* const confirm = made.get();
    confirm->generation = ++_lastRemove;
    confirm->screenId = *screenId;
    confirm->title = summary.title;
    confirm->basedOn = summary.basedOn;
    confirm->message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
    confirm->busy = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);

    Dialog node;
    node.open = Prop<bool>{true};
    node.title = Prop<std::string>{"Remove " + summary.title + "?"};
    node.description = Prop<std::string>{"It is removed for good. A tab that shows it opens " +
                                         titleOf(summary.basedOn) + " instead."};
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
    _options.sync->remove(
        confirm->screenId,
        guarded([this, generation = confirm->generation, screenId = confirm->screenId, basedOn = confirm->basedOn,
                 title = confirm->title](std::expected<void, std::string> const& removed) {
            // The confirmation the removal was asked from, and not one opened since, takes the answer.
            RemoveConfirm* const open =
                _remove != nullptr && _remove->generation == generation ? _remove.get() : nullptr;
            if (!removed) {
                if (open != nullptr) {
                    _options.runtime->batch([open, &removed] {
                        open->busy->set(false);
                        open->message->set(removed.error());
                    });
                } else {
                    notify("Not removed: " + removed.error(), Tone::Err);
                }
                return;
            }
            if (open != nullptr) {
                closeRemoveLater();
            }
            notify("Removed " + title, Tone::Ok);
            fallBack(screenId, basedOn, title);
        }));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the removed screen, its base, then its title.
void CustomScreenChrome::fallBack(std::string const& screenId, std::string const& basedOn, std::string const& title) {
    if (_model == nullptr) {
        return;
    }
    for (WorkspaceTabId const tab : _model->order()) {
        ScreenHandle const* const handle = _model->handleOf(tab);
        if (handle == nullptr || DocumentScreenHost::screenIdOf(*handle) != screenId) {
            continue;
        }
        ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
        _model->openInPlace(tab, basedOn, params,
                            guarded([this, title](std::expected<WorkspaceTabId, MountError> const& opened) {
                                if (opened) {
                                    return;
                                }
                                std::string text = title;
                                text += " is removed, but stays open in its tab until it can close";
                                if (opened.error().reason != kStayedOpenReason) {
                                    text += ": " + opened.error().reason;
                                }
                                notify(text, Tone::Warn);
                            }));
    }
}

void CustomScreenChrome::closeRemoveLater() {
    RemoveConfirm const* const shown = _remove.get();
    _options.runtime->owner().post(guarded([this, shown] {
        if (_remove.get() == shown) {
            _remove.reset();
        }
    }));
}

void CustomScreenChrome::notify(std::string const& text, Tone tone) const {
    if (_options.notify) {
        _options.notify(text, tone);
    }
}

}  // namespace morph::ui
