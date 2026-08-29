// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace crm {

class CrmError : public std::runtime_error {
public:
    explicit CrmError(std::string message) : std::runtime_error{std::move(message)} {}
};

class ValidationError : public CrmError {
public:
    explicit ValidationError(std::string message) : CrmError{std::move(message)} {}
};

class NotFound : public CrmError {
public:
    explicit NotFound(std::string message) : CrmError{std::move(message)} {}
};

class Forbidden : public CrmError {
public:
    explicit Forbidden(std::string message) : CrmError{std::move(message)} {}
};

/// @brief Thrown when an action collides with a decision already taken, or
///        with an entity-capacity rule (kanban's WIP-limit analogue for a
///        pipeline stage).
class Conflict : public CrmError {
public:
    explicit Conflict(std::string message) : CrmError{std::move(message)} {}
};

/// @brief Thrown when an opportunity or lead is asked to make a transition
///        its current stage/status does not allow.
///
/// The pipeline is a guarded state machine (README build order §3), so an
/// illegal edge is a rejected action rather than a silently ignored one.
class IllegalTransition : public CrmError {
public:
    explicit IllegalTransition(std::string message) : CrmError{std::move(message)} {}
};

/// @brief Thrown when a mutating action dispatches with no authenticated
///        principal.
///
/// crm's field-level audit history (README build order §6) needs a real
/// author on every entry; every mutating action checks rather than assumes
/// the authorizer ran (the authorize/authenticate empty-principal TOCTOU
/// `LADDER.md`'s "Authorization is per-execute" section names).
class EmptyPrincipalError : public CrmError {
public:
    EmptyPrincipalError() : CrmError{"mutating action dispatched with an empty principal"} {}
};

}  // namespace crm
