// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/collection_view.hpp
/// @brief `collectionView`: a `CollectionSession` rendered as a view tree (`ui::Node`): the title and collection
///        actions, a table of the rows with their actions, the confirm dialog and the row editor, as a dialog or
///        beside the table.
///
/// Specified in `docs/spec/forms/engine.md`, "Rendering collections and wizards".

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../reactive/control.hpp"
#include "../../reactive/signal.hpp"
#include "../../ui/view.hpp"
#include "collection.hpp"
#include "form_view.hpp"

namespace morph::forms {

/// @brief Replacements for the parts of a collection view a host draws from its own UI kit. Each one left empty
///        keeps the built-in part.
struct CollectionChrome {
    /// @brief Replaces the title and the collection actions' buttons. Called once, with the session, whose model
    ///        gives the title, columns and actions and whose `fire` runs one.
    std::function<ui::Node(CollectionSession&)> header;
    /// @brief Replaces each row's cells and buttons: the rows are then a list of these nodes rather than a table.
    ///        Called once per row mounted, with the session and the row's signal (read it inside bindings).
    std::function<ui::Node(CollectionSession&, reactive::Signal<CollectionRow> const&)> row;
    /// @brief Replaces the confirm dialog: mounted while an action waits for confirmation, unmounted once it is
    ///        answered. Called with the session, whose `pendingConfirm`, `confirm` and `cancelConfirm` it uses.
    std::function<ui::Node(CollectionSession&)> confirm;
    /// @brief Replaces the editor dialog of a `collection` view; it is always mounted, and shows the editor while
    ///        `openRow()` is engaged. Called once with the session and the editor's form view, which it places.
    ///        A master-detail view has no dialog and does not call it.
    std::function<ui::Node(CollectionSession&, ui::Node editor)> editor;
};

/// @brief How `collectionView` draws a collection and labels its chrome.
struct CollectionViewOptions {
    /// @brief The editor's form view options; its overrides reach the editor's fields.
    FormViewOptions form{};
    /// @brief The host's replacements for the view's own parts.
    CollectionChrome chrome{};
    /// @brief The caption of a row's Open button.
    std::string openLabel = "Open";
    /// @brief The confirm dialog's title.
    std::string confirmTitle = "Confirm";
    /// @brief The confirm dialog's question.
    std::string confirmMessage = "Are you sure?";
    /// @brief The confirm dialog's accept button.
    std::string acceptLabel = "OK";
    /// @brief The confirm dialog's cancel button.
    std::string cancelLabel = "Cancel";
    /// @brief The built-in header's title (`collection_title`); empty shows the schema's.
    ui::Prop<std::string> title{};
};

namespace detail::engine {

/// @brief The error line of a collection: the list's failure, else the last action's.
/// @param session The session.
/// @return `error: <message>`, or empty.
[[nodiscard]] inline std::string collectionErrorText(CollectionSession const& session) {
    std::exception_ptr error = session.listError();
    if (error == nullptr) {
        error = session.actionError();
    }
    return error == nullptr ? std::string{} : "error: " + reactive::errorMessage(error);
}

/// @brief The indices of a collection's actions with one scope, in document order.
/// @param model The model.
/// @param scope The scope.
/// @return The indices into `model.actions()`.
[[nodiscard]] inline std::vector<std::size_t> actionsOf(CollectionModel const& model, CollectionActionScope scope) {
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < model.actions().size(); ++i) {
        if (elementAt(model.actions(), i).scope == scope) {
            indices.push_back(i);
        }
    }
    return indices;
}

/// @brief The built-in header: the title, a busy indicator while the list loads, and the collection actions.
/// @param session The session.
/// @param heading The title in place of the schema's; empty shows the schema's.
/// @return The header.
[[nodiscard]] inline ui::Node collectionHeader(CollectionSession& session, ui::Prop<std::string> const& heading = {}) {
    CollectionSession* const view = &session;
    ui::Text title{.text = isSet(heading) ? heading : ui::Prop<std::string>{std::string{session.model().title()}},
                   .role = ui::TextRole::Heading};
    title.common.testId = "collection_title";
    std::vector<ui::Node> children{
        ui::text(std::move(title)),
        ui::busy(ui::Busy{.active = [view] { return view->loading(); }, .label = std::string{}}),
    };
    for (std::size_t const index : actionsOf(session.model(), CollectionActionScope::Collection)) {
        CollectionAction const& action = elementAt(session.model().actions(), index);
        ui::Button button{.label = action.label, .onClick = [view, index] { view->fire(index); }};
        button.common.enabled = [view] { return !view->actionPending(); };
        button.common.testId = "collectionAction_" + action.action;
        children.push_back(ui::button(std::move(button)));
    }
    return ui::row(ui::Row{.children = std::move(children), .gap = 4});
}

/// @brief A row's built-in buttons: Open (with a row action) and each row action.
/// @param session The session.
/// @param rowKey  The row's key text.
/// @param options The view options.
/// @return The buttons, in a row.
[[nodiscard]] inline ui::Node rowButtons(CollectionSession& session, std::string const& rowKey,
                                         CollectionViewOptions const& options) {
    CollectionSession* const view = &session;
    std::vector<ui::Node> buttons;
    if (session.model().rowAction().has_value()) {
        ui::Button open{.label = options.openLabel, .onClick = [view, rowKey] { view->open(rowKey); }};
        open.common.enabled = [view] { return view->editor() != nullptr; };
        open.common.testId = "rowOpen_" + rowKey;
        buttons.push_back(ui::button(std::move(open)));
    }
    for (std::size_t const index : actionsOf(session.model(), CollectionActionScope::Row)) {
        CollectionAction const& action = elementAt(session.model().actions(), index);
        ui::Button button{.label = action.label, .onClick = [view, index, rowKey] { view->fire(index, rowKey); }};
        button.common.enabled = [view] { return !view->actionPending(); };
        button.common.testId = "rowAction_" + action.action + "_" + rowKey;
        buttons.push_back(ui::button(std::move(button)));
    }
    return ui::row(ui::Row{.children = std::move(buttons), .gap = 4});
}

/// @brief The rows table's activation and selection: activating a row opens it when the view has a row action; in a
///        master-detail view a row is selected while the editor is open on it, and selecting one opens it.
/// @param session The session.
/// @return The options, with the table's test id.
[[nodiscard]] inline ui::TableOptions rowsTableOptions(CollectionSession& session) {
    CollectionSession* const view = &session;
    ui::TableOptions table{};
    if (session.model().rowAction().has_value()) {
        table.onActivate = [view](ui::Key const& key) {
            if (auto const* const text = std::get_if<std::string>(&key)) {
                view->open(*text);
            }
        };
    }
    if (session.model().kind() == CollectionKind::MasterDetail) {
        table.selectionMode = ui::SelectionMode::Single;
        table.selection = [view] {
            std::optional<std::string> const& open = view->openRow();
            return open.has_value() ? std::vector<ui::Key>{ui::Key{*open}} : std::vector<ui::Key>{};
        };
        table.onSelectionChange = [view](std::vector<ui::Key> const& keys) {
            auto const* const text = keys.empty() ? nullptr : std::get_if<std::string>(&keys.front());
            if (text == nullptr) {
                view->close();
            } else {
                view->open(*text);
            }
        };
    }
    table.common.testId = "collection_rows";
    return table;
}

/// @brief The rows: a table of the visible columns' cells and the row buttons, or a list of the row chrome's nodes.
///        In a master-detail view a row is selected while the editor is open on it, and selecting one opens it.
/// @param session The session.
/// @param options The view options, shared with the row views.
/// @return The rows.
[[nodiscard]] inline ui::Node collectionRows(CollectionSession& session,
                                             std::shared_ptr<CollectionViewOptions const> const& options) {
    CollectionSession* const view = &session;
    auto const rows = [view] { return view->rows(); };
    auto const keyOf = [](CollectionRow const& row) { return ui::Key{row.key}; };
    if (options->chrome.row) {
        ui::Node list = ui::forEach<CollectionRow>(
            rows, keyOf,
            [view, options](reactive::Signal<CollectionRow> const& slot) { return options->chrome.row(*view, slot); });
        return list;
    }
    std::vector<ui::TableColumn> columns;
    std::vector<std::string> fields;
    for (CollectionColumn const& column : session.model().columns()) {
        if (!column.hidden) {
            columns.push_back(ui::TableColumn{.label = column.label});
            fields.push_back(column.field);
        }
    }
    bool const buttons =
        session.model().rowAction().has_value() || !actionsOf(session.model(), CollectionActionScope::Row).empty();
    if (buttons) {
        columns.push_back(ui::TableColumn{.label = std::string{}});
    }
    ui::TableOptions table = rowsTableOptions(session);
    return ui::table<CollectionRow>(
        std::move(columns), rows, keyOf,
        [view, options, fields, buttons](reactive::Signal<CollectionRow> const& slot) {
            reactive::Signal<CollectionRow> const* const row = &slot;
            std::string const key = slot.peek().key;
            std::vector<ui::Node> cells;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                ui::Text cell{.text = [row, i] {
                    std::vector<std::string> const& texts = row->get().cells;
                    return i < texts.size() ? texts.at(i) : std::string{};
                }};
                cell.common.testId = "cell_" + fields.at(i) + "_" + key;
                cells.push_back(ui::text(std::move(cell)));
            }
            if (buttons) {
                cells.push_back(rowButtons(*view, key, *options));
            }
            return cells;
        },
        std::move(table));
}

