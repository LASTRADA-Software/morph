// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "testkit/backend_rig.hpp"

#include <cstddef>
#include <memory>
#include <vector>

/// @file
/// `ClientPool<Presenter>` -- N presenter instances over one `BackendRig`'s
/// N clients, the multi-client convergence-test scaffold `examples/
/// TESTING.md` names as rung 3's obligation (design spec §6 -- absorbed
/// into rung 4's scope).

namespace morph::ladder::testkit {

template <typename Presenter>
class ClientPool {
  public:
    /// @brief Constructs one `Presenter` per client in @p rig, forwarding
    ///        each client's `(Bridge&, IExecutor*)` pair to `Presenter`'s
    ///        constructor -- the same pair every rung's presenter already
    ///        takes (`examples/TESTING.md`'s presenter-architecture rule 2).
    /// @param rig The already-constructed `BackendRig` to build presenters
    ///        over. Must outlive this `ClientPool`.
    /// @param nClients How many presenters to construct -- the same count
    ///        passed to @p rig's own constructor. `BackendRig` has no
    ///        accessor for the count it was built with (its constructor
    ///        takes `nClients` but never stores it for later retrieval), so
    ///        the caller -- which already has that value on hand for the
    ///        `BackendRig{mode, nClients, ...}` call -- passes it again here.
    ClientPool(BackendRig& rig, std::size_t nClients) {
        _presenters.reserve(nClients);
        for (std::size_t i = 0; i < nClients; ++i) {
            _presenters.push_back(std::make_unique<Presenter>(rig.bridge(i), rig.executor()));
        }
    }

    /// @return The presenter for client @p index.
    [[nodiscard]] Presenter& at(std::size_t index) { return *_presenters.at(index); }

    /// @return How many presenters this pool holds.
    [[nodiscard]] std::size_t size() const noexcept { return _presenters.size(); }

  private:
    std::vector<std::unique_ptr<Presenter>> _presenters;
};

}  // namespace morph::ladder::testkit
