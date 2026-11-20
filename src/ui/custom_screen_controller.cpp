// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/ui/custom_screen_controller.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

namespace {

CustomScreenCheckResult refusedAt(std::string pointer, std::string reason) {
    return CustomScreenCheckResult{
        .status = CustomScreenCheckStatus::Refused,
        .refusal = CheckRefusal{.pointer = std::move(pointer), .reason = std::move(reason)}};
}

CustomScreenCheckResult dropped() {
    return CustomScreenCheckResult{.status = CustomScreenCheckStatus::Dropped, .refusal = {}};
}

CustomScreenCheckResult succeeded() {
    return CustomScreenCheckResult{.status = CustomScreenCheckStatus::Done, .refusal = {}};
}

void finish(std::function<void(CustomScreenCheckResult)> const& callback, CustomScreenCheckResult result) {
    if (callback) {
        callback(std::move(result));
    }
}

void answer(std::function<void(std::expected<void, std::string>)> const& callback,
            std::expected<void, std::string> result) {
    if (callback) {
        callback(std::move(result));
    }
}

}  // namespace

CustomScreenController::CustomScreenController(CustomScreenControllerOptions options) : _options{std::move(options)} {
    if (_options.runtime == nullptr || _options.host == nullptr || _options.sync == nullptr) {
        throw std::invalid_argument{"morph::ui::CustomScreenController: a null runtime, host or sync"};
    }
    if (!_options.documents || !_options.sourceOf || !_options.catalog || !_options.openTab) {
        throw std::invalid_argument{
            "morph::ui::CustomScreenController: documents, sourceOf, catalog and openTab are required"};
    }
    _offered = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    _listVersion = std::make_unique<reactive::Signal<std::uint64_t>>(*_options.runtime, 0);
}

CustomScreenController::~CustomScreenController() { *_alive = false; }

void CustomScreenController::attach(WorkspaceModel* model) {
    // Tab ids are issued per workspace: an answer asked under the old one must not find a tab of the new one by id.
    if (model != _model) {
        ++_epoch;
    }
    _model = model;
    // The old workspace's views keep the state they read alive themselves.
    _tabs.clear();
    ++_sourceSerial;
    // The list belongs to the frame that is gone: the new session's list arrives with its own sync.
    if (model == nullptr) {
        _listed = CustomScreensListed{};
    }
}

void CustomScreenController::setOffered(bool offered) {
    _options.runtime->batch([this, offered] { _offered->set(offered); });
    if (_options.onListChanged) {
        _options.onListChanged();
    }
}

void CustomScreenController::listed(CustomScreensListed const& listed) {
    _listed = listed;
    _options.runtime->batch([this] {
        _offered->set(true);
        _listVersion->set(_listVersion->peek() + 1);
    });
    if (_options.onListChanged) {
        _options.onListChanged();
    }
}

bool CustomScreenController::offered() const { return _offered->get(); }

void CustomScreenController::follow(WorkspaceTabId tab) {
    std::shared_ptr<TabState>& slot = _tabs[tab.value];
    if (slot == nullptr) {
        slot = std::make_shared<TabState>();
        slot->message = std::make_unique<reactive::Signal<std::string>>(*_options.runtime, std::string{});
        slot->pending = std::make_unique<reactive::Signal<bool>>(*_options.runtime, false);
    }
    prune();
}

std::shared_ptr<CustomScreenController::TabState> CustomScreenController::stateOf(WorkspaceTabId tab) const {
    auto const found = _tabs.find(tab.value);
    return found == _tabs.end() ? nullptr : found->second;
}

void CustomScreenController::prune() {
    for (auto shown = _tabs.begin(); shown != _tabs.end();) {
        TabState& state = *shown->second;
        bool const held = handleOf(WorkspaceTabId{shown->first}) != nullptr;
        if (held) {
            state.seen = true;
        }
        // A state made while its tab was opening has not been seen with a handle yet, and one with a save in flight
        // is still read by the answer.
        if (!held && state.seen && !state.pending->peek()) {
            shown = _tabs.erase(shown);
        } else {
            ++shown;
        }
    }
}

void CustomScreenController::settled(WorkspaceTabId tab) {
    if (std::shared_ptr<TabState> const state = stateOf(tab)) {
        state->pending->set(false);
    }
    prune();
}

