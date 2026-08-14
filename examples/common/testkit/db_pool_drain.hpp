// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/Pool.hpp>

#include <cstddef>
#include <vector>

/// @file
/// `drainPoolIdleMappers()` — forces the next
/// `Lightweight::GlobalDataMapperPool().Acquire()` anywhere in the process to
/// actually construct a fresh `DataMapper` (and so a fresh `SqlConnection`),
/// instead of possibly handing back an idle, already-connected one left over
/// from an earlier acquisition. See `db_busy_fixture.hpp`'s
/// "`SetPostConnectedHook` and `GlobalDataMapperPool()`" note for the
/// concrete problem this solves: a caller's `SetPostConnectedHook` override
/// only fires when the pool actually creates a new `SqlConnection`, and once
/// any rung's model acquires its persistence through the pool rather than
/// owning a connection for its own lifetime, a test relying on "the code
/// under test's connection opens fresh, under my hook" needs this to make
/// that a hard guarantee rather than an incidental one.

namespace morph::ladder::testkit {

/// @brief Forces the pool empty, so the very next `Acquire()` anywhere
///        constructs a genuinely new mapper.
///
/// `Pool::Acquire()`'s non-blocking growth strategies (`BoundedOverflow`,
/// morph's own configured default, and `UnboundedGrow`) only ever construct
/// a fresh mapper when the pool's idle list is empty at the moment of the
/// call; otherwise they hand back whatever sits at the back of that list.
/// So: drain it. Acquiring and **holding** every currently-idle mapper
/// (never returning them while held) is the only way to empty that list
/// from outside the pool — there is no reset/clear method — after which the
/// very next `Acquire()` anywhere, while this batch is still held, is
/// guaranteed to construct new.
///
/// `Config.maxSize` acquisitions are always enough regardless of the pool's
/// prior idle count, since `BoundedOverflow`'s own `Return()` never keeps
/// more than `maxSize` idle mappers at once. Releasing the returned batch
/// (by letting it go out of scope, or calling `.clear()` on it) is safe at
/// any point after the acquisition this call was meant to protect has
/// already happened — it does not undo that acquisition.
///
/// @return The drained batch. Keep it alive (e.g. as a local `auto`) across
///         the acquisition that must be fresh; release it once that
///         acquisition has happened.
[[nodiscard]] inline std::vector<::Lightweight::DataMapperPool::PooledDataMapper> drainPoolIdleMappers() {
    std::vector<::Lightweight::DataMapperPool::PooledDataMapper> held;
    held.reserve(::Lightweight::DefaultPoolConfig.maxSize);
    for (std::size_t i = 0; i < ::Lightweight::DefaultPoolConfig.maxSize; ++i) {
        held.push_back(::Lightweight::GlobalDataMapperPool().Acquire());
    }
    return held;
}

}  // namespace morph::ladder::testkit
