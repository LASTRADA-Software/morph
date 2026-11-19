// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file table_views.hpp
/// @brief A table's saved views: the user's own and the shared ones, loaded with the table's state and saved under
///        the store's revision rule, so two windows saving at once lose neither's view.

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "../core/completion.hpp"
#include "../core/executor.hpp"
#include "../table/state_store.hpp"
#include "table_state.hpp"

namespace morph::ui {

/// @brief The saved views of one table: the parts `"views"` (the user's own) and `"shared"` (published for everyone)
///        of its persisted state.
///
/// `load` goes through the session's `TableStates`, so the views join the turn's one batched load with the screen's
/// `state` keys, and a second mount reads them from the kept copies with no call. A change applies an operation to
/// the views loaded and saves the whole document through the store, expecting the revision loaded, or `kNoDocument`
/// when nothing was: a window that saves over another window's newer document gets `Conflict`, reads the document
/// again, applies its operation to that one and saves once more, so neither window's view is lost. A table that is
/// mounted does not follow saves made elsewhere until its next conflict or mount.
///
/// Every member is called on the session's owner, where every completion it returns is delivered. The object may go
/// before a change it started completes; the change still completes.
class TableViews {
public:
    /// @brief What a load or a change answers: nothing, or the words a status line shows.
    using Result = std::expected<void, std::string>;

    /// @brief The views of the table @p key names.
    /// @param states The session's table states; must outlive the object and every change it starts.
    /// @param key    The table's state key; its `part` is ignored.
    /// @param owner  The session's owner, where completions are delivered. Borrowed: it must outlive them.
    TableViews(TableStates& states, table::TableStateKey key, exec::IExecutor& owner)
        : _held{std::make_shared<Held>(Held{.states = &states,
                                            .owner = &owner,
                                            .own = Part{.key = partOf(key, "views"), .document = {}, .revision = {}},
                                            .shared = Part{.key = partOf(key, "shared"), .document = {},
                                                           .revision = {}}})} {}

    /// @brief Loads the user's views, and the shared ones when the store shares.
    /// @return Completes when every part is read; the reason a part could not be read otherwise.
    [[nodiscard]] async::Completion<Result> load() {
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_held->owner);
        auto const settle = std::make_shared<Settle>(Settle{.promise = std::move(promise), .waiting = 1, .failure = {}});
        bool const shares = this->shares();
        settle->waiting = shares ? 2 : 1;
        loadPart(_held, &Held::own, settle);
        if (shares) {
            loadPart(_held, &Held::shared, settle);
        }
        return std::move(completion);
    }

    /// @brief The user's own views as last loaded or saved.
    /// @return The document.
    [[nodiscard]] table::ViewsDocument const& own() const noexcept { return _held->own.document; }

    /// @brief The shared views as last loaded or published.
    /// @return The document; empty when the store does not share.
    [[nodiscard]] table::ViewsDocument const& shared() const noexcept { return _held->shared.document; }

    /// @brief Whether the store keeps a shared part for everyone.
    /// @return `states.store().shares()`.
    [[nodiscard]] bool shares() const { return _held->states->store().shares(); }

    /// @brief Applies @p operation to the user's views and saves them.
    ///
    /// An operation that does not apply answers at once and saves nothing. A save that meets another window's
    /// document reads it again and saves the operation over it once more: with that document's revision, or with an
    /// empty `expected` when the store holds nothing, which a store that does not honour `kNoDocument` answers with
    /// `Conflict`. A second conflict answers "Views could not be saved: another window changed them at the same
    /// time".
    /// @param operation The operation.
    /// @return Nothing once saved; the words a status line shows otherwise.
    [[nodiscard]] async::Completion<Result> change(table::ViewOperation operation) {
        return start(table::ViewsPart::Own, std::move(operation));
    }

    /// @brief Applies @p operation to the shared views through the store's `publish`, under the same rule as `change`.
    /// @param operation The operation.
    /// @return Nothing once published; the words a status line shows otherwise.
    [[nodiscard]] async::Completion<Result> publish(table::ViewOperation operation) {
        return start(table::ViewsPart::Shared, std::move(operation));
    }

private:
    // One part's key, the document read or saved last, and its revision; nothing when nothing is stored.
    struct Part {
        table::TableStateKey key;
        table::ViewsDocument document;
        std::optional<std::string> revision;
    };

    // What a change still running needs after the object is gone.
    struct Held {
        TableStates* states = nullptr;
        exec::IExecutor* owner = nullptr;
        Part own;
        Part shared;
    };

