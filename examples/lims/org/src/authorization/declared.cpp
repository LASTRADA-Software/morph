// SPDX-License-Identifier: Apache-2.0
#include "lims/org/authorization/declared.hpp"

#include <algorithm>

namespace lims {

Verdict authorizationVerdict(AuthorizationFacts const& facts) {
    if (std::ranges::find(facts.held, facts.required) == facts.held.end()) {
        return Verdict::MissingRole;
    }
    if (facts.firstPerformer.has_value() && *facts.firstPerformer == facts.principal) {
        return Verdict::SameActor;
    }
    if (facts.amount.has_value()) {
        if (!facts.limit.has_value()) {
            return Verdict::NoLimit;
        }
        if (*facts.amount > *facts.limit) {
            return Verdict::OverLimit;
        }
    }
    return Verdict::Allowed;
}

}  // namespace lims
