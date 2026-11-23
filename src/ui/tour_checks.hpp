// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace morph::ui::detail {

/// What the tour checks read of one registered screen.
struct TourScreen {
    /// Its id.
    std::string id;
    /// Its document's JSON text; empty for an entry without one: a host screen, or one the application has not built.
    std::string text;
    /// Whether a signed-in navigator lists it: it is not public.
    bool listed = true;
};

/// What one registered screen offers a step's target: whether it has a document, whether a signed-in navigator lists
/// it, and the test ids its document carries (its literal `testId`s, its components' fallbacks' included, and
/// `action_<id>` for each head action).
struct TourTarget {
    /// Whether a signed-in navigator lists it.
    bool listed = true;
    /// Whether it has a document; a host screen has none.
    bool hasDocument = false;
    /// The test ids its document carries.
    std::set<std::string, std::less<>> ids;
};

/// The registered screens as the checks read them, by screen id.
using TourTargets = std::map<std::string, TourTarget, std::less<>>;

/// Reads every screen's targets once, for any number of `checkTour` calls.
/// @param screens The registered screens.
/// @return Their targets, by id.
[[nodiscard]] TourTargets indexTourScreens(std::span<TourScreen const> screens);

/// Checks what one tour names (its screen, each step's screen, `advanceOn` and target) against the registered screens:
/// the one check the bundle's build and a runtime registration (`TourRegistry`) share.
/// @param tour    The tour, as the loader read it.
/// @param parts   The shell's declared parts, which produce the accepted shell ids.
/// @param screens The registered screens' targets.
/// @return The refusal, naming the tour and the step, or nothing.
[[nodiscard]] std::optional<Refusal> checkTour(TourDecl const& tour, std::optional<ShellParts> const& parts,
                                               TourTargets const& screens);

/// Checks every `tour.start` in a document whose `args` names a tour as a literal.
/// @param document The document's id, for the refusal.
/// @param text     Its JSON text.
/// @param declared The tours that may be named.
/// @return The refusal of the first literal id not in @p declared, or nothing.
[[nodiscard]] std::optional<Refusal> checkTourStarts(std::string const& document, std::string_view text,
                                                     std::set<std::string, std::less<>> const& declared);

/// Checks what the app document's tours name against the registered screens, and every literal `tour.start` against
/// the tours (docs/spec/ui/tours.md, "Checks").
/// @param app     The app document, loaded; null when the bundle has none, and then it declares no tour.
/// @param appText The app document's JSON text; empty with none.
/// @param screens The registered screens.
/// @return The first refusal, naming the tour and the step or the document and the command, or nothing.
[[nodiscard]] std::optional<Refusal> checkTours(Document const* app, std::string_view appText,
                                                std::span<TourScreen const> screens);

/// The screens of a served bundle as the checks read them: each screen's document text from the bundle's files.
/// @param bundle The bundle.
/// @return Its screens, in catalog order.
[[nodiscard]] std::vector<TourScreen> tourScreensOf(ServedBundle const& bundle);

/// The app document's text in a served bundle.
/// @param bundle The bundle.
/// @return It; empty when the bundle has none.
[[nodiscard]] std::string appTextOf(ServedBundle const& bundle);

}  // namespace morph::ui::detail