bool CustomScreenController::holds(WorkspaceTabId tab, std::uint64_t generation) const {
    std::shared_ptr<TabState> const state = stateOf(tab);
    return state != nullptr && handleOf(tab) != nullptr && state->generation == generation;
}

void CustomScreenController::tell(WorkspaceTabId tab, std::string text) {
    if (std::shared_ptr<TabState> const state = stateOf(tab)) {
        state->message->set(std::move(text));
    }
}

void CustomScreenController::saveOutcome(WorkspaceTabId tab, std::uint64_t generation, CustomScreenSaveKind kind,
                                         std::string const& title) {
    CustomScreenOutcome outcome{
        .event = CustomScreenEvent::Saved, .save = kind, .title = title, .detail = {}, .tone = Tone::Warn};
    if (holds(tab, generation)) {
        outcome.tone = Tone::Ok;
    } else if (ScreenHandle const* const handle = handleOf(tab); handle == nullptr) {
        outcome.event = CustomScreenEvent::SavedTabClosed;
    } else if (DocumentScreenHost::drafted(*handle)) {
        outcome.event = CustomScreenEvent::SavedDraftReplaced;
    } else {
        outcome.event = CustomScreenEvent::SavedDraftGone;
    }
    tellOutcome(outcome);
}

ScreenHandle* CustomScreenController::handleOf(WorkspaceTabId tab) const {
    return _model == nullptr ? nullptr : _model->handleOf(tab);
}

bool CustomScreenController::drafted(WorkspaceTabId tab) const {
    if (_model == nullptr) {
        return false;
    }
    // A tracked read of the model's tabs first, so a binding made while the tab is opening reads the handle once the
    // model holds it.
    static_cast<void>(_model->tabs().get());
    ScreenHandle const* const handle = _model->handleOf(tab);
    return handle != nullptr && DocumentScreenHost::drafted(*handle);
}

bool CustomScreenController::saving(WorkspaceTabId tab) const {
    std::shared_ptr<TabState> const state = stateOf(tab);
    return state != nullptr && state->pending->get();
}

std::string CustomScreenController::message(WorkspaceTabId tab) const {
    std::shared_ptr<TabState> const state = stateOf(tab);
    return state == nullptr ? std::string{} : state->message->get();
}

bool CustomScreenController::canChange(std::string const& screenId) const {
    static_cast<void>(_listVersion->get());
    if (!_offered->get() || !isCustomScreenId(screenId)) {
        return false;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary.has_value() && summary->mine;
}

std::optional<std::string> CustomScreenController::draftOf(WorkspaceTabId tab) const {
    ScreenHandle const* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    if (handle == nullptr || state == nullptr || !DocumentScreenHost::draftedNow(*handle)) {
        return std::nullopt;
    }
    return state->draftText;
}

std::optional<std::string> CustomScreenController::screenOf(WorkspaceTabId tab) const {
    ScreenHandle const* const handle = handleOf(tab);
    return handle == nullptr ? std::nullopt : DocumentScreenHost::screenIdOf(*handle);
}

std::optional<CustomScreenSummary> CustomScreenController::summaryOf(std::string_view screenId) const {
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

std::string CustomScreenController::baseOf(std::string const& screenId) const {
    if (!isCustomScreenId(screenId)) {
        return screenId;
    }
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary ? summary->basedOn : std::string{};
}

std::string CustomScreenController::currentText(std::string const& screenId) const {
    if (!isCustomScreenId(screenId)) {
        return _options.sourceOf(screenId);
    }
    CustomDocuments::Entry const* const entry = _options.sync->documents().entry(screenId);
    return entry == nullptr ? std::string{} : entry->text;
}

std::string CustomScreenController::titleOf(std::string const& screenId) const {
    if (isCustomScreenId(screenId)) {
        std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
        return summary ? summary->title : screenId;
    }
    return _options.titleOf ? _options.titleOf(screenId) : screenId;
}

bool CustomScreenController::isBroken(std::string_view screenId) const {
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    return summary && summary->broken;
}

void CustomScreenController::tellOutcome(CustomScreenOutcome const& outcome) const {
    if (_options.onOutcome) {
        _options.onOutcome(outcome);
    }
}

void CustomScreenController::source(WorkspaceTabId tab, std::function<void(CustomScreenSource)> done) {
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
                            guarded([this, tab, stored = *screenId, done = std::move(done)](
                                        std::expected<std::shared_ptr<Document const>, std::string> loaded) mutable {
                                if (loaded && _options.sync->documents().entry(stored) != nullptr) {
                                    source(tab, std::move(done));
                                    return;
                                }
                                tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::SourceUnreadable,
                                                                .save = CustomScreenSaveKind::None,
                                                                .title = titleOf(stored),
                                                                .detail = loaded ? std::string{} : loaded.error(),
                                                                .tone = Tone::Warn});
                            }));
        return;
    }
    std::string const text = drafted ? state->draftText : currentText(*screenId);
    done(CustomScreenSource{.screenId = *screenId,
                            .text = prettyJson(text),
                            .readOnly = !_offered->peek(),
                            .stored = false,
                            .broken = std::nullopt});
}

