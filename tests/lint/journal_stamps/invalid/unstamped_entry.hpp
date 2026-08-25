// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_journal_stamps.sh: the defect itself -- a
// hand-rolled entry that records everything except which payload shape wrote
// it, so `journal::replay()` can only replay it unverified. This is the exact
// shape four ladder models had (issue #244).

#pragma once

namespace lint_fixture {

template <typename Action, typename Result>
void logActionUnstamped(const Action& action, const Result& result) {
    ::morph::journal::LogEntry entry;
    entry.modelType = "FixtureModel";
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    append(std::move(entry));
}

}  // namespace lint_fixture
