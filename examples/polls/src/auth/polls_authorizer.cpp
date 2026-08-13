// SPDX-License-Identifier: Apache-2.0
#include "polls/auth/polls_authorizer.hpp"

namespace polls::auth {

bool PollsAuthorizer::authorizeRegister(const ::morph::session::Context& /*ctx*/,
                                        std::string_view /*modelType*/) const {
    return true;
}

bool PollsAuthorizer::authorizeInstance(const ::morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                        std::string_view /*actionType*/, std::uint64_t /*modelId*/,
                                        std::string_view /*ownerPrincipal*/) const {
    return true;
}

}  // namespace polls::auth
