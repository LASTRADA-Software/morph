// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <string>
#include <utility>

#include "test_support.hpp"

namespace morph::testing {

/// @brief Binds through @p backend's `bindModel` and waits for the id.
///
/// What a test that drives a backend directly writes instead of the removed
/// synchronous acquire verbs: the request's shape selects private,
/// register-or-attach, or re-point (see `backend::detail::BindRequest`).
/// @param backend The backend.
/// @param request The bind request.
/// @return The bound id; the bind's failure is rethrown.
inline ::morph::exec::detail::ModelId bindNow(::morph::backend::detail::IBackend& backend,
                                              ::morph::backend::detail::BindRequest request) {
    return awaitAnswer([&backend, &request](::morph::exec::IExecutor& reply) {
        return backend.bindModel(std::move(request), reply);
    });
}

/// @brief Register-or-attach the shared instance for (@p typeId, @p primary).
/// @param backend    The backend.
/// @param typeId     Model type id.
/// @param factory    Factory for a fresh instance.
/// @param primary    Canonical primary key.
/// @param contextKey Entity key; defaults to none.
/// @return The bound id.
inline ::morph::exec::detail::ModelId bindShared(
    ::morph::backend::detail::IBackend& backend, std::string typeId,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory, std::string primary,
    std::string contextKey = {}) {
    return bindNow(backend, ::morph::backend::detail::BindRequest{.typeId = std::move(typeId),
                                                                  .factory = std::move(factory),
                                                                  .contextKey = std::move(contextKey),
                                                                  .primary = std::move(primary),
                                                                  .current = {}});
}

/// @brief Re-point from @p current to the shared instance for (@p typeId, @p primary).
/// @param backend    The backend.
/// @param typeId     Model type id.
/// @param factory    Factory for a fresh instance.
/// @param primary    Canonical primary key.
/// @param current    The instance currently held.
/// @param contextKey Entity key; defaults to none.
/// @return The bound id.
inline ::morph::exec::detail::ModelId bindAttach(
    ::morph::backend::detail::IBackend& backend, std::string typeId,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory, std::string primary,
    ::morph::exec::detail::ModelId current, std::string contextKey = {}) {
    return bindNow(backend, ::morph::backend::detail::BindRequest{.typeId = std::move(typeId),
                                                                  .factory = std::move(factory),
                                                                  .contextKey = std::move(contextKey),
                                                                  .primary = std::move(primary),
                                                                  .current = current});
}

}  // namespace morph::testing
