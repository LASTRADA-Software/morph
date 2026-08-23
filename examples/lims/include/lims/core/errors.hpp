// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace lims {

class LimsError : public std::runtime_error {
  public:
    explicit LimsError(std::string message) : std::runtime_error{std::move(message)} {}
};

class ValidationError : public LimsError {
  public:
    explicit ValidationError(std::string message) : LimsError{std::move(message)} {}
};

class NotFound : public LimsError {
  public:
    explicit NotFound(std::string message) : LimsError{std::move(message)} {}
};

class Forbidden : public LimsError {
  public:
    explicit Forbidden(std::string message) : LimsError{std::move(message)} {}
};

/// @brief Thrown when an action collides with a decision already taken —
///        resolving a conflict that somebody else has already resolved.
///
/// Distinct from `IllegalTransition`, which is about the sample's lifecycle:
/// this one is about a *record* that has already reached a terminal state.
class Conflict : public LimsError {
  public:
    explicit Conflict(std::string message) : LimsError{std::move(message)} {}
};

/// @brief Thrown when a sample is asked to make a transition its current
///        state does not allow.
///
/// The lifecycle is a guarded state machine (README build order §2), so an
/// illegal edge is a rejected action rather than a silently ignored one — a
/// published sample that quietly accepted a new result would be a regulatory
/// problem, not a UI inconvenience.
class IllegalTransition : public LimsError {
  public:
    explicit IllegalTransition(std::string message) : LimsError{std::move(message)} {}
};

/// @brief Thrown when a mutating action dispatches with no authenticated
///        principal.
///
/// A 21 CFR Part 11-style audit trail whose entries can carry an empty
/// author is not an audit trail. The README names the empty-principal TOCTOU
/// as disqualifying for this rung specifically, so every mutating action
/// checks rather than assuming the authorizer ran.
class EmptyPrincipalError : public LimsError {
  public:
    EmptyPrincipalError() : LimsError{"mutating action dispatched with an empty principal"} {}
};

}  // namespace lims
