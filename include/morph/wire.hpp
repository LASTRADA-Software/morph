// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <glaze/glaze.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "session.hpp"

namespace morph::wire {

/// @brief JSON wire envelope used between any client and `RemoteServer`.
///
/// Supersedes the legacy pipe-delimited protocol: a single struct carries all
/// request and reply variants, with the `kind` field acting as a discriminator.
/// Empty/zero fields are tolerated — callers populate only what their kind needs.
///
/// @par Discriminator values
/// - `"register"`  — client requests model creation. Uses `typeId`, and
///                   optionally `contextKey` (the new instance's stable
///                   identity, e.g. an account id — see `RemoteServer::setLogProvider`).
/// - `"deregister"` — client destroys an instance. Uses `modelId`.
/// - `"execute"`   — client dispatches an action. Uses `callId`, `modelId`,
///                   `modelType`, `actionType`, `body`, and optionally `session`.
/// - `"ok"`        — server success reply. Uses `callId` if present and `result`.
///                   For `register` replies, `modelId` carries the new id.
/// - `"err"`       — server failure reply. Uses `callId` if present and `message`.
struct Envelope {
    /// @brief Discriminator — see class docstring for valid values.
    std::string kind;

    /// @brief Correlation id for matching async replies to their requests.
    uint64_t callId = 0;

    /// @brief Model type id for `register`.
    std::string typeId;

    /// @brief Stable identity of the model instance being registered (e.g. an
    ///        account id). Empty means "no identity" — the server-side holder
    ///        gets no action log attached even if a `LogProvider` is configured.
    ///        Ignored on every kind other than `register`.
    std::string contextKey;

    /// @brief Existing model instance id for `deregister`, `execute`, `ok(register)`.
    uint64_t modelId = 0;

    /// @brief Model type id for `execute` (routing key in `ActionDispatcher`).
    std::string modelType;

    /// @brief Action type id for `execute` (the second routing key).
    std::string actionType;

    /// @brief Serialized JSON of the action payload (`execute`) or of the
    ///        deserializable result (`ok`).
    std::string body;

    /// @brief Free-text error message on `err`. Empty on success.
    std::string message;

    /// @brief Session context for authorization and routing. Populated on
    ///        `execute`; ignored on every other kind.
    ::morph::session::Context session;
};

/// @brief Builds a `register` envelope.
/// @param typeId     Model type id to register.
/// @param contextKey Optional stable identity for the new instance (default: none).
inline Envelope makeRegister(std::string typeId, std::string contextKey = {}) {
    Envelope env;
    env.kind = "register";
    env.typeId = std::move(typeId);
    env.contextKey = std::move(contextKey);
    return env;
}

/// @brief Builds a `deregister` envelope.
inline Envelope makeDeregister(uint64_t modelId) {
    Envelope env;
    env.kind = "deregister";
    env.modelId = modelId;
    return env;
}

/// @brief Builds an `ok` reply envelope. `body` carries the result for executes,
///        `modelId` carries the new id for register-replies.
inline Envelope makeOk(uint64_t callId = 0, std::string body = {}, uint64_t modelId = 0) {
    Envelope env;
    env.kind = "ok";
    env.callId = callId;
    env.body = std::move(body);
    env.modelId = modelId;
    return env;
}

/// @brief Builds an `err` reply envelope.
inline Envelope makeErr(std::string message, uint64_t callId = 0) {
    Envelope env;
    env.kind = "err";
    env.callId = callId;
    env.message = std::move(message);
    return env;
}

/// @brief Encodes @p env as a single JSON line.
/// @throws std::runtime_error on serialisation failure (should never happen for valid input).
inline std::string encode(const Envelope& env) {
    std::string out;
    if (auto errCode = glz::write_json(env, out)) {
        throw std::runtime_error("envelope encode failed: " + glz::format_error(errCode, out));
    }
    return out;
}

/// @brief Decodes @p json into an `Envelope`.
/// @throws std::runtime_error if @p json is not a valid envelope.
inline Envelope decode(std::string_view json) {
    Envelope env{};
    if (auto errCode = glz::read_json(env, json)) {
        throw std::runtime_error("envelope decode failed: " + glz::format_error(errCode, json));
    }
    return env;
}

}  // namespace morph::wire
