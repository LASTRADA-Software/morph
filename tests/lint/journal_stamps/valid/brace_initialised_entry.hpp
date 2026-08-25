// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_journal_stamps.sh: brace initialisation with a
// `.schema` designator is accepted, and a nested brace on the way does not
// end the initialiser as far as the checker is concerned.

#pragma once

namespace lint_fixture {

template <typename Action>
::morph::journal::LogEntry makeStampedEntry(const Action& action) {
    ::morph::journal::LogEntry entry{
        .modelType = "FixtureModel",
        .actionType = std::string{::morph::model::ActionTraits<Action>::typeId()},
        .payload = ::morph::model::ActionTraits<Action>::toJson(action),
        .schema = ::morph::model::detail::actionPayloadSchema<Action>(),
    };
    return entry;
}

}  // namespace lint_fixture
