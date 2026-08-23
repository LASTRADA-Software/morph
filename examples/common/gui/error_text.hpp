// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>

#include <exception>

/// @file
/// `std::exception_ptr` -> displayable text, once.
///
/// Every ladder presenter and QML bridge that surfaces a `Completion` error to
/// QML needs the same three lines, and nothing about them is per-model: it is
/// an exception pointer in and a `QString` out, with the only variation being
/// which signal carries the result. `docs/analysis/qml-bridge-boilerplate.md`
/// measured the rest of the bridge duplication as O(QML surface) and largely
/// irreducible; this is one of the two pieces it identified as genuinely
/// model-independent and worth extracting (morph#168).

namespace morph::ladder::gui {

/// @brief Renders an exception pointer as text fit for display.
///
/// Handles the `catch (...)` arm that hand-written copies of this kept
/// omitting. That omission is not cosmetic: these handlers run as `Completion`
/// error callbacks, so an exception that is not a `std::exception` escaping one
/// takes the process down rather than reaching the user. Callers get a string
/// in every case and never have to reason about it.
///
/// @param err The exception pointer to render. A null pointer yields the
///        unknown-error text rather than being rethrown, since a caller that
///        reached an error callback has an error to show either way.
/// @return `what()` for a `std::exception`, decoded as UTF-8; otherwise
///         `"unknown error"`.
[[nodiscard]] QString errorText(const std::exception_ptr& err) noexcept;

}  // namespace morph::ladder::gui