/// @brief The built-in confirm dialog: open while an action waits, its question and the accept and cancel buttons.
/// @param session The session.
/// @param options The view options.
/// @return The dialog.
[[nodiscard]] inline ui::Node confirmDialog(CollectionSession& session, CollectionViewOptions const& options) {
    CollectionSession* const view = &session;
    ui::Button accept{.label = options.acceptLabel, .onClick = [view] { view->confirm(); }};
    accept.common.testId = "confirm_accept";
    ui::Button cancel{.label = options.cancelLabel, .onClick = [view] { view->cancelConfirm(); }};
    cancel.common.testId = "confirm_cancel";
    ui::Text message{.text = options.confirmMessage};
    message.common.testId = "confirm_message";
    ui::Dialog dialog{
        .open = [view] { return view->pendingConfirm().has_value(); },
        .title = options.confirmTitle,
        .child = ui::column(ui::Column{
            .children = {ui::text(std::move(message)),
                         ui::row(ui::Row{.children = {ui::button(std::move(accept)), ui::button(std::move(cancel))},
                                         .gap = 4})},
            .gap = 4}),
        .onDismiss = [view] { view->cancelConfirm(); },
    };
    dialog.common.testId = "confirmDialog";
    return ui::dialog(std::move(dialog));
}

/// @brief The confirm chrome, mounted only while an action waits.
/// @param session The session.
/// @param chrome  The host's confirm chrome.
/// @return A switch showing it while `pendingConfirm()` is engaged.
[[nodiscard]] inline ui::Node confirmChrome(CollectionSession& session,
                                            std::function<ui::Node(CollectionSession&)> const& chrome) {
    CollectionSession const* const view = &session;
    return ui::switchOf(ui::Switch{
        .selector = [view] { return ui::Key{std::int64_t{view->pendingConfirm().has_value() ? 1 : 0}}; },
        .cases = {ui::SwitchCase{.key = ui::Key{std::int64_t{1}}, .node = chrome(session)}},
    });
}

}  // namespace detail::engine

