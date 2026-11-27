// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/table_views.hpp>.

#include <algorithm>
#include <exception>
#include <expected>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/table_views.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace morph::ui {

async::Completion<TableViews::Result> TableViews::load() {
    auto [completion, promise] = async::Completion<Result>::makeSettleable(_held->owner);
    bool const shares = this->shares();
    auto const settle =
        std::make_shared<Settle>(Settle{.promise = std::move(promise), .waiting = shares ? 2 : 1, .failure = {}});
    loadPart(_held, &Held::own, settle);
    if (shares) {
        loadPart(_held, &Held::shared, settle);
    }
    return std::move(completion);
}

table::SavedView const* TableViews::find(table::ViewsDocument const& document, std::string_view name) {
    auto const found = std::ranges::find(document.views, name, &table::SavedView::name);
    return found == document.views.end() ? nullptr : &*found;
}

std::string TableViews::failed(table::ViewsPart part, std::string const& reason) {
    if (part == table::ViewsPart::Shared) {
        return reason.empty() ? std::string{"This view could not be published"}
                              : "This view could not be published: " + reason;
    }
    return reason.empty() ? std::string{"Views could not be saved"} : "Views could not be saved: " + reason;
}

std::string TableViews::reasonOf(std::exception_ptr const& error) {
    std::string reason;
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& failure) {
        reason = failure.what();
    } catch (...) {
        reason.clear();
    }
    return reason;
}

table::TableStateKey TableViews::partOf(table::TableStateKey key, std::string part) {
    key.part = std::move(part);
    return key;
}

std::optional<std::string> TableViews::take(Part& part, std::optional<table::StoredTableState> const& stored) {
    if (!stored) {
        part.document = {};
        part.revision.reset();
        return std::nullopt;
    }
    auto read = table::readViews(stored->json);
    if (!read) {
        return "The saved views could not be read: " + read.error().message;
    }
    part.document = std::move(*read);
    part.revision = stored->revision;
    return std::nullopt;
}

void TableViews::loadPart(std::shared_ptr<Held> const& held, Part Held::* member,
                          std::shared_ptr<Settle> const& settle) {
    auto const done = [settle](std::string failure) {
        if (settle->failure.empty()) {
            settle->failure = std::move(failure);
        }
        if (--settle->waiting > 0) {
            return;
        }
        if (settle->failure.empty()) {
            settle->promise.resolve(Result{});
        } else {
            settle->promise.resolve(Result{std::unexpected(settle->failure)});
        }
    };
    held->states->load(((*held).*member).key)
        .then([held, member, done](std::optional<table::StoredTableState> const& stored) {
            done(take((*held).*member, stored).value_or(std::string{}));
        })
        .onError([done](std::exception_ptr const& error) {
            std::string text = "The saved views could not be loaded";
            if (auto const cause = reasonOf(error); !cause.empty()) {
                text += ": " + cause;
            }
            done(std::move(text));
        });
}

async::Completion<TableViews::Result> TableViews::start(table::ViewsPart part, table::ViewOperation operation,
                                                        std::optional<table::SavedView> upsert) {
    auto [completion, promise] = async::Completion<Result>::makeSettleable(_held->owner);
    auto const saving = std::make_shared<Saving>(Saving{.held = _held,
                                                        .part = part,
                                                        .operation = std::move(operation),
                                                        .promise = std::move(promise),
                                                        .retried = false,
                                                        .upsert = std::move(upsert)});
    Part const& held = partIn(*_held, part);
    attempt(saving, held.revision.value_or(std::string{table::kNoDocument}));
    return std::move(completion);
}

void TableViews::attempt(std::shared_ptr<Saving> const& saving, std::string expected) {
    Part const& part = partIn(*saving->held, saving->part);
    // The document held is the one stored now after a reload, so a name another window took meanwhile is replaced
    // rather than refused; a store that applies the operation itself does so under the same revision.
    if (saving->upsert) {
        saving->operation = find(part.document, saving->upsert->name) != nullptr
                                ? table::ViewOperation{table::ReplaceView{*saving->upsert}}
                                : table::ViewOperation{table::AddView{*saving->upsert}};
    }
    auto applied = table::applyViewOperation(part.document, saving->operation, saving->part);
    if (!applied) {
        std::string text = table::viewErrorText(applied.error(), saving->operation);
        saving->promise.resolve(Result{
            std::unexpected(saving->part == table::ViewsPart::Own ? std::move(text) : failed(saving->part, text))});
        return;
    }
    table::ITableStateStore& store = saving->held->states->store();
    std::string json = table::writeViews(*applied, saving->part);
    auto sent = saving->part == table::ViewsPart::Own
                    ? store.save(part.key, json, std::move(expected))
                    : store.publish(part.key, saving->operation, std::move(expected));
    sent.then([saving, document = std::move(*applied), json](table::SaveResult const& result) mutable {
            answered(saving, std::move(document), std::move(json), result);
        })
        .onError([saving](std::exception_ptr const& error) {
            saving->promise.resolve(Result{std::unexpected(failed(saving->part, reasonOf(error)))});
        });
}

void TableViews::answered(std::shared_ptr<Saving> const& saving, table::ViewsDocument document, std::string json,
                          table::SaveResult const& result) {
    Part& part = partIn(*saving->held, saving->part);
    switch (result.outcome) {
        case table::SaveOutcome::Saved:
            part.document = std::move(document);
            part.revision = result.revision;
            saving->held->states->keep(part.key,
                                       table::StoredTableState{.json = std::move(json), .revision = result.revision});
            saving->promise.resolve(Result{});
            return;
        case table::SaveOutcome::Conflict:
            if (saving->retried) {
                saving->promise.resolve(
                    Result{std::unexpected(failed(saving->part, "another window changed them at the same time"))});
                return;
            }
            saving->retried = true;
            reload(saving);
            return;
        case table::SaveOutcome::Refused:
        default:
            saving->promise.resolve(Result{std::unexpected(failed(saving->part, result.reason))});
            return;
    }
}

void TableViews::reload(std::shared_ptr<Saving> const& saving) {
    Part const& part = partIn(*saving->held, saving->part);
    saving->held->states->store()
        .load(part.key)
        .then([saving](std::optional<table::StoredTableState> const& stored) {
            Part& reread = partIn(*saving->held, saving->part);
            if (auto failure = take(reread, stored)) {
                saving->promise.resolve(Result{std::unexpected(
                    saving->part == table::ViewsPart::Own ? std::move(*failure) : failed(saving->part, *failure))});
                return;
            }
            if (stored) {
                saving->held->states->keep(reread.key, *stored);
            }
            attempt(saving, stored ? stored->revision : std::string{});
        })
        .onError([saving](std::exception_ptr const& error) {
            saving->promise.resolve(Result{std::unexpected(failed(saving->part, reasonOf(error)))});
        });
}

std::optional<table::StateDocument> stateAtMount(std::optional<table::StateDocument> stored,
                                                 table::ViewsDocument const& own) {
    if (!own.defaultView) {
        return stored;
    }
    auto const view = std::ranges::find(own.views, *own.defaultView, &table::SavedView::name);
    if (view == own.views.end()) {
        return stored;
    }
    if (!stored) {
        table::StateDocument document;
        document.layout = view->layout;
        document.sort = view->sort;
        document.filters = view->filters;
        document.view = view->name;
        return document;
    }
    if (own.defaultOnMount) {
        stored->sort = view->sort;
        stored->filters = view->filters;
    }
    return stored;
}

}  // namespace morph::ui
