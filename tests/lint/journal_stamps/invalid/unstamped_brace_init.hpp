// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_journal_stamps.sh: brace initialisation with no
// `.schema` designator is the same defect written a different way, and must be
// rejected the same way.

#pragma once

namespace lint_fixture {

template <typename Action>
::morph::journal::LogEntry makeUnstampedEntry(const Action& action) {
    ::morph::journal::LogEntry entry{
        .modelType = "FixtureModel",
        .actionType = std::string{::morph::model::ActionTraits<Action>::typeId()},
        .payload = ::morph::model::ActionTraits<Action>::toJson(action),
    };
    return entry;
}

}  // namespace lint_fixture
