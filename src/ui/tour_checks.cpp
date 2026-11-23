// SPDX-License-Identifier: Apache-2.0

#include "tour_checks.hpp"

#include <cstddef>
#include <map>
#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui::detail {
namespace {

using Names = std::set<std::string, std::less<>>;

// Every literal `testId` in a document's JSON, the components' fallbacks included.
// NOLINTNEXTLINE(misc-no-recursion): a document is a tree, walked down.
void collectTestIds(Json const& json, Names& out) {
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        if (auto const found = object->find("testId"); found != object->end()) {
            if (auto const* const id = std::get_if<std::string>(&found->second.data)) {
                out.insert(*id);
            }
        }
        for (auto const& [key, value] : *object) {
            static_cast<void>(key);
            collectTestIds(value, out);
        }
    } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
        for (Json const& element : *array) {
            collectTestIds(element, out);
        }
    }
}

// The ids the interpreter gives the buttons of a screen head's `actions`: `action_<id>`.
void collectHeadActions(Json const& json, Names& out) {
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    if (object == nullptr) {
        return;
    }
    auto const actions = object->find("actions");
    auto const* const list = actions == object->end() ? nullptr : std::get_if<Json::array_t>(&actions->second.data);
    if (list == nullptr) {
        return;
    }
    for (Json const& action : *list) {
        auto const* const fields = std::get_if<Json::object_t>(&action.data);
        if (fields == nullptr) {
            continue;
        }
        if (auto const id = fields->find("id"); id != fields->end()) {
            if (auto const* const text = std::get_if<std::string>(&id->second.data)) {
                out.insert("action_" + *text);
            }
        }
    }
}

// The tour a `tour.start` command's `args` names as a literal: `{"obj": {"id": "x"}}` or `{"lit": {"id": "x"}}`. A
// computed id is checked at run time instead.
std::optional<std::string> literalTourId(Json const& args) {
    auto const* const object = std::get_if<Json::object_t>(&args.data);
    if (object == nullptr || object->size() != 1) {
        return std::nullopt;
    }
    auto const& [form, record] = *object->begin();
    if (form != "obj" && form != "lit") {
        return std::nullopt;
    }
    auto const* const fields = std::get_if<Json::object_t>(&record.data);
    if (fields == nullptr) {
        return std::nullopt;
    }
    auto const id = fields->find("id");
    auto const* const text = id == fields->end() ? nullptr : std::get_if<std::string>(&id->second.data);
    return text == nullptr ? std::nullopt : std::optional<std::string>{*text};
}

struct TourStart {
    std::string path;
    std::string id;
};

// Every `tour.start` whose `args` names a tour as a literal, with its JSON Pointer.
// NOLINTNEXTLINE(misc-no-recursion): a document is a tree, walked down.
void collectTourStarts(Json const& json, std::string const& path, std::vector<TourStart>& out) {
    if (auto const* const object = std::get_if<Json::object_t>(&json.data)) {
        auto const host = object->find("host");
        auto const args = object->find("args");
        if (host != object->end() && args != object->end()) {
            auto const* const name = std::get_if<std::string>(&host->second.data);
            if (name != nullptr && *name == "tour.start") {
                if (std::optional<std::string> id = literalTourId(args->second)) {
                    out.push_back(TourStart{.path = path, .id = std::move(*id)});
                }
            }
        }
        for (auto const& [key, value] : *object) {
            collectTourStarts(value, pointerAppend(path, key), out);
        }
    } else if (auto const* const array = std::get_if<Json::array_t>(&json.data)) {
        for (std::size_t index = 0; index < array->size(); ++index) {
            collectTourStarts(array->at(index), pointerAppend(path, index), out);
        }
    }
}

// Whether the shell draws @p target for this bundle: the Tours button (a tour is declared), the chat's input, a
// navigator entry of a listed screen with a document, or a part the app document declares.
bool shellTarget(std::string_view target, std::optional<ShellParts> const& parts, TourTargets const& screens) {
    if (target == "shell_tours" || target == "chat_input") {
        return true;
    }
    if (target.starts_with("nav_")) {
        auto const found = screens.find(target.substr(4));
        return found != screens.end() && found->second.listed && found->second.hasDocument;
    }
    if (!parts) {
        return false;
    }
    if (target == "shell_search") {
        return !parts->search.empty();
    }
    if (target == "shell_inbox") {
        return parts->inbox.has_value();
    }
    if (target == "shell_help") {
        return parts->help.has_value();
    }
    if (target == "shell_principal_menu") {
        return parts->principal.has_value();
    }
    if (target == "shell_brand_title") {
        return parts->brand.has_value();
    }
    if (target == "shell_context") {
        return parts->context.has_value();
    }
    return false;
}

