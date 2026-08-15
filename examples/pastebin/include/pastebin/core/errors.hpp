// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

/// @file
/// Domain exceptions. A model's `execute(...)` throws one of these; morph
/// captures it as a `std::exception_ptr` and delivers it to the caller's
/// `.onError(...)` callback on the GUI executor. On a remote backend the
/// `what()` string travels back in the error envelope.

namespace pastebin {

/// @brief Base of every pastebin-specific error a model throws.
struct PastebinError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief No paste exists at the given id (never existed, deleted, or
///        already expired/burned).
struct NotFound : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief The paste existed but its `expiresAt` has passed.
struct Expired : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief The paste existed but its burn-after-reads budget was already
///        exhausted before this read.
struct Burned : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief An action's `validate()` rejected its input.
struct ValidationError : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief `CreatePaste`'s content exceeded the server's message-size bound.
struct TooLarge : PastebinError {
    using PastebinError::PastebinError;
};

/// @brief `EditPaste` lost a race: the paste's content/syntax changed
///        between this client's read and its write. Distinct from
///        `ValidationError` — the request was well-formed and the paste
///        exists and is editable, but the specific edit could not be applied
///        because it was no longer editing what it thought it was editing.
struct Conflict : PastebinError {
    using PastebinError::PastebinError;
};

}  // namespace pastebin