void CustomScreenController::storedSource(std::string const& screenId, std::function<void(CustomScreenSource)> done) {
    _options.sync->get(
        screenId,
        guarded([this, screenId, done = std::move(done)](std::expected<CustomScreenFound, std::string> found) {
            if (!found) {
                tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::StoredSourceUnreadable,
                                                .save = CustomScreenSaveKind::None,
                                                .title = titleOf(screenId),
                                                .detail = found.error(),
                                                .tone = Tone::Err});
                return;
            }
            bool const editable = _offered->peek() && found->summary.mine;
            done(CustomScreenSource{.screenId = screenId,
                                    .text = prettyJson(found->document),
                                    .readOnly = !editable,
                                    .stored = true,
                                    .broken = found->summary.broken});
        }));
}

void CustomScreenController::preview(WorkspaceTabId tab, std::string const& text,
                                     std::function<void(CustomScreenCheckResult)> done) {
    std::shared_ptr<TabState> const state = stateOf(tab);
    std::optional<std::string> const screenId = screenOf(tab);
    if (!screenId || state == nullptr) {
        finish(done, refusedAt("", "the screen is closed"));
        return;
    }
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = _options.catalog()});
    if (!loaded) {
        finish(done, refusedAt(loaded.error().path, loaded.error().reason));
        return;
    }
    std::string const basedOn = baseOf(*screenId);
    if (basedOn.empty()) {
        finish(done, refusedAt("", "the base of " + *screenId + " is not known"));
        return;
    }
    auto const document = std::make_shared<Document const>(std::move(*loaded));
    std::uint64_t const serial = _sourceSerial;
    std::uint64_t const generation = state->generation;
    _options.sync->check(
        basedOn, text,
        guarded([this, tab, serial, generation, text, document,
                 done = std::move(done)](std::expected<std::optional<CheckRefusal>, std::string> checked) {
            // The asker that is still there takes the answer: not one replaced since, and not a tab whose draft
            // changed meanwhile, such as one the assistant proposed.
            std::shared_ptr<TabState> const current = stateOf(tab);
            if (serial != _sourceSerial || (current != nullptr && current->generation != generation)) {
                finish(done, dropped());
                return;
            }
            if (!checked) {
                finish(done, refusedAt("", checked.error()));
                return;
            }
            if (*checked) {
                finish(done, refusedAt((*checked)->pointer, (*checked)->reason));
                return;
            }
            if (auto mounted = mountAsDraft(tab, document, text); !mounted) {
                finish(done, refusedAt(mounted.error().pointer, mounted.error().reason));
                return;
            }
            finish(done, succeeded());
        }));
}

std::expected<void, MountError> CustomScreenController::mountAsDraft(WorkspaceTabId tab,
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen's id and the document's text are told apart by name.
bool CustomScreenController::proposeDraft(WorkspaceTabId tab, std::string const& screenId, std::string const& text) {
    if (!_offered->peek() || stateOf(tab) == nullptr || screenOf(tab) != screenId) {
        return false;
    }
    auto const refused = [this, &screenId](std::string reason) {
        tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::ProposalNotShown,
                                        .save = CustomScreenSaveKind::None,
                                        .title = titleOf(screenId),
                                        .detail = std::move(reason),
                                        .tone = Tone::Warn});
        return false;
    };
    auto loaded = loadDocumentText(text, LoadOptions{.catalog = _options.catalog()});
    if (!loaded) {
        return refused(loaded.error().reason);
    }
    if (auto mounted = mountAsDraft(tab, std::make_shared<Document const>(std::move(*loaded)), text); !mounted) {
        return refused(mounted.error().reason);
    }
    return true;
}

