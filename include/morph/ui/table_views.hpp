// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file table_views.hpp
/// @brief A table's saved views: the user's own and the shared ones, loaded with the table's state and saved under
///        the store's revision rule, so two windows saving at once lose neither's view.

#include <algorithm>
#include <exception>
#include <expected>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
class MORPH_API TableViews {
public:
    /// @brief What a load or a change answers: nothing, or the words a status line shows.
    using Result = std::expected<void, std::string>;

    /// @brief The views of the table @p key names.
    /// @param states The session's table states; must outlive the object and every change it starts.
    /// @param key    The table's state key; its `part` is ignored.
    /// @param owner  The session's owner, where completions are delivered. Borrowed: it must outlive them.
    TableViews(TableStates& states, table::TableStateKey const& key, exec::IExecutor& owner)
        : _held{std::make_shared<Held>(
              Held{.states = &states,
                   .owner = &owner,
                   .own = Part{.key = partOf(key, "views"), .document = {}, .revision = {}},
                   .shared = Part{.key = partOf(key, "shared"), .document = {}, .revision = {}}})} {}

    /// @brief Loads the user's views, and the shared ones when the store shares.
    /// @return Completes when every part is read; the reason a part could not be read otherwise.
    [[nodiscard]] async::Completion<Result> load();

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
    /// @return Nothing once published; otherwise the words a status line shows, "This view could not be published:"
    ///         and the reason.
    [[nodiscard]] async::Completion<Result> publish(table::ViewOperation operation) {
        return start(table::ViewsPart::Shared, std::move(operation));
    }

    /// @brief Saves @p view among the user's views: an `AddView`, or a `ReplaceView` when a view of its name is in
    ///        the document the operation is applied to, which after a `Conflict` is the one stored now.
    /// @param view The view.
    /// @return Nothing once saved; the words a status line shows otherwise.
    [[nodiscard]] async::Completion<Result> saveView(table::SavedView view) {
        return start(table::ViewsPart::Own, table::AddView{view}, view);
    }

    /// @brief Publishes @p view among the shared views, as `saveView` saves one among the user's.
    /// @param view The view.
    /// @return Nothing once published; otherwise "This view could not be published:" and the reason.
    [[nodiscard]] async::Completion<Result> publishView(table::SavedView view) {
        return start(table::ViewsPart::Shared, table::AddView{view}, view);
    }

    /// @brief One of the user's own views.
    /// @param name The view's name.
    /// @return It, as last loaded or saved; null when there is none of that name.
    [[nodiscard]] table::SavedView const* ownView(std::string_view name) const { return find(own(), name); }

    /// @brief One of the shared views.
    /// @param name The view's name.
    /// @return It, as last loaded or published; null when there is none of that name.
    [[nodiscard]] table::SavedView const* sharedView(std::string_view name) const { return find(shared(), name); }

private:
    [[nodiscard]] static table::SavedView const* find(table::ViewsDocument const& document, std::string_view name);

    // What a failure to change @p part says: a publish's failure is said as one, whatever stopped it.
    [[nodiscard]] static std::string failed(table::ViewsPart part, std::string const& reason);

    // The words of an exception a store's completion failed with.
    [[nodiscard]] static std::string reasonOf(std::exception_ptr const& error);

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
        /// A view added or replaced, decided at each attempt against the document it is applied to.
        std::optional<table::SavedView> upsert;
    };

    [[nodiscard]] static table::TableStateKey partOf(table::TableStateKey key, std::string part);

    [[nodiscard]] static Part& partIn(Held& held, table::ViewsPart part) {
        return part == table::ViewsPart::Own ? held.own : held.shared;
    }

    // Takes what the store holds for a part: its document and revision, or none.
    [[nodiscard]] static std::optional<std::string> take(Part& part,
                                                         std::optional<table::StoredTableState> const& stored);

    static void loadPart(std::shared_ptr<Held> const& held, Part Held::* member,
                         std::shared_ptr<Settle> const& settle);

    [[nodiscard]] async::Completion<Result> start(table::ViewsPart part, table::ViewOperation operation,
                                                  std::optional<table::SavedView> upsert = std::nullopt);

    // Applies the operation to the part as held and sends it, expecting @p expected.
    static void attempt(std::shared_ptr<Saving> const& saving, std::string expected);

    static void answered(std::shared_ptr<Saving> const& saving, table::ViewsDocument document, std::string json,
                         table::SaveResult const& result);

    // Reads the part again from the store itself, since the session's kept copy is the one that conflicted, keeps
    // what it read, and sends the operation once more over it.
    static void reload(std::shared_ptr<Saving> const& saving);

    std::shared_ptr<Held> _held;
};

/// @brief The state a table starts from at mount, given its user's own views.
///
/// With no stored state, the default view's layout, sort and filters, and its name as the state's `view`. With a
/// stored state and `defaultOnMount`, the stored state with the default view's sort and filters in place of its own;
/// its layout and `view` stay. Otherwise, and whenever the default names no view, the stored state as it is. The
/// result is fitted to the table as a stored state is.
/// @param stored The stored state; none when nothing is stored.
/// @param own    The user's own views.
/// @return The state to apply; none when there is nothing to apply.
[[nodiscard]] MORPH_API std::optional<table::StateDocument> stateAtMount(std::optional<table::StateDocument> stored,
                                                                         table::ViewsDocument const& own);

}  // namespace morph::ui
