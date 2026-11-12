// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <morph/core/action_refusal.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lims {

/// @brief Every lims refusal: an `ActionRefusal`, so its code and arguments reach the client beside the message.
///
/// Each subclass has a default code naming its class, which the kit's catalogue words as the class's sentence, and a
/// constructor taking a specific code and arguments for a refusal a screen shows in its own words. The message is
/// what lims logs and its tests read; a screen never shows it.
class LimsError : public morph::model::ActionRefusal {
public:
    /// @brief A refusal.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    LimsError(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : ActionRefusal{std::move(code), std::move(args), std::move(message)} {}
};

class ValidationError : public LimsError {
public:
    /// @brief A refusal worded by its class.
    /// @param message The text for logs and tests.
    explicit ValidationError(std::string message) : LimsError{"lims.refusal.validation", {}, std::move(message)} {}
    /// @brief A refusal with a specific code.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    ValidationError(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : LimsError{std::move(code), std::move(args), std::move(message)} {}
};

class NotFound : public LimsError {
public:
    /// @brief A refusal worded by its class.
    /// @param message The text for logs and tests.
    explicit NotFound(std::string message) : LimsError{"lims.refusal.notFound", {}, std::move(message)} {}
    /// @brief A refusal with a specific code.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    NotFound(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : LimsError{std::move(code), std::move(args), std::move(message)} {}
};

class Forbidden : public LimsError {
public:
    /// @brief A refusal worded by its class.
    /// @param message The text for logs and tests.
    explicit Forbidden(std::string message) : LimsError{"lims.refusal.forbidden", {}, std::move(message)} {}
    /// @brief A refusal with a specific code.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    Forbidden(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : LimsError{std::move(code), std::move(args), std::move(message)} {}
};

/// @brief Thrown when an action collides with something already there —
///        resolving a conflict that somebody else has already resolved, or
///        creating a company or site under a name that is taken.
///
/// Distinct from `IllegalTransition`, which is about the sample's lifecycle:
/// this one is about a *record* that has already reached a terminal state, or
/// a uniqueness the action would break.
class Conflict : public LimsError {
public:
    /// @brief A refusal worded by its class.
    /// @param message The text for logs and tests.
    explicit Conflict(std::string message) : LimsError{"lims.refusal.conflict", {}, std::move(message)} {}
    /// @brief A refusal with a specific code.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    Conflict(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : LimsError{std::move(code), std::move(args), std::move(message)} {}
};

/// @brief Thrown when an amount, a sum or a rounded value is outside what the books can hold exactly.
///
/// The ledger refuses rather than saturates: a balance clamped to the largest representable value is a wrong
/// balance that looks plausible.
class OutOfRange : public LimsError {
public:
    /// @brief A refusal worded by its class.
    /// @param message The text for logs and tests.
    explicit OutOfRange(std::string message) : LimsError{"lims.refusal.outOfRange", {}, std::move(message)} {}
    /// @brief A refusal with a specific code.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    OutOfRange(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : LimsError{std::move(code), std::move(args), std::move(message)} {}
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
    /// @brief A refusal worded by its class.
    /// @param message The text for logs and tests.
    explicit IllegalTransition(std::string message)
        : LimsError{"lims.refusal.illegalTransition", {}, std::move(message)} {}
    /// @brief A refusal with a specific code.
    /// @param code    The dotted code.
    /// @param args    The named arguments of the code's sentence.
    /// @param message The text for logs and tests.
    IllegalTransition(std::string code, std::vector<morph::model::RefusalArg> args, std::string message)
        : LimsError{std::move(code), std::move(args), std::move(message)} {}
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
    EmptyPrincipalError()
        : LimsError{"lims.refusal.emptyPrincipal", {}, "mutating action dispatched with an empty principal"} {}
};

/// @brief The class codes every lims refusal has by default, which the kit's catalogues word.
inline constexpr std::array<std::string_view, 7> kClassRefusalCodes{
    "lims.refusal.validation", "lims.refusal.notFound",          "lims.refusal.forbidden",     "lims.refusal.conflict",
    "lims.refusal.outOfRange", "lims.refusal.illegalTransition", "lims.refusal.emptyPrincipal"};

/// @brief The specific codes org throws; none yet, org's refusals are worded by their class.
inline constexpr std::array<std::string_view, 0> kOrgRefusalCodes{};

}  // namespace lims
