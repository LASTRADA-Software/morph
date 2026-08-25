// SPDX-License-Identifier: Apache-2.0
//
// Fixture for scripts/check_journal_stamps.sh: a construction that genuinely
// cannot derive a fingerprint says so in place, and is accepted.

#pragma once

namespace lint_fixture {

inline void recordUndecodablePayload(std::string actionType, std::string payload) {
    // journal-stamp-exempt: there is no `Action` type here to derive a
    // fingerprint from -- this path exists precisely because the payload could
    // not be decoded into one, and stamping this build's fingerprint would be
    // a claim about a shape this build never saw. The reason deliberately runs
    // past three lines, so that the checker is shown reading the whole comment
    // block rather than a fixed-size window above the declaration.
    ::morph::journal::LogEntry entry;
    entry.actionType = std::move(actionType);
    entry.payload = std::move(payload);
    append(std::move(entry));
}

}  // namespace lint_fixture
