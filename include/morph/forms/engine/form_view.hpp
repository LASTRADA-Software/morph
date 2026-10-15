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
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../../reactive/control.hpp"
#include "../../ui/view.hpp"
#include "../../util/datetime.hpp"
#include "../layout.hpp"
#include "detail/text.hpp"
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
    /// @brief The explicit-mode button's label.
    std::string submitLabel = "Submit";
    /// @brief The caption of a collection's Add button.
    std::string addLabel = "Add";
    /// @brief The caption of a collection row's Remove button.
    std::string removeLabel = "Remove";
    /// @brief Whether the form's title heads the view.
    bool showTitle = true;
    /// @brief Whether the status line ("fill the required (*) fields", …) is shown.
    bool showStatus = true;
};

namespace detail::engine {

/// @brief The shared view options a form's nodes hold.
using ViewOptions = std::shared_ptr<FormViewOptions const>;

/// @brief The status line under a form.
/// @param session The session.
/// @return The line: what is missing, or how the form submits.
// NOLINTNEXTLINE(modernize-use-string-view): the line is a bound text property, which holds a string.
[[nodiscard]] inline std::string statusText(FormSession const& session) {
    if (!session.ready()) {
        return "fill the required (*) fields";
    }
    return session.submitMode() == SubmitMode::Explicit ? "ready: press Submit" : "submits as you type";
}

/// @brief The last reply or failure.
/// @param session The session.
/// @return `error: <message>`, `ok: <reply>`, or empty before the first answer.
[[nodiscard]] inline std::string resultText(FormSession const& session) {
    if (std::exception_ptr const error = session.lastError()) {
        return "error: " + reactive::errorMessage(error);
    }
    std::optional<std::string> const& reply = session.lastReply();
    return reply.has_value() ? "ok: " + *reply : std::string{};
}

/// @brief A draft wall clock (`YYYY-MM-DDTHH:MM[:SS]`, or a `YYYY-MM-DD` date) as the instant it denotes.
/// @param text          The draft.
/// @param date          Whether the field holds a date.
/// @param offsetMinutes The display zone.
/// @return The instant, or `std::nullopt` while the draft is not a complete one.
[[nodiscard]] inline std::optional<time::Timestamp> draftInstant(std::string_view text, bool date, int offsetMinutes) {
    std::optional<std::chrono::sys_seconds> local;
    if (date) {
        if (isIsoDate(text)) {
            local = parseWallClock(std::string{text} + "T00:00");
        }
    } else {
        local = parseWallClock(text);
    }
    if (!local) {
        return std::nullopt;
    }
    auto const utc = *local - std::chrono::minutes{date ? 0 : offsetMinutes};
    return time::Timestamp{time::DateTime{std::chrono::time_point_cast<std::chrono::milliseconds>(utc)}};
}

/// @brief The draft a date-time control writes for an instant (blank for none).
/// @param value         The instant.
/// @param date          Whether the field holds a date.
/// @param offsetMinutes The display zone.
/// @return `YYYY-MM-DDTHH:MM:SS` in the zone, or `YYYY-MM-DD`.
[[nodiscard]] inline std::string instantDraft(std::optional<time::Timestamp> const& value, bool date,
                                              int offsetMinutes) {
    if (!value || !value->hasValue()) {
        return {};
    }
    auto const instant =
        std::chrono::floor<std::chrono::seconds>((**value).value) + std::chrono::minutes{date ? 0 : offsetMinutes};
    std::string const text = formatWallClock(instant);
    return date ? text.substr(0, 10) : text;
}

/// @brief The view-tree field state of a field: read-only, required, and its error as the one message shown.
/// @param field The field.
/// @return The state, bound to the field.
[[nodiscard]] inline ui::FieldState fieldState(FieldState const* field) {
    return ui::FieldState{
        .readonly = [field] { return field->readonly().get(); },
        .required = [field] { return field->required().get(); },
        .errors =
            [field] {
                auto const& error = field->error().get();
                return error.has_value() ? std::vector<std::string>{error->message} : std::vector<std::string>{};
            },
        .stale = false,
    };
}

/// @brief The `Common` of a field's control: its test id (`field_<path>`) and accessible name (its label).
/// @param field The field.
/// @return The common part.
[[nodiscard]] inline ui::Common controlCommon(FieldState const* field) {
    ui::Common common{};
    common.testId = "field_" + field->path();
    common.a11y.name = field->label();
    return common;
}

[[nodiscard]] ui::Node fieldCell(FormSession& session, FieldState& state, ViewOptions const& options);

/// @brief A Select over the field's options (an Enum's, or a Choice's as last fetched), keyed by each option's
///        JSON literal, so an id past 2^53 is never a number on the way.
/// @param field The field.
/// @param style Dropdown or radio.
/// @return The control.
[[nodiscard]] inline ui::Node optionSelect(FieldState* field, ui::SelectStyle style) {
    return ui::select(ui::Select{
        .options =
            [field] {
                std::vector<ui::SelectOption> options;
                for (ChoiceOption const& option : field->options()) {
                    options.push_back(ui::SelectOption{.key = ui::Key{option.valueJson}, .label = option.label});
                }
                return options;
            },
        .selected = [field] -> std::optional<ui::Key> {
            std::string_view const selected = trim(field->text().get());
            return selected.empty() ? std::nullopt : std::optional<ui::Key>{ui::Key{std::string{selected}}};
        },
        .onSelect =
            [field](ui::Key const& key) {
                if (auto const* const value = std::get_if<std::string>(&key)) {
                    field->text().set(*value);
                }
            },
        .onOpen = [field] { field->retryOptions(); },
        .style = style,
        .field = fieldState(field),
        .common = controlCommon(field),
    });
}

/// @brief The built-in control of one field (the table in `docs/spec/forms/engine.md`, "Rendering").
/// @param session The session.
/// @param state   The field.
/// @param options The view options (members and rows draw their own cells with them).
/// @return The control.
// One case per kind; an Object or an ObjectArray draws its members' cells.
// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
[[nodiscard]] inline ui::Node fieldControl(FormSession& session, FieldState& state, ViewOptions const& options) {
    FieldState* const field = &state;
    FieldSpec const& spec = state.spec();
    auto const textInput = [field](ui::TextInputMode mode) {
        return ui::textInput(ui::TextInput{
            .value = [field] { return field->text().get(); },
            .onChange = [field](std::string text) { field->text().set(std::move(text)); },
            .onSubmit = {},
            .onCommit = {},
            .placeholder = field->placeholder(),
            .mode = mode,
            .field = fieldState(field),
            .common = controlCommon(field),
        });
    };
    auto const unitSuffix = [&](ui::Node input) {
        if (spec.unit.empty()) {
            return input;
        }
        return ui::row(ui::Row{
            .children = {std::move(input), ui::text(ui::Text{.text = spec.unit, .role = ui::TextRole::Muted})},
            .gap = 1,
        });
    };
    auto const dateInput = [&](bool date) {
        int const offset = date ? 0 : session.options().displayOffsetMinutes;
        return ui::dateTimeInput(ui::DateTimeInput{
            .value = [field, date, offset] { return draftInstant(trim(field->text().get()), date, offset); },
            .onChange =
                [field, date, offset](std::optional<time::Timestamp> const& value) {
                    field->text().set(instantDraft(value, date, offset));
                },
            .mode = date ? ui::DateMode::Date : ui::DateMode::DateTime,
            .offsetMinutes = offset,
            .field = fieldState(field),
            .common = controlCommon(field),
        });
    };
    if (spec.kind == FieldKind::Multiline) {
        return textInput(ui::TextInputMode::Multiline);
    }
    if (spec.kind == FieldKind::Integer || spec.kind == FieldKind::Number || spec.kind == FieldKind::Decimal) {
        return unitSuffix(textInput(ui::TextInputMode::SingleLine));
    }
    if (spec.kind == FieldKind::Quantity) {
        if (spec.units.size() <= 1) {
            return unitSuffix(textInput(ui::TextInputMode::SingleLine));
        }
        std::vector<ui::SelectOption> units;
        units.reserve(spec.units.size());
        for (std::size_t i = 0; i < spec.units.size(); ++i) {
            units.push_back(
                ui::SelectOption{.key = ui::Key{static_cast<std::int64_t>(i)}, .label = spec.units.at(i).display});
        }
        ui::Common unitCommon{};
        unitCommon.testId = "unit_" + field->path();
        unitCommon.a11y.name = field->label();
        return ui::row(ui::Row{
            .children = {textInput(ui::TextInputMode::SingleLine),
                         ui::select(ui::Select{
                             .options = std::move(units),
                             .selected = [field] -> std::optional<ui::Key> {
                                 return ui::Key{static_cast<std::int64_t>(field->unit().get())};
                             },
                             .onSelect =
                                 [field](ui::Key const& key) {
                                     if (auto const* const index = std::get_if<std::int64_t>(&key);
                                         index != nullptr && *index >= 0) {
                                         field->switchUnit(static_cast<std::size_t>(*index));
                                     }
                                 },
                             .style = ui::SelectStyle::Dropdown,
                             .field = {},
                             .common = std::move(unitCommon),
                         })},
            .gap = 1,
        });
    }
    if (spec.kind == FieldKind::Boolean) {
        return ui::checkbox(ui::Checkbox{
            .label = field->label(),
            .checked = [field] { return trim(field->text().get()) == "true"; },
            .onToggle = [field](bool checked) { field->text().set(checked ? "true" : "false"); },
            .field = fieldState(field),
            .common = controlCommon(field),
        });
    }
    if (spec.kind == FieldKind::Enum) {
        return optionSelect(field, spec.widget == "radio" ? ui::SelectStyle::Radio : ui::SelectStyle::Dropdown);
    }
    if (spec.kind == FieldKind::Choice) {
        return ui::row(ui::Row{
            .children =
                {optionSelect(field, spec.widget == "radio" ? ui::SelectStyle::Radio : ui::SelectStyle::Dropdown),
                 ui::busy(ui::Busy{.active = [field] { return field->optionsPending(); }, .label = std::string{}})},
            .gap = 1,
        });
    }
    if (spec.kind == FieldKind::DateTime || spec.kind == FieldKind::EpochDateTime) {
        return dateInput(false);
    }
    if (spec.kind == FieldKind::Date || spec.kind == FieldKind::EpochDate) {
        return dateInput(true);
    }
    if (spec.kind == FieldKind::Slider) {
        return ui::slider(ui::Slider{
            .value =
                [field] {
                    std::string_view const text = trim(field->text().get());
                    std::int64_t value = field->spec().sliderMin;
                    std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
                    return value;
                },
            .minimum = spec.sliderMin,
            .maximum = spec.sliderMax,
            .step = spec.sliderStep,
            .onChange = [field](std::int64_t value) { field->text().set(std::to_string(value)); },
            .field = fieldState(field),
            .common = controlCommon(field),
        });
    }
    if ((spec.kind == FieldKind::Object || spec.kind == FieldKind::ObjectArray) && spec.truncated) {
        return ui::text(ui::Text{.text = "nested too deeply to edit here", .role = ui::TextRole::Muted});
    }
    if (spec.kind == FieldKind::Object) {
        std::vector<ui::Node> cells;
        for (auto const& member : state.members()) {
            cells.push_back(fieldCell(session, *member, options));
        }
        ui::Panel panel{
            .title = state.label(), .padding = 1, .child = ui::column(ui::Column{.children = std::move(cells)})};
        panel.common.testId = "field_" + state.path();
        return ui::panel(std::move(panel));
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
                    .padding = 1,
                    .child = ui::column(ui::Column{.children = std::move(cells)}),
                });
            });
        ui::Button add{.label = options->addLabel, .onClick = [field] { field->addRow(); }};
        add.common.testId = "add_" + field->path();
        ui::Column column{.children = {std::move(rows), ui::button(std::move(add))}};
        column.common.testId = "field_" + state.path();
        return ui::column(std::move(column));
    }
    return textInput(ui::TextInputMode::SingleLine);
}