void CustomScreenController::saveAs(WorkspaceTabId tab, std::string const& title,
                                    std::optional<Json::object_t> attributes,
                                    std::function<void(std::expected<void, std::string>)> done) {
    std::shared_ptr<TabState> const state = stateOf(tab);
    std::optional<std::string> const screenId = screenOf(tab);
    auto const fail = [this, tab, &done](std::uint64_t generation, std::string const& reason) {
        // The tab's message speaks for the draft the save was asked about, not for one that replaced it.
        if (holds(tab, generation)) {
            tell(tab, reason);
        }
        answer(done, std::unexpected(reason));
    };
    if (!screenId || state == nullptr) {
        fail(0, "the screen is closed");
        return;
    }
    std::string const base = baseOf(*screenId);
    std::string const text = DocumentScreenHost::drafted(*handleOf(tab)) ? state->draftText : currentText(*screenId);
    if (base.empty() || text.empty()) {
        fail(state->generation, "the screen's base or document is not known");
        return;
    }
    if (!_offered->peek()) {
        fail(state->generation, "this server does not store custom screens");
        return;
    }
    if (state->pending->peek()) {
        fail(state->generation, "another save is still running");
        return;
    }
    std::uint64_t const generation = state->generation;
    state->pending->set(true);
    _options.sync->save(
        CustomScreensSave{.app = {},
                          .id = std::nullopt,
                          .title = title,
                          .basedOn = base,
                          .document = text,
                          .attributes = std::move(attributes)},
        guarded([this, tab, generation, title, done = std::move(done)](std::expected<std::string, std::string> saved) {
            if (!saved) {
                settled(tab);
                if (holds(tab, generation)) {
                    tell(tab, saved.error());
                }
                answer(done, std::unexpected(saved.error()));
                return;
            }
            openSaved(tab, generation, *saved, title, done);
        }));
}

void CustomScreenController::openSaved(WorkspaceTabId tab, std::uint64_t generation, std::string const& screenId,
                                       std::string const& title,
                                       std::function<void(std::expected<void, std::string>)> const& done) {
    answer(done, {});
    _options.sync->open(
        screenId, guarded([this, tab, generation, screenId,
                           title](std::expected<std::shared_ptr<Document const>, std::string> loaded) {
            // The tab is replaced only while it still holds the draft that was saved: a newer draft is
            // the person's to keep, and the close guard is what asks before one is dropped.
            if (!holds(tab, generation)) {
                settled(tab);
                saveOutcome(tab, generation, CustomScreenSaveKind::SaveAs, title);
                return;
            }
            if (!loaded) {
                settled(tab);
                tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::SavedNotOpened,
                                                .save = CustomScreenSaveKind::SaveAs,
                                                .title = title,
                                                .detail = loaded.error(),
                                                .tone = Tone::Warn});
                return;
            }
            // The draft mark ends for the replacement and stays ended only if the tab is replaced: the
            // saved document is stored, so the old screen may close without asking when the draft was
            // all that kept it dirty, and otherwise its own close guard decides. A screen that stays
            // open still shows the draft, so the mark returns with the draft's text and generation
            // untouched.
            ScreenHandle* const handle = handleOf(tab);
            DocumentScreenHost::endDraft(*handle);
            ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
            _model->openInPlace(
                tab, screenId, params,
                guarded([this, tab, generation, title](std::expected<WorkspaceTabId, MountError> opened) {
                    settled(tab);
                    if (!opened) {
                        ScreenHandle* const shown = handleOf(tab);
                        if (shown != nullptr && holds(tab, generation)) {
                            DocumentScreenHost::resumeDraft(*shown);
                        }
                        tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::SavedStaysDraft,
                                                        .save = CustomScreenSaveKind::SaveAs,
                                                        .title = title,
                                                        .detail = opened.error().reason == kStayedOpenReason
                                                                      ? std::string{}
                                                                      : opened.error().reason,
                                                        .tone = Tone::Warn});
                        return;
                    }
                    if (std::shared_ptr<TabState> const replaced = stateOf(tab); replaced != nullptr) {
                        ++replaced->generation;
                        replaced->draftText.clear();
                    }
                    tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::Saved,
                                                    .save = CustomScreenSaveKind::SaveAs,
                                                    .title = title,
                                                    .detail = {},
                                                    .tone = Tone::Ok});
                }));
        }));
}

