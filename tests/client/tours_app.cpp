// SPDX-License-Identifier: Apache-2.0

#include "tours_app.hpp"

#include <expected>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/tour_registry.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tours {
namespace {

constexpr std::string_view kHome = R"({"vocab":"ui/1","id":"tours.home","title":"Home",
  "state":{"missing":{"init":"nope"}},
  "actions":[{"id":"refresh","label":"Refresh","onClick":[]}],
  "view":{"kind":"column","children":[
    {"kind":"text","testId":"home_text","text":"Home"},
    {"kind":"button","testId":"take_tour","label":"Take the tour",
     "onClick":[{"host":"tour.start","args":{"obj":{"id":"getting-started"}}}]},
    {"kind":"button","testId":"bad_tour","label":"Bad",
     "onClick":[{"host":"tour.start","args":{"obj":{"id":{"ref":"missing"}}}}]},
    {"kind":"text","testId":"can_tour","text":{"fn":"format","args":["{0}",{"can":"host:tour.start"}]}}]}})";

/// A screen that signs the person out as it opens, as a session-expired screen does.
constexpr std::string_view kExpired = R"({"vocab":"ui/1","id":"tours.expired","title":"Expired",
  "onMount":[{"signOut":true}],
  "view":{"kind":"text","testId":"expired_text","text":"Expired"}})";

/// The home screen of the variants that declare fewer tours: the registry refuses a `tour.start` of a tour the app
/// document does not declare.
constexpr std::string_view kPlainHome = R"({"vocab":"ui/1","id":"tours.home","title":"Home",
  "actions":[{"id":"refresh","label":"Refresh","onClick":[]}],
  "view":{"kind":"column","children":[{"kind":"text","testId":"home_text","text":"Home"}]}})";

constexpr std::string_view kSamples = R"({"vocab":"ui/1","id":"tours.samples","title":"Samples",
  "view":{"kind":"column","children":[
    {"kind":"text","testId":"samples_table","text":"rows"},
    {"kind":"text","testId":"samples_filter","text":"filter"}]}})";

constexpr std::string_view kTours = R"([
  {"id":"getting-started","title":"Getting started","summary":"The shell",
   "steps":[{"target":"shell_tours","title":"Tours"},
            {"target":"nav_tours.samples","advanceOn":{"screen":"tours.samples"},"title":"Open Samples"},
            {"target":"samples_table","title":"The table"}]},
  {"id":"register","screen":"tours.samples","title":"Register a sample",
   "steps":[{"target":"samples_table","title":"Here"}]},
  {"id":"find","screen":"tours.samples","title":"Find a sample",
   "steps":[{"target":"samples_filter","title":"Filter"}]},
  {"id":"locked","title":"Locked away",
   "steps":[{"screen":"tours.closed","target":"shell_tours","title":"Closed"}]},
  {"id":"expires","title":"Expires",
   "steps":[{"target":"shell_tours","title":"First"},
            {"screen":"tours.expired","target":"shell_tours","title":"Second"}]},
  {"id":"waits","title":"Waits first",
   "steps":[{"target":"nav_tours.samples","advanceOn":{"screen":"tours.samples"},"title":"Open Samples"},
            {"target":"samples_table","title":"The table"}]}])";

constexpr std::string_view kScreenTours = R"([
  {"id":"register","screen":"tours.samples","title":"Register a sample",
   "steps":[{"target":"samples_table","title":"Here"}]}])";

/// The application with @p home as its first screen and @p appTours as the app document's tours; empty serves no app
/// document.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> build(std::string_view home,
                                                                                        std::string_view appTours) {
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "tours", .version = "1"}};
    auto const add = [&served](std::string id, std::string const& title, std::string_view text) {
        morph::ui::CatalogEntry entry;
        entry.id = std::move(id);
        entry.title = title;
        return served.addScreen(std::move(entry), std::string{text});
    };
    if (auto added = add("tours.home", "Home", home); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = add("tours.samples", "Samples", kSamples); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto added = add("tours.expired", "Expired", kExpired); !added) {
        return std::unexpected(std::move(added.error()));
    }
    morph::ui::CatalogEntry closed;
    closed.id = "tours.closed";
    closed.title = "Closed";
    closed.available = false;
    if (auto added = served.addEntry(std::move(closed)); !added) {
        return std::unexpected(std::move(added.error()));
    }
    if (!appTours.empty()) {
        if (auto added = served.addFile(morph::ui::BundleFile{
                .kind = "app",
                .id = "app",
                .bytes = R"({"vocab":"ui/1","id":"app","tours":)" + std::string{appTours} + "}"});
            !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> toursBundle() {
    return build(kHome, kTours);
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> screenToursBundle() {
    return build(kPlainHome, kScreenTours);
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> noToursBundle() {
    return build(kPlainHome, {});
}

std::shared_ptr<morph::ui::TourRegistry> toursRegistry() {
    auto built = toursBundle();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    auto registry = std::make_shared<morph::ui::TourRegistry>([held = *built] { return held; });
    morph::ui::registerTours(registry, morph::session::allowAllAuthorizer(), morph::model::detail::defaultDispatcher(),
                             morph::model::detail::defaultRegistry());
    return registry;
}

std::string runtimeTour(std::string const& id, std::string const& stepTitle) {
    return R"({"id":")" + id +
           R"(","title":"What's new","summary":"Added while the server runs","steps":[{"title":")" + stepTitle +
           R"("},{"title":"And more"}]})";
}

}  // namespace tours
