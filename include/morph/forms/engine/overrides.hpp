// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/overrides.hpp
/// @brief `Overrides`: an application's replacements for the control `formView` draws for a field, chosen by
///        field path, widget hint, unit or kind; `FieldView`, what a replacement is handed.
///
/// Specified in `docs/spec/forms/engine.md`, "Rendering".

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../../ui/view.hpp"
#include "field_model.hpp"
#include "form_session.hpp"

namespace morph::forms {

struct FormViewOptions;

/// @brief A field's spec and state, handed to an override: what it shows (the value, the draft, the errors, the
///        options) and what it writes (the draft). Copyable; valid while the session lives.
class FieldView {
public:
    /// @param session The session. Borrowed.
    /// @param state   The field. Borrowed.
    /// @param options The view options the form is drawn with.
    FieldView(FormSession& session, FieldState& state, std::shared_ptr<FormViewOptions const> options) noexcept
        : _session{&session}, _state{&state}, _options{std::move(options)} {}

    /// @brief The field's spec.
    /// @return The spec.
    [[nodiscard]] FieldSpec const& spec() const noexcept { return _state->spec(); }

    /// @brief The field's state: draft, encoding, error, visibility, options. Writing its `text()` is a user edit.
    /// @return The state.
    [[nodiscard]] FieldState& state() const noexcept { return *_state; }

    /// @brief The session the field belongs to.
    /// @return The session.
    [[nodiscard]] FormSession& session() const noexcept { return *_session; }

    /// @brief The control `formView` draws for this field without overrides, for an override to wrap.
    /// @return The control.
    [[nodiscard]] ui::Node defaultControl() const;

private:
    FormSession* _session;
    FieldState* _state;
    std::shared_ptr<FormViewOptions const> _options;
};

/// @brief Replacement controls, resolved per field in the order field path, widget hint, unit, kind, else the
///        built-in control.
///
/// An override replaces the control only: the cell around it (label, help, visibility) is still drawn.
class Overrides {
public:
    /// @brief Draws a field's control.
    using Render = std::function<ui::Node(FieldView const&)>;

    /// @brief Overrides one field.
    /// @param path   The field's spec path (`FieldSpec::path`: `name`, `parent.member`, `rows[].member`).
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byField(std::string path, Render render) {
        _byField.insert_or_assign(std::move(path), std::move(render));
        return *this;
    }

    /// @brief Overrides every field with an `x-widget` hint.
    /// @param hint   The hint, e.g. `"slider"`.
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byWidget(std::string hint, Render render) {
        _byWidget.insert_or_assign(std::move(hint), std::move(render));
        return *this;
    }

    /// @brief Overrides every field in a unit.
    /// @param unit   The unit's `ExtUnits.unitAscii`.
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byUnit(std::string unit, Render render) {
        _byUnit.insert_or_assign(std::move(unit), std::move(render));
        return *this;
    }

    /// @brief Overrides every field of a kind.
    /// @param kind   The kind.
    /// @param render The control.
    /// @return `*this`, for chaining.
    Overrides& byKind(FieldKind kind, Render render) {
        _byKind.insert_or_assign(kind, std::move(render));
        return *this;
    }

    /// @brief The override for @p spec, if any.
    /// @param spec The field.
    /// @return The first match in field, widget, unit, kind order; null when none matches.
    [[nodiscard]] Render const* resolve(FieldSpec const& spec) const {
        if (auto const found = _byField.find(spec.path); found != _byField.end()) {
            return &found->second;
        }
        if (!spec.widget.empty()) {
            if (auto const found = _byWidget.find(spec.widget); found != _byWidget.end()) {
                return &found->second;
            }
        }
        if (!spec.unitAscii.empty()) {
            if (auto const found = _byUnit.find(spec.unitAscii); found != _byUnit.end()) {
                return &found->second;
            }
        }
        if (auto const found = _byKind.find(spec.kind); found != _byKind.end()) {
            return &found->second;
        }
        return nullptr;
    }

private:
    std::map<std::string, Render, std::less<>> _byField;
    std::map<std::string, Render, std::less<>> _byWidget;
    std::map<std::string, Render, std::less<>> _byUnit;
    std::map<FieldKind, Render> _byKind;
};

}  // namespace morph::forms
