// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "completion.hpp"
#include "executor.hpp"
#include "wire.hpp"

/// @file
/// @brief `IEnvelopeTransport`: a transport that sends a request envelope of a kind its backend verbs do not build,
///        such as the UI service's `ui-*` kinds, and settles with the peer's reply.
///
/// Specified in `docs/spec/core/backend.md`, "Envelope requests".

namespace morph::backend {

/// @brief What a wire transport offers beside `IBackend`: one request, one reply, matched by `callId`.
///
/// `SocketBackend` and `QtWebSocketBackend` implement it. A backend without a wire, such as `LocalBackend`, has no
/// peer and does not.
class IEnvelopeTransport {
public:
    IEnvelopeTransport() = default;
    IEnvelopeTransport(IEnvelopeTransport const&) = delete;
    IEnvelopeTransport& operator=(IEnvelopeTransport const&) = delete;
    IEnvelopeTransport(IEnvelopeTransport&&) = delete;
    IEnvelopeTransport& operator=(IEnvelopeTransport&&) = delete;

    /// @brief Sends one request envelope and settles with its reply.
    ///
    /// The envelope travels with the session it carries: the transport assigns its `callId` and stamps nothing else,
    /// because a protocol that passes its session explicitly (`ui::UiClient::open`) must not have it replaced by the
    /// transport's default. The reply resolves the `Completion` whether it is `ok` or `err`, since reading an `err`
    /// is the caller's protocol; only a failure to deliver the request or its reply rejects it. A request made while
    /// the connection is down is held and sent at the next connect; `cancelPending`, a drop and the transport's
    /// destruction reject what is pending or held.
    /// @param envelope The request. Its `callId` is replaced.
    /// @param cbExec   Executor the answer is delivered on. Borrowed: it must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the reply envelope, or rejected with the transport's failure
    ///         (`DisconnectedError` for a dropped or closed connection).
    virtual ::morph::async::Completion<::morph::wire::Envelope> request(::morph::wire::Envelope envelope,
                                                                        ::morph::exec::IExecutor& cbExec) = 0;

protected:
    ~IEnvelopeTransport() = default;
};

}  // namespace morph::backend
