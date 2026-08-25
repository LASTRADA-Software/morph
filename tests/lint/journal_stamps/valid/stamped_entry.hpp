// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_journal_stamps.sh: the shape a hand-rolled
// journaling model is required to have. Not compiled -- the checker is a
// textual gate, and this file exists to prove it accepts what it should.

#pragma once

namespace lint_fixture {

template <typename Action, typename Result>
void logActionStamped(const Action& action, const Result& result) {
    ::morph::journal::LogEntry entry;
    entry.modelType = "FixtureModel";
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.schema = ::morph::model::detail::actionPayloadSchema<Action>();
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    append(std::move(entry));
}

}  // namespace lint_fixture
