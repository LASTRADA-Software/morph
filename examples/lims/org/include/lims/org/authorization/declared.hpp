// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// Declared authorization: per action, the role it needs, the scope its payload names, an optional value limit and
/// an optional separation-of-duties pair, declared once beside the action by the module that owns it and checked by
/// `requireAuthorized` inside the model.
///
/// The check runs in the model because only the model has the decoded payload: `IAuthorizer::authorize` at the
/// server edge is handed the model and action ids, never the payload, so the edge can ask whether a principal holds
/// the role somewhere (`lims::app::declaredRequirements`) and nothing more.

namespace lims {

/// @brief One action's declared authorization; an action with no specialisation declares none.
///
/// A specialisation, beside the action's registration in the module that owns it, provides one of:
/// - `static constexpr LimsRole role` — the role the action needs;
/// - `static constexpr bool anyRole = true` — some role, whichever: the principal must hold at least one role in the
///   scope, and the action does not care which;
///
/// and, each only where the action needs it:
/// - `static Scope scopeOf(Action const&)` — the scope the action's payload names, read from its scope field; without
///   it the role is needed in some scope, any. A role held at a company holds at its sites;
/// - `static constexpr bool scopeFromModel = true` — the payload names no scope but the model knows it (the site of
///   the sample the handler is attached to): the model then judges with `requireAuthorizedIn` and passes the scope;
/// - `static ::morph::math::Rational amountOf(Action const&)` and
///   `static std::optional<::morph::math::Rational> limitOf(std::string_view principal, Scope const&)` — a value
///   limit: the principal may act on amounts up to their limit in the scope, and on none without one;
/// - `static constexpr std::string_view separatedFrom` (the action id of the pair's first half),
///   `static constexpr std::string_view sameActorRefusal` (the refusal's words) and
///   `static std::optional<std::string> firstPerformerOf(Action const&)` (who performed the first half on the same
///   subject) — a separation-of-duties pair: the principal may not be that performer;
/// - `static constexpr bool notOnSelf = true` and `static std::string subjectOf(Action const&)` — the principal the
///   action is done to: the caller may not be that principal, so nobody sets their own limit or grants themselves a
///   role.
/// @tparam Action The action.
template <typename Action>
struct Authorization;

/// @brief Whether @p Action declares a specific role.
/// @tparam Action The action.
template <typename Action>
concept DeclaresRole = requires {
    { Authorization<Action>::role } -> std::convertible_to<LimsRole>;
};

/// @brief Whether @p Action declares that some role, whichever, is enough.
/// @tparam Action The action.
template <typename Action>
concept DeclaresAnyRole = requires { requires Authorization<Action>::anyRole; };

/// @brief Whether @p Action declares a rule.
/// @tparam Action The action.
template <typename Action>
concept DeclaresRule = DeclaresRole<Action> || DeclaresAnyRole<Action>;

/// @brief Whether @p Action's rule leaves the scope to the model, which passes it to `requireAuthorizedIn`.
/// @tparam Action The action.
template <typename Action>
concept DeclaresModelScope = DeclaresRule<Action> && requires { requires Authorization<Action>::scopeFromModel; };

/// @brief The role @p Action declares.
/// @tparam Action A declared action.
/// @return The role; nothing for an action that accepts any role.
template <typename Action>
    requires DeclaresRule<Action>
[[nodiscard]] constexpr std::optional<LimsRole> declaredRole() {
    if constexpr (DeclaresRole<Action>) {
        return Authorization<Action>::role;
    } else {
        return std::nullopt;
    }
}

/// @brief Whether @p Action's rule names the scope its payload holds.
/// @tparam Action The action.
template <typename Action>
concept DeclaresScope = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::scopeOf(action) } -> std::convertible_to<Scope>;
};

/// @brief Whether @p Action's rule declares a value limit. A limit is judged in a scope, so it needs one.
/// @tparam Action The action.
template <typename Action>
concept DeclaresValueLimit =
    DeclaresScope<Action> && requires(Action const& action, std::string_view principal, Scope const& scope) {
        { Authorization<Action>::amountOf(action) } -> std::convertible_to<::morph::math::Rational>;
        {
            Authorization<Action>::limitOf(principal, scope)
        } -> std::convertible_to<std::optional<::morph::math::Rational>>;
    };

