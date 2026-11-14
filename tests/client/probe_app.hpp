// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// A two-screen application the foundation's tests open: `probe.home` shows the answer of a query and navigates to
/// `probe.other`.

namespace probe {

/// @brief The catalog facts of the search and count actions: what a registry validates a shell declaration against.
/// @return The facts.
[[nodiscard]] morph::ui::Catalog searchCatalog();

/// @brief The query's action.
struct Answer {};

/// @brief A sign-in that answers in a shape of its own, `{user, jwt}`, as an application's own server may.
struct Login {};

/// @brief What `Login` answers.
struct LoginReply {
    /// @brief The principal.
    std::string user;
    /// @brief The session token.
    std::string jwt;
};

/// @brief An attachment moved inline: the file's bytes in base64.
struct Attach {
    /// @brief The bytes, in base64.
    std::string content;
};

/// @brief The scope switcher's read.
struct Scopes {};

/// @brief One scope the person may choose.
struct ScopeOption {
    /// @brief The caption.
    std::string label;
    /// @brief The value screens read as the context.
    std::string value;
};

/// @brief What `Scopes` answers.
struct ScopeList {
    /// @brief The options.
    std::vector<ScopeOption> options;
};

/// @brief A search.
struct Search {
    /// @brief The typed text.
    std::string q;
};

/// @brief One hit of `Search`.
struct SearchHit {
    /// @brief The screen it opens.
    std::string screen;
    /// @brief Its caption.
    std::string title;
};

/// @brief What `Search` answers.
struct SearchHits {
    /// @brief The hits.
    std::vector<SearchHit> hits;
};

/// @brief A module's count.
struct Count {};

/// @brief What `Count` answers: a plain reply, not a registered record.
struct CountReply {
    /// @brief The count.
    std::int64_t n = 0;
};

/// @brief The model: one answer, a sign-in, an attachment and the scopes.
struct ProbeModel {
    /// @brief The scopes.
    /// @param action The request.
    /// @return Two options.
    [[nodiscard]] ScopeList execute(Scopes const& action) const;
    /// @brief The search.
    /// @param action The request.
    /// @return A hit on each of `probe.width` and `probe.second`.
    [[nodiscard]] SearchHits execute(Search const& action) const;
    /// @brief The count.
    /// @param action The request.
    /// @return 3.
    [[nodiscard]] CountReply execute(Count const& action) const;
    /// @brief The answer.
    /// @param action The request.
    /// @return 42.
    [[nodiscard]] std::int64_t execute(Answer const& action) const;
    /// @brief Signs in as `ada`.
    /// @param action The request.
    /// @return `{user: "ada", jwt: "t-1"}`.
    [[nodiscard]] LoginReply execute(Login const& action) const;
    /// @brief Takes an attachment.
    /// @param action The attachment.
    /// @return How many base64 characters it carried.
    [[nodiscard]] std::int64_t execute(Attach const& action) const;
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
[[nodiscard]] morph::client::LocalApp app();

}  // namespace probe

/// @brief The probe model keeps no state, so the shell's parts and a screen may share one instance of it.
template <>
struct morph::ui::author::ModelFacts<probe::ProbeModel> {
    /// @brief Equivalent to a private instance.
    static constexpr bool stateless = true;
};