/// @brief One field's cell: the label (with ` *` while required), the control (an override's, else the built-in)
///        and the help text; shown while the field is visible.
/// @param session The session.
/// @param state   The field.
/// @param options The view options.
/// @return The cell.
// NOLINTNEXTLINE(misc-no-recursion): nested controls draw cells.
inline ui::Node fieldCell(FormSession& session, FieldState& state, ViewOptions const& options) {
    FieldState const* const field = &state;
    ui::Node control;
    if (Overrides::Render const* const render = options->overrides.resolve(state.spec())) {
        control = (*render)(FieldView{session, state, options});
    } else {
        control = fieldControl(session, state, options);
    }
    std::vector<ui::Node> children;
    if (state.spec().kind != FieldKind::Boolean && state.spec().kind != FieldKind::Object) {
        ui::Text label{.text = [field] { return field->label() + (field->required().get() ? " *" : ""); }};
        label.common.testId = "label_" + state.path();
        children.push_back(ui::text(std::move(label)));
    }
    children.push_back(std::move(control));
    if (!state.help().empty()) {
        children.push_back(ui::text(ui::Text{.text = state.help(), .role = ui::TextRole::Muted}));
    }
    ui::Column cell{.children = std::move(children)};
    cell.common.visible = [field] { return field->visible().get(); };
    cell.common.testId = "cell_" + state.path();
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
    return ui::grid(ui::Grid{.columns = width, .cells = std::move(cells), .gap = 1});
}

}  // namespace detail::engine