void CustomScreenController::save(WorkspaceTabId tab) {
    ScreenHandle const* const handle = handleOf(tab);
    std::shared_ptr<TabState> const state = stateOf(tab);
    std::optional<std::string> const screenId = screenOf(tab);
    if (!screenId || state == nullptr || state->pending->peek() || !DocumentScreenHost::drafted(*handle) ||
        !canChange(*screenId)) {
        return;
    }
    // `canChange` held, so the screen is listed and the default is never read.
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
                saveOutcome(tab, generation, CustomScreenSaveKind::Save, title);
                return;
            }
            _options.sync->fetch(
                savedId, guarded([this, tab, generation,
                                  title](std::expected<std::shared_ptr<Document const>, std::string> const& loaded) {
                    settled(tab);
                    if (!holds(tab, generation)) {
                        saveOutcome(tab, generation, CustomScreenSaveKind::Save, title);
                        return;
                    }
                    DocumentScreenHost::endDraft(*handleOf(tab));
                    std::shared_ptr<TabState> const shown = stateOf(tab);
                    ++shown->generation;
                    shown->draftText.clear();
                    shown->message->set(std::string{});
                    if (!loaded) {
                        tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::SavedNotReread,
                                                        .save = CustomScreenSaveKind::Save,
                                                        .title = title,
                                                        .detail = loaded.error(),
                                                        .tone = Tone::Warn});
                        return;
                    }
                    tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::Saved,
                                                    .save = CustomScreenSaveKind::Save,
                                                    .title = title,
                                                    .detail = {},
                                                    .tone = Tone::Ok});
                }));
        }));
}

void CustomScreenController::discard(WorkspaceTabId tab) {
    ScreenHandle const* const handle = handleOf(tab);
    std::optional<std::string> const screenId = screenOf(tab);
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

void CustomScreenController::checkAndSave(std::string const& screenId, std::string const& text,
                                          std::function<void()> checked,
                                          std::function<void(CustomScreenCheckResult)> done) {
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    if (!summary) {
        finish(done, refusedAt("", "the custom screen is not listed"));
        return;
    }
    std::uint64_t const serial = _sourceSerial;
    _options.sync->check(
        summary->basedOn, text,
        guarded([this, screenId, text, serial, listed = *summary, checked = std::move(checked),
                 done = std::move(done)](std::expected<std::optional<CheckRefusal>, std::string> answered) {
            // The asker the check was made for takes the answer, not one opened since.
            if (serial != _sourceSerial) {
                finish(done, dropped());
                return;
            }
            if (!answered) {
                finish(done, refusedAt("", answered.error()));
                return;
            }
            if (*answered) {
                finish(done, refusedAt((*answered)->pointer, (*answered)->reason));
                return;
            }
            if (checked) {
                checked();
            }
            _options.sync->save(
                CustomScreensSave{.app = {},
                                  .id = screenId,
                                  .title = listed.title,
                                  .basedOn = listed.basedOn,
                                  .document = text,
                                  .attributes = listed.attributes},
                guarded([this, serial, title = listed.title, done](std::expected<std::string, std::string> saved) {
                    bool const asked = serial == _sourceSerial;
                    if (!saved) {
                        if (asked) {
                            finish(done, refusedAt("", saved.error()));
                        } else {
                            tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::SaveRefused,
                                                            .save = CustomScreenSaveKind::Save,
                                                            .title = title,
                                                            .detail = saved.error(),
                                                            .tone = Tone::Err});
                        }
                        return;
                    }
                    finish(done, asked ? succeeded() : dropped());
                    tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::Saved,
                                                    .save = CustomScreenSaveKind::Save,
                                                    .title = title,
                                                    .detail = {},
                                                    .tone = Tone::Ok});
                }));
        }));
}

