// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/forms/engine/field_control.hpp>.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <morph/forms/engine/detail/text.hpp>
#include <morph/forms/engine/field_control.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/control.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace morph::forms::detail::engine {

std::string resultText(FormSession const& session) {
    if (std::exception_ptr const error = session.lastError()) {
        return "error: " + reactive::errorMessage(error);
    }
    std::optional<std::string> const& reply = session.lastReply();
    return reply.has_value() ? "ok: " + *reply : std::string{};
}

std::optional<time::Timestamp> draftInstant(std::string_view text, bool date, int offsetMinutes) {
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

std::string instantDraft(std::optional<time::Timestamp> const& value, bool date, int offsetMinutes) {
    if (!value || !value->hasValue()) {
        return {};
    }
    auto const instant =
        std::chrono::floor<std::chrono::seconds>((**value).value) + std::chrono::minutes{date ? 0 : offsetMinutes};
    std::string const text = formatWallClock(instant);
    return date ? text.substr(0, 10) : text;
}

ui::FieldState fieldState(FieldState const* field) {
    return ui::FieldState{
        .readonly = [field] { return field->readonly().get(); },
        .required = [field] { return field->required().get(); },
        .errors =
            [field] {
                auto const& error = field->error().get();
                return error.has_value() ? std::vector<std::string>{error->message} : std::vector<std::string>{};
            },
        .stale = false,
        .description = [field] { return field->help(); },
    };
}

ui::Common controlCommon(FieldState const* field) {
    ui::Common common{};
    common.testId = "field_" + field->path();
    common.a11y.name = [field] { return field->label(); };
    return common;
}

ui::Node optionSelect(FieldState* field, ui::SelectStyle style) {
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
        // The cell draws the caption, so the select is told its label and not to show it.
        .label = [field] { return field->label(); },
        .showLabel = false,
        .field = fieldState(field),
        .common = controlCommon(field),
    });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ui::Node scalarControl(FormSession& session, FieldState& state) {
    FieldState* const field = &state;
    FieldSpec const& spec = state.spec();
    auto const textInput = [field](ui::TextInputMode mode, std::string unit = {}, ui::Prop<int> decimals = -1) {
        return ui::textInput(ui::TextInput{
            .value = [field] { return field->text().get(); },
            .onChange = [field](std::string text) { field->text().set(std::move(text)); },
            .onSubmit = {},
            .onCommit = {},
            .placeholder = [field] { return field->placeholder(); },
            .mode = mode,
            .unit = std::move(unit),
            .decimals = std::move(decimals),
            .label = [field] { return field->label(); },
            .showLabel = false,
            .field = fieldState(field),
            .common = controlCommon(field),
        });
    };
    // A number's input shows its unit and knows its entry places: an integer's 0, the chosen unit's, a plain
    // number's display places, a decimal's declared ones; -1 where none is declared. A record's places
    // (`FieldState::places`) replace the canonical unit's, or a plain number's, and no other unit's.
    auto const numberInput = [&] {
        if (spec.kind == FieldKind::Integer) {
            return textInput(ui::TextInputMode::SingleLine, spec.unit, 0);
        }
        if (!spec.units.empty()) {
            return textInput(ui::TextInputMode::SingleLine, spec.unit, [field] {
                auto const& units = field->spec().units;
                std::size_t const unit = std::min(field->unit().get(), units.size() - 1);
                std::optional<std::uint32_t> const places = field->places().get();
                return static_cast<int>(unit == 0 && places.has_value() ? *places : units.at(unit).decimals);
            });
        }
        int decimals = -1;
        if (spec.displayDecimals) {
            decimals = static_cast<int>(*spec.displayDecimals);
        } else if (spec.decimalPlaces) {
            decimals = static_cast<int>(*spec.decimalPlaces);
        }
        return textInput(ui::TextInputMode::SingleLine, spec.unit, [field, decimals] {
            std::optional<std::uint32_t> const places = field->places().get();
            return places.has_value() ? static_cast<int>(*places) : decimals;
        });
    };
    auto const dateInput = [&](bool date) {
        int const offset = date ? 0 : session.options().displayOffsetMinutes;
        // The input shows and reads the session's pattern, which it follows; the draft text below stays ISO.
        FormSession const* const owner = &session;
        return ui::dateTimeInput(ui::DateTimeInput{
            .value = [field, date, offset] { return draftInstant(trim(field->text().get()), date, offset); },
            .onChange =
                [field, date, offset](std::optional<time::Timestamp> const& value) {
                    field->text().set(instantDraft(value, date, offset));
                },
            .mode = date ? ui::DateMode::Date : ui::DateMode::DateTime,
            .offsetMinutes = offset,
            .label = [field] { return field->label(); },
            .showLabel = false,
            .field = fieldState(field),
            .common = controlCommon(field),
            .pattern = [owner, date] { return owner->pattern(date); },
        });
    };
    if (spec.kind == FieldKind::Multiline) {
        return textInput(ui::TextInputMode::Multiline);
    }
    if (spec.kind == FieldKind::Integer || spec.kind == FieldKind::Number || spec.kind == FieldKind::Decimal) {
        return numberInput();
    }
    if (spec.kind == FieldKind::Quantity) {
        if (spec.units.size() <= 1) {
            return numberInput();
        }
        std::vector<ui::SelectOption> units;
        units.reserve(spec.units.size());
        for (std::size_t i = 0; i < spec.units.size(); ++i) {
            units.push_back(
                ui::SelectOption{.key = ui::Key{static_cast<std::int64_t>(i)}, .label = spec.units.at(i).display});
        }
        ui::Common unitCommon{};
        unitCommon.testId = "unit_" + field->path();
        unitCommon.a11y.name = [field] { return field->label(); };
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
            .gap = 4,
        });
    }
    if (spec.kind == FieldKind::Boolean) {
        return ui::checkbox(ui::Checkbox{
            .label = [field] { return field->label(); },
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
            .gap = 4,
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
    return textInput(ui::TextInputMode::SingleLine);
}

}  // namespace morph::forms::detail::engine
