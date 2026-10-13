// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_source.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>

/// @file
/// A two-screen application the foundation's tests open: `probe.home` shows the answer of a query and navigates to
/// `probe.other`.

namespace probe {

/// @brief The query's action.
struct Answer {};

/// @brief The model: one answer.
struct ProbeModel {
    /// @brief The answer.
    /// @param action The request.
    /// @return 42.
    [[nodiscard]] std::int64_t execute(Answer const& action) const;
};

/// @brief The start screen.
inline constexpr std::string_view kHome =
    R"({"vocab":"ui/1","id":"probe.home","models":{"probe":{"type":"Probe_Model"}},)"
    R"("queries":{"answer":{"model":"probe","action":"Probe_Answer"}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"text","testId":"answer","text":{"fn":"format","args":["answer {0}",{"ref":"answer"}]}},)"
    R"({"kind":"button","testId":"next","label":"Next","onClick":{"navigate":"probe.other"}}]}})";

/// @brief The screen `probe.home` navigates to.
inline constexpr std::string_view kOther =
    R"({"vocab":"ui/1","id":"probe.other","view":{"kind":"text","testId":"other","text":"The other screen"}})";

/// @brief The bundle: both screens.
/// @return It, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

/// @brief The application, linked.
/// @return It.
[[nodiscard]] morph::examples::LocalApp app();

}  // namespace probe
