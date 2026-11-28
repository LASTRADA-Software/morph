// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/form_view.hpp
/// @brief `formView`: a `FormSession` rendered as a view tree (`ui::Node`), so one form draws on every frontend.
///
/// Specified in `docs/spec/forms/engine.md`, "Rendering".

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../../reactive/control.hpp"
#include "../../ui/grid.hpp"
#include "../../ui/view.hpp"
#include "../../util/datetime.hpp"
#include "../layout.hpp"
#include "detail/text.hpp"
#include "field_control.hpp"
#include "field_layout.hpp"
#include "field_model.hpp"
#include "form_session.hpp"
#include "overrides.hpp"

namespace morph::forms {

/// @brief How `formView` lays a form out and labels its chrome.
struct FormViewOptions {
    /// @brief Replacement controls.
    Overrides overrides{};
    /// @brief Columns of every section's, accordion's and tab's grid; `x-colspan` is clamped to it.
    int gridColumns = 2;
    /// @brief Columns of the implicit group's grid; unset is 1 while `gridColumns` is 2, else `gridColumns`.
    std::optional<int> flatGridColumns;
    /// @brief The explicit-mode button's label, which the status line names (`ready: press <label>`).
    ui::Prop<std::string> submitLabel = "Submit";
    /// @brief The caption of a collection's Add button.
    std::string addLabel = "Add";
    /// @brief The caption of a collection row's Remove button.
    std::string removeLabel = "Remove";
    /// @brief Whether the form's title heads the view.
    bool showTitle = true;
    /// @brief Whether the status line ("fill the required (*) fields", …) is shown.
    bool showStatus = true;
    /// @brief A heading over the fields (test id `form_title`) in place of the schema's title; empty shows none.
    ///        A title given is shown whatever `showTitle` says.
    ui::Prop<std::string> title;
    /// @brief Where each cell puts its field's caption: over the control, or beside it.
    FieldLayout fieldLayout = FieldLayout::Stacked;
    /// @brief The width of an inline caption, in backend units; at least 1, else `formView` throws
    ///        `std::invalid_argument`. Read only for `FieldLayout::Inline`.
    int inlineLabelWidth = 16;
};

namespace detail::engine {

/// @brief The shared view options a form's nodes hold.
using ViewOptions = std::shared_ptr<FormViewOptions const>;

/// @brief The status line under a form.
/// @param session     The session.
/// @param submitLabel The explicit-mode button's label, which the line names.
/// @return The line: what is missing, or how the form submits.
// NOLINTNEXTLINE(modernize-use-string-view): the line is a bound text property, which holds a string.
[[nodiscard]] inline std::string statusText(FormSession const& session, std::string_view submitLabel) {
    if (!session.ready()) {
        return "fill the required (*) fields";
    }
    return session.submitMode() == SubmitMode::Explicit ? "ready: press " + std::string{submitLabel}
                                                        : "submits as you type";
}

[[nodiscard]] ui::Node fieldCell(FormSession& session, FieldState& state, ViewOptions const& options);

/// @brief The built-in control of one field (the table in `docs/spec/forms/engine.md`, "Rendering").
/// @param session The session.
/// @param state   The field.
/// @param options The view options (members and rows draw their own cells with them).
/// @return The control.
// An Object or an ObjectArray draws its members' cells; every other kind is a scalar control.
// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
[[nodiscard]] inline ui::Node fieldControl(FormSession& session, FieldState& state, ViewOptions const& options) {
    FieldState* const field = &state;
    FieldSpec const& spec = state.spec();
    if ((spec.kind == FieldKind::Object || spec.kind == FieldKind::ObjectArray) && spec.truncated) {
        return ui::text(ui::Text{.text = "nested too deeply to edit here", .role = ui::TextRole::Muted});
    }
    if (spec.kind == FieldKind::Object) {
        std::vector<ui::Node> cells;
        for (auto const& member : state.members()) {
            cells.push_back(fieldCell(session, *member, options));
        }
        FieldState const* const object = &state;
        ui::Panel panel{.title = [object] { return object->label(); },
                        .padding = 4,
                        .child = ui::column(ui::Column{.children = std::move(cells)})};
        panel.common.testId = "field_" + state.path();
        return ui::panel(std::move(panel));
    }
    if (spec.kind == FieldKind::ObjectArray && spec.widget == "grid") {
        std::vector<ui::GridColumn> columns;
        columns.reserve(spec.children.size());
        for (FieldSpec const& member : spec.children) {
            columns.push_back(
                ui::GridColumn{.id = member.name, .label = member.title, .field = member.name, .value = {}});
        }
        return ui::formGrid(session, state, std::move(columns), {}, ui::formatOf(session));
    }
    if (spec.kind == FieldKind::ObjectArray) {
        FormSession* const owner = &session;
        ViewOptions const& shared = options;
        auto rows = ui::forEach<std::shared_ptr<RowState>>(
            [field] { return field->rows(); },
            [](std::shared_ptr<RowState> const& row) { return ui::Key{static_cast<std::int64_t>(row->key())}; },
            [field, owner, shared](reactive::Signal<std::shared_ptr<RowState>> const& slot) {
                std::shared_ptr<RowState> const& row = slot.peek();
                std::vector<ui::Node> cells;
                for (auto const& member : row->fields()) {
                    cells.push_back(fieldCell(*owner, *member, shared));
                }
                std::uint64_t const key = row->key();
                ui::Button remove{.label = shared->removeLabel, .onClick = [field, key] { field->removeRow(key); }};
                remove.common.testId = "remove_" + field->path() + "#" + std::to_string(key);
                cells.push_back(ui::button(std::move(remove)));
                // The title binding holds the row: it is the row view's first binding, so it is destroyed after
                // every binding inside the panel, and the row's state outlives everything that reads it after the
                // session has let it go.
                return ui::panel(ui::Panel{
                    .title =
                        [field, row] {
                            auto const& current = field->rows();
                            auto const found = std::ranges::find(current, row);
                            return field->label() + " " + std::to_string(found - current.begin() + 1);
                        },
                    .padding = 4,
                    .child = ui::column(ui::Column{.children = std::move(cells)}),
                });
            });
        ui::Button add{.label = options->addLabel, .onClick = [field] { field->addRow(); }};
        add.common.testId = "add_" + field->path();
        ui::Column column{.children = {std::move(rows), ui::button(std::move(add))}};
        column.common.testId = "field_" + state.path();
        return ui::column(std::move(column));
    }
    return scalarControl(session, state);
}

/// @brief Space as wide as an inline caption, which lines a Boolean's box or a help text up with the controls.
/// @param width The caption's width, in backend units.
/// @return The spacer.
[[nodiscard]] inline ui::Node captionSpace(int width) {
    ui::Spacer space{};
    space.common.layout = {.width = ui::Sizing::fixed(width), .height = {}};
    return ui::spacer(std::move(space));
}

/// @brief The inline form of a cell's content: a row centred across, the label at the caption's width (a spacer for
///        a Boolean, whose checkbox carries its label) and the control stretching. The help is the control's
///        description, under the control already; a control that does not read the field state (an override's)
///        has it as a second row under the control, indented by the caption's width.
/// @param state       The field.
/// @param control     The control.
/// @param options     The view options.
/// @param common      The cell's own visibility and test id.
/// @param helpOutside Whether the cell draws the help, because the control does not.
/// @return The row, or a column of the row and the help's row.
[[nodiscard]] inline ui::Node inlineCell(FieldState const& state, ui::Node control, ViewOptions const& options,
                                         ui::Common common, bool helpOutside) {
    FieldState const* const field = &state;
    int const width = options->inlineLabelWidth;
    ui::Node caption;
    if (state.spec().kind == FieldKind::Boolean) {
        caption = captionSpace(width);
    } else {
        ui::Text label{.text = [field] { return field->label(); }, .role = ui::TextRole::Label};
        label.common.testId = "label_" + state.path();
        label.common.layout = {.width = ui::Sizing::fixed(width), .height = {}};
        caption = ui::text(std::move(label));
    }
    // A column fills its one content child across, so the control takes the width the stretching column is given.
    ui::Column holder{.children = {std::move(control)}};
    holder.common.layout = {.width = ui::Sizing::stretch(), .height = {}};
    ui::Row line{
        .children = {std::move(caption), ui::column(std::move(holder))}, .gap = 3, .align = ui::Align::Center};
    if (!helpOutside || !state.hasHelp()) {
        line.common = std::move(common);
        return ui::row(std::move(line));
    }
    ui::Row help{.children = {captionSpace(width), ui::text(ui::Text{.text = [field] { return field->help(); },
                                                                     .role = ui::TextRole::Muted})},
                 .gap = 3};
    ui::Column cell{.children = {ui::row(std::move(line)), ui::row(std::move(help))}};
    cell.common = std::move(common);
    return ui::column(std::move(cell));
}

/// @brief One field's cell: the label, the control (an override's, else the built-in) and the help text; shown while
///        the field is visible. A built-in control draws the help as its description, so it is tied to the control
///        on every kit; a control an override draws, an Object's panel and an ObjectArray's rows are not one control
///        that reads the field state, so the cell draws their help as a muted text. The control marks a required
///        field, so the label carries no mark of its own. The options' `fieldLayout` puts the label over the control,
///        or beside it (`inlineCell`); an Object's panel and an ObjectArray's rows are stacked either way, and their
///        members follow the form's layout.
/// @param session The session.
/// @param state   The field.
/// @param options The view options.
/// @return The cell.
// NOLINTNEXTLINE(misc-no-recursion): nested controls draw cells.
inline ui::Node fieldCell(FormSession& session, FieldState& state, ViewOptions const& options) {
    FieldState const* const field = &state;
    ui::Node control;
    FieldKind const kind = state.spec().kind;
    bool helpOutside = kind == FieldKind::Object || kind == FieldKind::ObjectArray;
    if (Overrides::Render const* const render = options->overrides.resolve(state.spec())) {
        control = (*render)(FieldView{session, state, options});
        helpOutside = true;
    } else {
        control = fieldControl(session, state, options);
    }
    ui::Common common{};
    common.visible = [field] { return field->visible().get(); };
    common.testId = "cell_" + state.path();
    if (options->fieldLayout == FieldLayout::Inline && kind != FieldKind::Object && kind != FieldKind::ObjectArray) {
        return inlineCell(state, std::move(control), options, std::move(common), helpOutside);
    }
    std::vector<ui::Node> children;
    if (state.spec().kind != FieldKind::Boolean && state.spec().kind != FieldKind::Object) {
        ui::Text label{.text = [field] { return field->label(); }, .role = ui::TextRole::Label};
        label.common.testId = "label_" + state.path();
        children.push_back(ui::text(std::move(label)));
    }
    children.push_back(std::move(control));
    if (helpOutside && state.hasHelp()) {
        children.push_back(ui::text(ui::Text{.text = [field] { return field->help(); }, .role = ui::TextRole::Muted}));
    }
    ui::Column cell{.children = std::move(children), .common = std::move(common)};
    return ui::column(std::move(cell));
}

/// @brief The fields of one group on a grid of @p columns, each spanning `x-colspan` clamped to it.
/// @param session The session.
/// @param group   The group.
/// @param columns The grid's columns.
/// @param options The view options.
/// @return The grid.
[[nodiscard]] inline ui::Node groupGrid(FormSession& session, FieldGroupSpec const& group, int columns,
                                        ViewOptions const& options) {
    std::vector<ui::GridCell> cells;
    int const width = std::max(1, columns);
    for (std::string const& name : group.fields) {
        FieldState& state = session.field(name);
        cells.push_back(ui::GridCell{.node = fieldCell(session, state, options),
                                     .span = std::clamp(state.spec().colspan, 1, width)});
    }
    return ui::grid(ui::Grid{.columns = width, .cells = std::move(cells), .gap = 4});
}

}  // namespace detail::engine

inline ui::Node FieldView::defaultControl() const {
    return detail::engine::fieldControl(*_session, *_state, _options);
}

/// @brief Renders @p session: the title (the options' `title`, else the schema's while `showTitle`), its fields laid
///        out by group (sections as panels, consecutive tab groups as one tab bar, accordions as collapsible panels,
///        the implicit group as a grid), the status line, the Submit button in explicit mode, and the last reply or
///        error.
///
/// The nodes bind to the session and its fields by pointer: mount the result after the session and destroy the
/// mount before it.
/// @param session The session.
/// @param options Overrides, grid columns, the chrome's labels, the field layout.
/// @return The view.
/// @throws std::invalid_argument when `options.inlineLabelWidth` is below 1.
[[nodiscard]] inline ui::Node formView(FormSession& session, FormViewOptions options = {}) {
    using detail::engine::groupGrid;
    if (options.inlineLabelWidth < 1) {
        throw std::invalid_argument{"morph::forms::formView: an inline label is at least one unit wide"};
    }
    auto const shared = std::make_shared<FormViewOptions const>(std::move(options));
    int const columns = std::max(1, shared->gridColumns);
    int const flatColumns = shared->flatGridColumns.value_or(columns == 2 ? 1 : columns);
    FormSession* const form = &session;
    std::vector<ui::Node> children;
    if (detail::engine::isSet(shared->title)) {
        ui::Text title{.text = shared->title, .role = ui::TextRole::Heading};
        title.common.testId = "form_title";
        children.push_back(ui::text(std::move(title)));
    } else if (shared->showTitle) {
        children.push_back(
            ui::text(ui::Text{.text = std::string{session.model().title()}, .role = ui::TextRole::Heading}));
    }
    auto const groups = session.model().groups();
    for (std::size_t i = 0; i < groups.size();) {
        FieldGroupSpec const& group = detail::engine::elementAt(groups, i);
        if (group.implicit) {
            children.push_back(groupGrid(session, group, flatColumns, shared));
            ++i;
        } else if (group.kind == GroupKind::Tab) {
            std::size_t const first = i;
            std::vector<ui::Tab> tabs;
            while (i < groups.size() && detail::engine::elementAt(groups, i).kind == GroupKind::Tab &&
                   !detail::engine::elementAt(groups, i).implicit) {
                tabs.push_back(
                    ui::Tab{.label = [owner = &session, i] { return owner->groupTitle(i); },
                            .node = groupGrid(session, detail::engine::elementAt(groups, i), columns, shared)});
                ++i;
            }
            reactive::Signal<std::size_t>* const selected = &session.tabSelection(first);
            children.push_back(
                ui::tabs(ui::Tabs{.tabs = std::move(tabs),
                                  .selected = [selected] { return selected->get(); },
                                  .onSelect = [selected](std::size_t index) { selected->set(index); }}));
        } else if (group.kind == GroupKind::Accordion) {
            reactive::Signal<bool>* const collapsed = &session.groupCollapsed(i);
            children.push_back(ui::panel(ui::Panel{.title = [owner = &session, i] { return owner->groupTitle(i); },
                                                   .padding = 4,
                                                   .child = groupGrid(session, group, columns, shared),
                                                   .collapsible = true,
                                                   .collapsed = [collapsed] { return collapsed->get(); },
                                                   .onToggle = [collapsed](bool value) { collapsed->set(value); }}));
            ++i;
        } else {
            children.push_back(ui::panel(ui::Panel{.title = [owner = &session, i] { return owner->groupTitle(i); },
                                                   .padding = 4,
                                                   .child = groupGrid(session, group, columns, shared)}));
            ++i;
        }
    }
    if (shared->showStatus) {
        ui::Text status{
            .text = [form, shared] { return detail::engine::statusText(*form, shared->submitLabel.evaluate()); },
            .role = ui::TextRole::Muted};
        status.common.testId = "form_status";
        children.push_back(ui::text(std::move(status)));
    }
    if (session.submitMode() == SubmitMode::Explicit) {
        ui::Button submit{.label = shared->submitLabel, .onClick = [form] { form->submit(); }};
        submit.common.enabled = [form] { return form->ready() && !form->pending(); };
        submit.common.testId = "form_submit";
        children.push_back(ui::button(std::move(submit)));
    }
    ui::Text result{
        .text = [form] { return detail::engine::resultText(*form); },
        .role = [form] { return form->lastError() != nullptr ? ui::TextRole::Error : ui::TextRole::Success; },
    };
    // A quiet form still reports a failure: nothing else on it would say the save did not happen.
    result.common.visible = [form, shared] {
        return form->lastError() != nullptr || (shared->showStatus && form->lastReply().has_value());
    };
    result.common.testId = "form_result";
    children.push_back(ui::text(std::move(result)));
    return ui::column(ui::Column{.children = std::move(children), .gap = 4});
}

}  // namespace morph::forms
