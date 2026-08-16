// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

/// @file
/// Domain exceptions. A model's `execute(...)` throws one of these; morph
/// captures it as a `std::exception_ptr` and delivers it to the caller's
/// `.onError(...)` callback. See `bookmarks/core/errors.hpp` for the
/// identical shape and rationale this mirrors.

namespace polls {

/// @brief Base of every polls-specific error a model throws.
struct PollsError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief No poll/option exists at the given id — it never existed, or it
///        was deleted.
struct NotFound : PollsError {
    using PollsError::PollsError;
};

/// @brief An action's `validate()` rejected its input.
struct ValidationError : PollsError {
    using PollsError::PollsError;
};

/// @brief A write lost a race: the target row changed between this
///        client's read and its write, or an operation conflicts with
///        the current state (e.g., finalizing an already-finalized poll).
struct Conflict : PollsError {
    using PollsError::PollsError;
};

/// @brief The caller is authenticated, but the target row exists and is
///        owned by a different principal or the caller lacks required
///        permissions (e.g., only the admin can finalize or edit options).
///        Distinguished from `NotFound` deliberately: a model's own re-check
///        needs its own typed signal for authorization failures.
struct Forbidden : PollsError {
    using PollsError::PollsError;
};

}  // namespace polls
