// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

/// @file
/// Kanban's typed exception hierarchy -- mirrors
/// `bookmarks::core::errors.hpp`/`polls::core::errors.hpp` exactly: one base
/// (`KanbanError`), four concrete types distinguishing the outcomes a
/// caller's `.onError(...)` needs to tell apart.

namespace kanban {

/// @brief Base for every exception `BoardModel`/`ProjectAdminModel` throws.
class KanbanError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// @brief An action's `validate()` rejected the request.
class ValidationError : public KanbanError {
  public:
    using KanbanError::KanbanError;
};

/// @brief The named project/column/task/etc. does not exist (or does not
///        belong to the project it was claimed to).
class NotFound : public KanbanError {
  public:
    using KanbanError::KanbanError;
};

/// @brief The caller's role does not permit the requested action.
class Forbidden : public KanbanError {
  public:
    using KanbanError::KanbanError;
};

/// @brief The action cannot proceed given the target's current state (WIP
///        limit exceeded, project archived, etc.).
class Conflict : public KanbanError {
  public:
    using KanbanError::KanbanError;
};

}  // namespace kanban