inline ui::Node FieldView::defaultControl() const {
    return detail::engine::fieldControl(*_session, *_state, _options);
}

/// @brief Renders @p session: its fields laid out by group (sections as panels, consecutive tab groups as one tab
///        bar, accordions as collapsible panels, the implicit group as a grid), the status line, the Submit button in
///        explicit mode, and the last reply or error.
///
/// The nodes bind to the session and its fields by pointer: mount the result after the session and destroy the
/// mount before it.
/// @param session The session.
/// @param options Overrides, grid columns, the chrome's labels.
/// @return The view.
[[nodiscard]] inline ui::Node formView(FormSession& session, FormViewOptions options = {}) {
    using detail::engine::groupGrid;
    auto const shared = std::make_shared<FormViewOptions const>(std::move(options));
    int const columns = std::max(1, shared->gridColumns);
    int const flatColumns = shared->flatGridColumns.value_or(columns == 2 ? 1 : columns);
    FormSession* const form = &session;
    std::vector<ui::Node> children;
    if (shared->showTitle) {
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
                    ui::Tab{.label = session.groupTitle(i),
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
            children.push_back(ui::panel(ui::Panel{.title = session.groupTitle(i),
                                                   .padding = 1,
                                                   .child = groupGrid(session, group, columns, shared),
                                                   .collapsible = true,
                                                   .collapsed = [collapsed] { return collapsed->get(); },
                                                   .onToggle = [collapsed](bool value) { collapsed->set(value); }}));
            ++i;
        } else {
            children.push_back(ui::panel(ui::Panel{
                .title = session.groupTitle(i), .padding = 1, .child = groupGrid(session, group, columns, shared)}));
            ++i;
        }
    }
    if (shared->showStatus) {
        ui::Text status{.text = [form] { return detail::engine::statusText(*form); }, .role = ui::TextRole::Muted};
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
    result.common.visible = [form] { return form->lastError() != nullptr || form->lastReply().has_value(); };
    result.common.testId = "form_result";
    children.push_back(ui::text(std::move(result)));
    return ui::column(ui::Column{.children = std::move(children), .gap = 1});
}

}  // namespace morph::forms