/// @brief Whether @p Action is the second half of a declared separation-of-duties pair.
/// @tparam Action The action.
template <typename Action>
concept DeclaresSeparation = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::separatedFrom } -> std::convertible_to<std::string_view>;
    { Authorization<Action>::sameActorRefusal } -> std::convertible_to<std::string_view>;
    { Authorization<Action>::firstPerformerOf(action) } -> std::convertible_to<std::optional<std::string>>;
};

/// @brief Whether @p Action refuses a caller who is its own subject.
/// @tparam Action The action.
template <typename Action>
concept DeclaresNotOnSelf = DeclaresRule<Action> && requires(Action const& action) {
    requires Authorization<Action>::notOnSelf;
    { Authorization<Action>::subjectOf(action) } -> std::convertible_to<std::string>;
};

/// @brief A rule needing @p Role at the company the action's `company` field names: the shape most ERP actions have.
/// @tparam Action An action with a `CompanyId company` member.
/// @tparam Role The role.
template <typename Action, LimsRole Role>
struct CompanyRule {
    /// @brief The role.
    static constexpr LimsRole role = Role;

    /// @brief The company the payload names.
    /// @param action The action.
    /// @return Its company.
    [[nodiscard]] static Scope scopeOf(Action const& action) { return Scope{action.company}; }
};

/// @brief What a declared rule decides.
enum class Verdict : std::uint8_t {
    Allowed,      ///< Every declared condition holds.
    MissingRole,  ///< The principal does not hold the role (any role, for a rule that accepts any) in the scope.
    SameActor,    ///< The principal performed the pair's first half on the same subject.
    OnSelf,       ///< The principal is the subject of an action that may not be done to oneself.
    NoLimit,      ///< The action carries an amount and the principal has no limit in the scope.
    OverLimit,    ///< The amount is above the principal's limit in the scope.
};

/// @brief Everything a rule is judged on, gathered by `requireAuthorized` from the declaration and the payload.
struct AuthorizationFacts {
    /// @brief The authenticated principal.
    std::string principal;
    /// @brief The role the action needs; empty when any role will do.
    std::optional<LimsRole> required = LimsRole::Analyst;
    /// @brief The roles the principal holds in the scope (anywhere, for a rule without one).
    std::vector<LimsRole> held;
    /// @brief Who performed the pair's first half on the same subject; empty without a pair or a performer.
    std::optional<std::string> firstPerformer;
    /// @brief The principal the action is done to; empty for an action that may be done to oneself.
    std::optional<std::string> notOnSelfSubject;
    /// @brief The amount the action acts on; empty without a value limit.
    std::optional<::morph::math::Rational> amount;
    /// @brief The principal's limit in the scope; empty when they have none.
    std::optional<::morph::math::Rational> limit;
};

/// @brief Judges @p facts. Pure: the same facts always give the same verdict.
///
/// The role is the first question, the pair the second, self-application the third, the limit the fourth, so a refusal
/// names the first reason that applies.
/// @param facts The facts.
/// @return The verdict.
[[nodiscard]] Verdict authorizationVerdict(AuthorizationFacts const& facts);

/// @brief The words a refusal uses for a required role.
/// @param role The role; empty for any role.
/// @return "the <role> role", or "any role".
[[nodiscard]] inline std::string roleWords(std::optional<LimsRole> role) {
    return role.has_value() ? "the " + std::string{roleName(*role)} + " role" : std::string{"any role"};
}

/// @brief Throws `Forbidden` unless the authenticated principal holds @p Action's declared role (any role, for a
///        rule that accepts any) in some scope.
///
/// Asked before the rows an action names are looked up, so a caller without the role learns nothing about which ids
/// exist; `requireAuthorized` then judges the scope once the rows are known.
/// @tparam Action A declared action.
/// @throws Forbidden when the principal holds the role in no scope.
template <typename Action>
    requires DeclaresRule<Action>
void requireRoleSomewhere() {
    std::string const principal = ::morph::session::current()->principal;
    auto const held = OrgModel::rolesAnywhere(principal);
    auto const role = declaredRole<Action>();
    if (role.has_value() ? std::ranges::find(held, *role) == held.end() : held.empty()) {
        throw Forbidden{std::string{::morph::model::ActionTraits<Action>::typeId()} + ": '" + principal +
                        "' does not hold " + roleWords(role)};
    }
}

