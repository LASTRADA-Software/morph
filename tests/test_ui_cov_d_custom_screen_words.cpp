// SPDX-License-Identifier: Apache-2.0

// What the chrome tells the person of each outcome, in the words it chooses for the event, the save and the detail.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/ui/custom_screen_chrome.hpp>
#include <string>
#include <string_view>

namespace {

using morph::ui::CustomScreenChrome;
using morph::ui::CustomScreenEvent;
using morph::ui::CustomScreenOutcome;
using morph::ui::CustomScreenSaveKind;

std::string described(CustomScreenEvent event, CustomScreenSaveKind save, std::string_view detail) {
    return CustomScreenChrome::describe(
        CustomScreenOutcome{.event = event, .save = save, .title = "Mine", .detail = std::string{detail}});
}

}  // namespace

TEST_CASE("covd custom screen words: every outcome is worded by its event, save and detail",
          "[ui][custom-screens][covd]") {
    struct Case {
        CustomScreenEvent event;
        CustomScreenSaveKind save;
        std::string_view detail;
        std::string_view words;
    };
    using E = CustomScreenEvent;
    using K = CustomScreenSaveKind;
    auto const [event, save, detail, words] = GENERATE(
        Case{E::Saved, K::Save, "", "Saved Mine"}, Case{E::Saved, K::SaveAs, "", "Saved as Mine"},
        Case{E::SavedTabClosed, K::Save, "", "Saved Mine, but its tab was closed first."},
        Case{E::SavedDraftReplaced, K::SaveAs, "",
             "Saved as Mine. The draft now open is a different one and is not saved."},
        Case{E::SavedDraftGone, K::Save, "", "Saved Mine, and the draft it was saved from is gone."},
        Case{E::SavedNotOpened, K::SaveAs, "no tab", "Saved as Mine, but it could not be opened: no tab"},
        Case{E::SavedNotOpened, K::SaveAs, "", "Saved as Mine, but it could not be opened"},
        Case{E::SavedNotReread, K::Save, "gone", "Saved Mine, but the stored screen could not be read again: gone"},
        Case{E::SavedNotReread, K::Save, "", "Saved Mine, but the stored screen could not be read again"},
        Case{E::SavedStaysDraft, K::SaveAs, "busy",
             "Saved as Mine, but it stays a draft here until this screen can close: busy"},
        Case{E::SavedStaysDraft, K::SaveAs, "",
             "Saved as Mine, but it stays a draft here until this screen can close"},
        Case{E::SaveRefused, K::None, "too long", "Not saved: too long"},
        Case{E::SaveRefused, K::None, "", "Not saved"}, Case{E::Removed, K::None, "", "Removed Mine"},
        Case{E::RemovedStaysOpen, K::None, "dirty",
             "Mine is removed, but stays open in its tab until it can close: dirty"},
        Case{E::RemovedStaysOpen, K::None, "", "Mine is removed, but stays open in its tab until it can close"},
        Case{E::OpenAsCancelled, K::None, "", "Open as cancelled: Mine stayed open"},
        Case{E::OpenFailed, K::None, "refused", "Cannot open Mine: refused"},
        Case{E::OpenFailed, K::None, "", "Cannot open Mine"},
        Case{E::SourceUnreadable, K::None, "bad", "The source of Mine could not be read: bad"},
        Case{E::SourceUnreadable, K::None, "", "The source of Mine could not be read"},
        Case{E::StoredSourceUnreadable, K::None, "bad", "Cannot open the source of Mine: bad"},
        Case{E::StoredSourceUnreadable, K::None, "", "Cannot open the source of Mine"},
        Case{E::ProposalNotShown, K::None, "bad", "The assistant's draft could not be shown: bad"},
        Case{E::ProposalNotShown, K::None, "", "The assistant's draft could not be shown"});
    CAPTURE(words);
    CHECK(described(event, save, detail) == words);
}

TEST_CASE("covd custom screen words: an event the chrome has no words for shows the title and its detail",
          "[ui][custom-screens][covd]") {
    auto const unknown = static_cast<CustomScreenEvent>(250);
    CHECK(described(unknown, CustomScreenSaveKind::None, "why") == "Mine: why");
    CHECK(described(unknown, CustomScreenSaveKind::None, "") == "Mine");
}
