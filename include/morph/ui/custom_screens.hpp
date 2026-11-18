// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <morph/export.hpp>
#include <optional>
#include <string_view>

#include "document.hpp"
#include "value.hpp"

/// @file
/// @brief A person's own screens: the store seam, the checks a custom document passes, and the server model that
///        serves them under the verified person.
///
/// Specified in `docs/spec/ui/custom_screens.md`.

namespace morph::ui {

/// @brief Checks that @p custom uses nothing its base screen does not: the base rule.
///
/// Compared: the actions it names, wherever declared and component fallbacks included, each on its model type and in
/// a position of the same kind (a query, a mutation, a model-backed table, a collection, a wizard, or a form by its
/// literal `submit`: none, `explicit` or `auto`); the commands run without a press (from a lifecycle list, a watch,
/// a component's `on` event, a form whose `submit` is not `explicit`, or the `onSuccess` and `onError` of a mutation
/// that a `run`, an `upload` or a `signIn` among them reaches, each name resolved in the scope that declares the
/// mutation), by trigger. A form with no `submit` keeps its action schema's mode, which this check cannot see and
/// which is automatic unless the schema says otherwise, so it counts as submitting itself.
///
/// Also compared: the client-side commands `upload`, `download`, `switchBackend`, `signIn`, `signOut` and
/// `resetPrefs`; every `host` name, whether a host command, a host value or a host component; each `host` command
/// as a whole, name and arguments, which must be identical to one of the base's; the delivered components, by name
/// and content hash; and the params, which must be the base's exactly. Every `upload` of the custom document must
/// send exactly `{"ref": "event"}` and sit directly in a `filePicker`'s `onPick` or a `dropZone`'s `onDrop`, so it
/// is always a file the person just chose. Text, layout, styles, states and expressions are not compared.
/// @param base       The base screen, loaded.
/// @param baseText   The base screen's JSON text.
/// @param custom     The custom document, loaded.
/// @param customText The text @p custom was loaded from, which the `host` names are read from.
/// @return The first use the base does not have, with its JSON Pointer, or nothing.
[[nodiscard]] MORPH_API std::optional<Refusal> checkBaseRule(Document const& base, std::string_view baseText,
                                                             Document const& custom, std::string_view customText);

}  // namespace morph::ui