namespace detail {

/// @brief Judges @p action against its declaration in @p scope, or in any scope when there is none.
/// @tparam Action A declared action.
/// @param action The action.
/// @param scope The scope the rule is judged in; empty for a rule that needs the role in some scope.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
void judge(Action const& action, std::optional<Scope> const& scope) {
    using Rule = Authorization<Action>;
    std::string const principal = ::morph::session::current()->principal;
    auto const role = declaredRole<Action>();
    AuthorizationFacts facts{.principal = principal,
                             .required = role,
                             .held = {},
                             .firstPerformer = std::nullopt,
                             .notOnSelfSubject = std::nullopt,
                             .amount = std::nullopt,
                             .limit = std::nullopt};
    std::string where = "in any scope";
    if (scope.has_value()) {
        facts.held = OrgModel::rolesIn(principal, *scope);
        where = "at " + scopeName(*scope);
        if constexpr (DeclaresValueLimit<Action>) {
            facts.amount = Rule::amountOf(action);
            facts.limit = Rule::limitOf(principal, *scope);
        }
    } else {
        facts.held = OrgModel::rolesAnywhere(principal);
    }
    if constexpr (DeclaresSeparation<Action>) {
        facts.firstPerformer = Rule::firstPerformerOf(action);
    }
    if constexpr (DeclaresNotOnSelf<Action>) {
        facts.notOnSelfSubject = Rule::subjectOf(action);
    }
    std::string const actionType{::morph::model::ActionTraits<Action>::typeId()};
    Verdict const verdict = authorizationVerdict(facts);
    if (verdict == Verdict::Allowed) {
        return;
    }
    if (verdict == Verdict::MissingRole) {
        throw Forbidden{actionType + ": '" + principal + "' does not hold " + roleWords(role) + " " + where};
    }
    if (verdict == Verdict::OnSelf) {
        throw Forbidden{actionType + ": '" + principal + "' may not do this to themselves"};
    }
    if (verdict == Verdict::NoLimit) {
        throw Forbidden{actionType + ": '" + principal + "' has no value limit " + where};
    }
    if (verdict == Verdict::OverLimit) {
        throw Forbidden{actionType + ": the amount is above '" + principal + "''s value limit " + where};
    }
    if constexpr (DeclaresSeparation<Action>) {
        throw Forbidden{actionType + ": '" + principal + "' " + std::string{Rule::sameActorRefusal}};
    }
    throw Forbidden{actionType + ": refused"};
}

}  // namespace detail

/// @brief Checks @p action against its declaration, as the authenticated principal.
///
/// Called by the model on every dispatch path, after the payload is validated and, where the rule needs a row, before
/// that row is looked up for anything but its scope, so a refusal is about permission and never a disguised missing
/// row: an unknown scope holds no role, so it is refused as a missing role.
/// @tparam Action A declared action whose scope the payload names, or that needs none.
/// @param action The action.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
    requires DeclaresRule<Action> && (!DeclaresModelScope<Action>)
void requireAuthorized(Action const& action) {
    if constexpr (DeclaresScope<Action>) {
        detail::judge(action, std::optional<Scope>{Authorization<Action>::scopeOf(action)});
    } else {
        detail::judge(action, std::nullopt);
    }
}

/// @brief Checks @p action against its declaration in @p scope, as the authenticated principal: for a rule whose scope
///        the model knows and the payload does not carry.
/// @tparam Action A declared action with `scopeFromModel`.
/// @param action The action.
/// @param scope The scope the model resolved; an empty scope when the row it comes from does not exist, which holds
///        no role and is refused as a missing role.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
    requires DeclaresModelScope<Action>
void requireAuthorizedIn(Action const& action, Scope const& scope) {
    detail::judge(action, std::optional<Scope>{scope});
}

/// @brief One declared role as the server edge can use it: by action id, with no payload.
struct RoleRequirement {
    /// @brief The action id.
    std::string_view actionType;
    /// @brief The role it needs, somewhere; empty when any role will do.
    std::optional<LimsRole> role = LimsRole::Analyst;
};

/// @brief @p Action's declared role, for the edge's table.
/// @tparam Action A declared action.
/// @return Its action id and role.
template <typename Action>
    requires DeclaresRule<Action>
[[nodiscard]] constexpr RoleRequirement requirementOf() {
    return RoleRequirement{.actionType = ::morph::model::ActionTraits<Action>::typeId(),
                           .role = declaredRole<Action>()};
}

}  // namespace lims
