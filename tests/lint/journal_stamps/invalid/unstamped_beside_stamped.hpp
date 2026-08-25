// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_journal_stamps.sh: one function in this file does
// stamp its entry, and a second one does not. Both name their local `entry`.
//
// A file-scoped search for `entry.schema =` would find the first function's
// assignment and pronounce the second one fine -- which is how a lint gate
// ends up reporting green while measuring nothing. The checker scopes the
// search to the declaration's own enclosing block, so this file must still be
// rejected, and it is this fixture that proves it.

#pragma once

namespace lint_fixture {

template <typename Action>
void logStamped(const Action& action) {
    ::morph::journal::LogEntry entry;
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.schema = ::morph::model::detail::actionPayloadSchema<Action>();
    append(std::move(entry));
}

template <typename Action>
void logUnstamped(const Action& action) {
    ::morph::journal::LogEntry entry;
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    append(std::move(entry));
}

}  // namespace lint_fixture