Refusal refusal(std::string path, std::string reason) {
    return Refusal{.document = std::string{kAppFile}, .path = std::move(path), .reason = std::move(reason)};
}

std::string stepName(TourDecl const& tour, std::size_t index) {
    return "the tour `" + tour.id + "`, step " + std::to_string(index + 1);
}

}  // namespace

// One tour. The effective screen of a step is the screen the active tab shows when the step shows: its own `screen`,
// else the latest of an earlier step's `screen` and the `advanceOn` of an earlier step, else the tour's `screen`.
std::optional<Refusal> checkTour(TourDecl const& tour, std::optional<ShellParts> const& parts,
                                 TourTargets const& screens) {
    if (!tour.screen.empty() && !screens.contains(tour.screen)) {
        return refusal(pointerAppend(tour.path, "screen"), "the tour `" + tour.id + "` belongs to `" + tour.screen +
                                                               "`, which is not a registered screen");
    }
    std::string current = tour.screen;
    for (std::size_t index = 0; index < tour.steps.size(); ++index) {
        TourStepDecl const& step = tour.steps.at(index);
        if (!step.screen.empty()) {
            if (!screens.contains(step.screen)) {
                return refusal(pointerAppend(step.path, "screen"), stepName(tour, index) + " opens `" + step.screen +
                                                                       "`, which is not a registered screen");
            }
            current = step.screen;
        }
        if (!step.advanceOn.empty() && !screens.contains(step.advanceOn)) {
            return refusal(
                pointerAppend(pointerAppend(step.path, "advanceOn"), "screen"),
                stepName(tour, index) + " waits for `" + step.advanceOn + "`, which is not a registered screen");
        }
        if (!step.target.empty() && !shellTarget(step.target, parts, screens)) {
            auto const found = current.empty() ? screens.end() : screens.find(current);
            bool const onScreen = found != screens.end() && found->second.ids.contains(step.target);
            if (!onScreen) {
                std::string where = "the shell";
                if (found != screens.end() && !found->second.hasDocument) {
                    where = "the shell (`" + current + "` is a host screen, whose parts the server cannot see)";
                } else if (found != screens.end()) {
                    where = "`" + current + "` or the shell";
                }
                return refusal(pointerAppend(step.path, "target"), stepName(tour, index) + " rings `" + step.target +
                                                                       "`, which nothing on " + where + " carries");
            }
        }
        if (!step.advanceOn.empty()) {
            current = step.advanceOn;
        }
    }
    return std::nullopt;
}

std::optional<Refusal> checkTourStarts(std::string const& document, std::string_view text, Names const& declared) {
    auto const json = parseJson(text);
    if (!json) {
        return std::nullopt;
    }
    std::vector<TourStart> starts;
    collectTourStarts(*json, "", starts);
    for (TourStart const& start : starts) {
        if (!declared.contains(start.id)) {
            return Refusal{
                .document = document,
                .path = start.path,
                .reason = "`tour.start` names the tour `" + start.id + "`, which the app document does not declare"};
        }
    }
    return std::nullopt;
}

TourTargets indexTourScreens(std::span<TourScreen const> screens) {
    TourTargets known;
    for (TourScreen const& screen : screens) {
        TourTarget info{.listed = screen.listed, .hasDocument = !screen.text.empty(), .ids = {}};
        if (info.hasDocument) {
            if (auto const json = parseJson(screen.text)) {
                collectTestIds(*json, info.ids);
                collectHeadActions(*json, info.ids);
            }
        }
        known.insert_or_assign(screen.id, std::move(info));
    }
    return known;
}

std::optional<Refusal> checkTours(Document const* app, std::string_view appText, std::span<TourScreen const> screens) {
    TourTargets const known = indexTourScreens(screens);
    Names declared;
    if (app != nullptr) {
        for (TourDecl const& tour : app->tours) {
            declared.insert(tour.id);
            if (auto refused = checkTour(tour, app->shell, known)) {
                return refused;
            }
        }
    }
    for (TourScreen const& screen : screens) {
        if (auto refused = checkTourStarts(screen.id, screen.text, declared)) {
            return refused;
        }
    }
    if (!appText.empty()) {
        if (auto refused = checkTourStarts(std::string{kAppFile}, appText, declared)) {
            return refused;
        }
    }
    return std::nullopt;
}

}  // namespace morph::ui::detail
