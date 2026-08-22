// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/core/types.hpp"
#include <morph/forms/forms.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace ledger {

struct CreateRule {
    LedgerId ledgerId;
    RuleTrigger trigger;
    std::string matchText;
    RuleAction action;
    std::string actionValue;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !matchText.empty(); }
};

struct UpdateRule {
    RuleId ruleId;
    std::string matchText;
    std::string actionValue;

    /// @brief The `version` the client read before composing this edit.
    ///
    ///        Optional, and disengaged means "apply unconditionally" -- the
    ///        behaviour every existing caller already relies on. Engaged, it
    ///        turns this into an optimistic-concurrency update: if the stored
    ///        row has moved on, the edit is refused with `VersionConflict`
    ///        rather than overwriting whatever landed in between.
    ///
    ///        Optional rather than required because a rule edit made from a
    ///        form the user just loaded is the common case, and forcing every
    ///        caller to thread a version through would be a larger change
    ///        than the conflict it prevents. A client that cares -- an offline
    ///        queue replaying an edit composed minutes ago, say -- opts in.
    std::optional<std::int32_t> expectedVersion;

    [[nodiscard]] bool validate() const noexcept { return ruleId.hasValue() && !matchText.empty(); }
};

struct RuleInfo {
    RuleId id;
    RuleTrigger trigger;
    std::string matchText;
    RuleAction action;
    std::string actionValue;
    std::int32_t version;
};

}  // namespace ledger