void CustomScreenController::openAs(WorkspaceTabId tab, std::string const& screenId) {
    std::optional<std::string> const from = screenOf(tab);
    if (_model == nullptr || !from || *from == screenId) {
        return;
    }
    ParamMap const params = DocumentScreenHost::paramsOf(*handleOf(tab)).value_or(ParamMap{});
    auto const replace = [this, tab, screenId, params, from = *from] {
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
                                    tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::OpenAsCancelled,
                                                                    .save = CustomScreenSaveKind::None,
                                                                    .title = titleOf(from),
                                                                    .detail = {},
                                                                    .tone = Tone::Info});
                                    return;
                                }
                                tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::OpenFailed,
                                                                .save = CustomScreenSaveKind::None,
                                                                .title = titleOf(screenId),
                                                                .detail = opened.error().reason,
                                                                .tone = Tone::Err});
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
                tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::OpenFailed,
                                                .save = CustomScreenSaveKind::None,
                                                .title = titleOf(screenId),
                                                .detail = loaded.error(),
                                                .tone = Tone::Err});
                return;
            }
            replace();
        }));
}

void CustomScreenController::openCustom(std::string const& screenId, ParamMap params,
                                        std::function<void(std::expected<void, std::string>)> const& done) {
    auto const failed = [this, screenId, done](std::string reason) {
        if (done) {
            done(std::unexpected(std::move(reason)));
            return;
        }
        tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::OpenFailed,
                                        .save = CustomScreenSaveKind::None,
                                        .title = titleOf(screenId),
                                        .detail = std::move(reason),
                                        .tone = Tone::Err});
    };
    _options.sync->open(screenId, guarded([this, screenId, params = std::move(params), done, failed](
                                              std::expected<std::shared_ptr<Document const>, std::string> loaded) {
                            if (!loaded) {
                                failed(loaded.error());
                                return;
                            }
                            std::expected<void, std::string> const opened = _options.openTab(screenId, params);
                            if (!opened) {
                                failed(opened.error());
                                return;
                            }
                            answer(done, {});
                        }));
}

void CustomScreenController::remove(std::string const& screenId,
                                    std::function<void(std::expected<void, std::string>)> done) {
    std::optional<CustomScreenSummary> const summary = summaryOf(screenId);
    std::string const title = summary ? summary->title : screenId;
    std::string const basedOn = summary ? summary->basedOn : std::string{};
    _options.sync->remove(screenId, guarded([this, screenId, title, basedOn,
                                             done = std::move(done)](std::expected<void, std::string> const& removed) {
                              if (!removed) {
                                  answer(done, removed);
                                  return;
                              }
                              answer(done, {});
                              tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::Removed,
                                                              .save = CustomScreenSaveKind::None,
                                                              .title = title,
                                                              .detail = {},
                                                              .tone = Tone::Ok});
                              fallBack(screenId, basedOn, title);
                          }));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the removed screen, its base, then its title.
void CustomScreenController::fallBack(std::string const& screenId, std::string const& basedOn,
                                      std::string const& title) {
    if (_model == nullptr || basedOn.empty()) {
        return;
    }
    for (WorkspaceTabId const tab : _model->order()) {
        ScreenHandle const* const handle = _model->handleOf(tab);
        if (handle == nullptr || DocumentScreenHost::screenIdOf(*handle) != screenId) {
            continue;
        }
        ParamMap const params = DocumentScreenHost::paramsOf(*handle).value_or(ParamMap{});
        _model->openInPlace(
            tab, basedOn, params, guarded([this, title](std::expected<WorkspaceTabId, MountError> const& opened) {
                if (opened) {
                    return;
                }
                tellOutcome(CustomScreenOutcome{
                    .event = CustomScreenEvent::RemovedStaysOpen,
                    .save = CustomScreenSaveKind::None,
                    .title = title,
                    .detail = opened.error().reason == kStayedOpenReason ? std::string{} : opened.error().reason,
                    .tone = Tone::Warn});
            }));
    }
}

