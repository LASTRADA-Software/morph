// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session.hpp>
#include <string>
#include <utility>

/// @file
/// The scaffolding every lims test needs: a scoped authenticated principal.
///
/// Models in this rung refuse to mutate anything without one
/// (`lims::requirePrincipal`), which is deliberate — the README names
/// empty-principal audit entries as disqualifying — so almost every test
/// declares one.

namespace lims::test {

/// @brief Builds a session context naming @p principal.
/// @param principal The authenticated principal's name.
/// @return The context.
[[nodiscard]] inline morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

/// @brief Installs @p principal as the authenticated principal for this scope.
///
/// `_ctx` is declared before `_scope` on purpose: `ScopedContext` holds a
/// reference to it, so the member order *is* the lifetime guarantee.
class ScopedPrincipal {
public:
    /// @param principal The name every action in this scope executes as.
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

}  // namespace lims::test