    // A load waiting for its parts.
    struct Settle {
        async::Completion<Result>::Promise promise;
        int waiting = 0;
        std::string failure;
    };

    // One operation being saved to one part.
    struct Saving {
        std::shared_ptr<Held> held;
        table::ViewsPart part = table::ViewsPart::Own;
        table::ViewOperation operation;
        async::Completion<Result>::Promise promise;
        bool retried = false;
    };

    [[nodiscard]] static table::TableStateKey partOf(table::TableStateKey key, std::string part) {
        key.part = std::move(part);
        return key;
    }

    [[nodiscard]] static Part& partIn(Held& held, table::ViewsPart part) {
        return part == table::ViewsPart::Own ? held.own : held.shared;
    }

    // Takes what the store holds for a part: its document and revision, or none.
    [[nodiscard]] static std::optional<std::string> take(Part& part, std::optional<table::StoredTableState> const& stored) {
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

    static void loadPart(std::shared_ptr<Held> const& held, Part Held::* member, std::shared_ptr<Settle> const& settle) {
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
                try {
                    std::rethrow_exception(error);
                } catch (std::exception const& failure) {
                    text += std::string{": "} + failure.what();
                } catch (...) {
                }
                done(std::move(text));
            });
    }

    [[nodiscard]] async::Completion<Result> start(table::ViewsPart part, table::ViewOperation operation) {
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_held->owner);
        auto saving = std::make_shared<Saving>(Saving{
            .held = _held, .part = part, .operation = std::move(operation), .promise = std::move(promise), .retried = false});
        Part const& held = partIn(*_held, part);
        attempt(saving, held.revision.value_or(std::string{table::kNoDocument}));
        return std::move(completion);
    }

    // Applies the operation to the part as held and sends it, expecting @p expected.
    static void attempt(std::shared_ptr<Saving> const& saving, std::string expected) {
        Part& part = partIn(*saving->held, saving->part);
        auto applied = table::applyViewOperation(part.document, saving->operation, saving->part);
        if (!applied) {
            saving->promise.resolve(Result{std::unexpected(table::viewErrorText(applied.error(), saving->operation))});
            return;
        }
        table::ITableStateStore& store = saving->held->states->store();
        std::string json = table::writeViews(*applied, saving->part);
        auto sent = saving->part == table::ViewsPart::Own ? store.save(part.key, json, std::move(expected))
                                                          : store.publish(part.key, saving->operation, std::move(expected));
        sent.then([saving, document = std::move(*applied), json](table::SaveResult const& result) mutable {
                answered(saving, std::move(document), std::move(json), result);
            })
            .onError([saving](std::exception_ptr const& error) {
                std::string text = "Views could not be saved";
                try {
                    std::rethrow_exception(error);
                } catch (std::exception const& failure) {
                    text += std::string{": "} + failure.what();
                } catch (...) {
                }
                saving->promise.resolve(Result{std::unexpected(std::move(text))});
            });
    }

    static void answered(std::shared_ptr<Saving> const& saving, table::ViewsDocument document, std::string json,
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
                    saving->promise.resolve(Result{std::unexpected(
                        std::string{"Views could not be saved: another window changed them at the same time"})});
                    return;
                }
                saving->retried = true;
                reload(saving);
                return;
            case table::SaveOutcome::Refused:
            default:
                saving->promise.resolve(Result{std::unexpected(
                    result.reason.empty() ? std::string{"Views could not be saved"}
                                          : "Views could not be saved: " + result.reason)});
                return;
        }
    }

    // Reads the part again from the store itself, since the session's kept copy is the one that conflicted, keeps
    // what it read, and sends the operation once more over it.
    static void reload(std::shared_ptr<Saving> const& saving) {
        Part& part = partIn(*saving->held, saving->part);
        saving->held->states->store()
            .load(part.key)
            .then([saving](std::optional<table::StoredTableState> const& stored) {
                Part& reread = partIn(*saving->held, saving->part);
                if (auto failure = take(reread, stored)) {
                    saving->promise.resolve(Result{std::unexpected(std::move(*failure))});
                    return;
                }
                if (stored) {
                    saving->held->states->keep(reread.key, *stored);
                }
                attempt(saving, stored ? stored->revision : std::string{});
            })
            .onError([saving](std::exception_ptr const&) {
                saving->promise.resolve(Result{std::unexpected(std::string{"Views could not be saved"})});
            });
    }

    std::shared_ptr<Held> _held;
};

}  // namespace morph::ui