void CustomScreenController::layoutChangesOf(std::string const& screenId,
                                             std::function<void(std::expected<LayoutChanges, std::string>)> done) {
    _options.sync->get(
        screenId, guarded([this, done = std::move(done)](std::expected<CustomScreenFound, std::string> found) {
            if (!found) {
                done(std::unexpected(std::move(found.error())));
                return;
            }
            auto changes =
                layoutChanges(_options.sourceOf(found->summary.basedOn), _options.catalog().schema, found->document);
            if (!changes) {
                done(std::unexpected(changes.error().message()));
                return;
            }
            done(std::move(*changes));
        }));
}

void CustomScreenController::applyLayoutChoices(std::string const& screenId, LayoutChoices choices,
                                                std::function<void(CustomScreenCheckResult)> done) {
    if (!canChange(screenId)) {
        finish(done, refusedAt("", "the custom screen is not the person's own"));
        return;
    }
    _options.sync->get(screenId, guarded([this, screenId, choices = std::move(choices), done = std::move(done)](
                                             std::expected<CustomScreenFound, std::string> found) mutable {
                           if (!found) {
                               finish(done, refusedAt("", found.error()));
                               return;
                           }
                           CustomScreenSummary const& listed = found->summary;
                           auto updated = morph::ui::applyLayoutChoices(
                               _options.sourceOf(listed.basedOn), _options.catalog().schema, found->document, choices);
                           if (!updated) {
                               finish(done, refusedAt(updated.error().path, updated.error().reason));
                               return;
                           }
                           saveUpdate(screenId, listed, *updated, std::move(done));
                       }));
}

void CustomScreenController::saveUpdate(std::string const& screenId, CustomScreenSummary const& listed,
                                        std::string const& text, std::function<void(CustomScreenCheckResult)> done) {
    _options.sync->check(
        listed.basedOn, text,
        guarded([this, screenId, listed, text,
                 done = std::move(done)](std::expected<std::optional<CheckRefusal>, std::string> answered) {
            if (!answered) {
                finish(done, refusedAt("", answered.error()));
                return;
            }
            if (*answered) {
                finish(done, refusedAt((*answered)->pointer, (*answered)->reason));
                return;
            }
            _options.sync->save(
                CustomScreensSave{.app = {},
                                  .id = screenId,
                                  .title = listed.title,
                                  .basedOn = listed.basedOn,
                                  .document = text,
                                  .attributes = listed.attributes},
                guarded([this, title = listed.title, done](std::expected<std::string, std::string> saved) {
                    if (!saved) {
                        finish(done, refusedAt("", saved.error()));
                        return;
                    }
                    finish(done, succeeded());
                    tellOutcome(CustomScreenOutcome{.event = CustomScreenEvent::Saved,
                                                    .save = CustomScreenSaveKind::Save,
                                                    .title = title,
                                                    .detail = {},
                                                    .tone = Tone::Ok});
                }));
        }));
}

std::vector<CustomScreenChoice> CustomScreenController::openAsChoices(std::string const& screenId) const {
    static_cast<void>(_listVersion->get());
    std::vector<CustomScreenChoice> choices;
    if (!_offered->get()) {
        return choices;
    }
    std::string const base = baseOf(screenId);
    std::shared_ptr<Document const> const baseDocument = base.empty() ? nullptr : _options.documents(base);
    // A base without params opens from the navigator; Open as is for screens opened with params.
    if (!baseDocument || baseDocument->params.empty()) {
        return choices;
    }
    for (CustomScreenSummary const& screen : _listed.screens) {
        if (screen.basedOn != base) {
            continue;
        }
        choices.push_back(CustomScreenChoice{.id = screen.id,
                                             .title = screen.title,
                                             .savedBy = screen.savedBy,
                                             .mine = screen.mine,
                                             .broken = screen.broken.has_value(),
                                             .current = screen.id == screenId});
    }
    if (choices.empty()) {
        return choices;
    }
    choices.insert(choices.begin(), CustomScreenChoice{.id = base,
                                                       .title = titleOf(base),
                                                       .savedBy = {},
                                                       .mine = true,
                                                       .broken = false,
                                                       .current = base == screenId});
    return choices;
}

std::vector<NavigatorCustomEntry> CustomScreenController::navigatorEntries() const {
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

}  // namespace morph::ui