/// @brief Renders @p session: the header (title, busy indicator, collection actions), the error line, the rows as a
///        table (visible columns' exact cells, then Open and the row actions), the confirm dialog, and the row
///        editor (`formView` of `editor()`) as a dialog for a `collection`, or beside the table, shown while a row
///        is open, for a `master-detail` view.
///
/// Test ids: `collection_title`, `collectionAction_<Action>`, `collection_error`, `collection_rows`,
/// `cell_<field>_<rowKey>`, `rowOpen_<rowKey>`, `rowAction_<Action>_<rowKey>`, `confirmDialog`, `confirm_message`,
/// `confirm_accept`, `confirm_cancel`, `editorDialog` and `detailForm`; the editor's fields have their form ids.
///
/// The nodes bind to the session by pointer: mount the result after the session and destroy the mount before it.
/// @param session The session.
/// @param options The editor's form options, the chrome and the labels.
/// @return The view.
[[nodiscard]] inline ui::Node collectionView(CollectionSession& session, CollectionViewOptions options = {}) {
    auto const shared = std::make_shared<CollectionViewOptions const>(std::move(options));
    CollectionSession* const view = &session;
    std::vector<ui::Node> list;
    list.push_back(shared->chrome.header ? shared->chrome.header(session)
                                         : detail::engine::collectionHeader(session, shared->title));
    ui::Text error{.text = [view] { return detail::engine::collectionErrorText(*view); }, .role = ui::TextRole::Error};
    error.common.visible = [view] { return view->listError() != nullptr || view->actionError() != nullptr; };
    error.common.testId = "collection_error";
    list.push_back(ui::text(std::move(error)));
    list.push_back(detail::engine::collectionRows(session, shared));
    list.push_back(shared->chrome.confirm ? detail::engine::confirmChrome(session, shared->chrome.confirm)
                                          : detail::engine::confirmDialog(session, *shared));
    FormSession* const editor = session.editor();
    bool const masterDetail = session.model().kind() == CollectionKind::MasterDetail;
    if (editor != nullptr && !masterDetail) {
        ui::Node form = formView(*editor, shared->form);
        if (shared->chrome.editor) {
            list.push_back(shared->chrome.editor(session, std::move(form)));
        } else {
            ui::Dialog dialog{
                .open = [view] { return view->openRow().has_value(); },
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): an editor exists only for a row action.
                .title = session.model().rowAction().value().action,
                .child = std::move(form),
                .onDismiss = [view] { view->close(); },
            };
            dialog.common.testId = "editorDialog";
            list.push_back(ui::dialog(std::move(dialog)));
        }
    }
    ui::Node column = ui::column(ui::Column{.children = std::move(list), .gap = 4});
    if (editor == nullptr || !masterDetail) {
        return column;
    }
    ui::Column detail{.children = {formView(*editor, shared->form)}};
    detail.common.visible = [view] { return view->openRow().has_value(); };
    detail.common.testId = "detailForm";
    return ui::row(ui::Row{.children = {std::move(column), ui::column(std::move(detail))}, .gap = 5});
}

}  // namespace morph::forms
