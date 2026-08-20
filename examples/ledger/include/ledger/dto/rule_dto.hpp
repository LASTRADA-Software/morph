// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ledger/core/types.hpp"
#include <morph/forms/forms.hpp>
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
