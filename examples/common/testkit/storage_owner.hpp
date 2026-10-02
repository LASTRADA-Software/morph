// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/executor.hpp>

/// @file
/// The owner a ladder test gives the storage it builds on its own thread: an
/// action log, an offline queue. A mirror of `morph::testing::storageOwner()`
/// (`tests/test_support.hpp`), which has no include path from `examples/`.

namespace morph::ladder::testkit {

/// @brief This thread's storage owner: one `MainThreadExecutor` per thread.
///
/// Storage constructed on the test's thread counts that thread as its owner,
/// so the test body and a model it calls directly use the storage's verbs at
/// once. A write made from another thread (a model's strand on a pool) is
/// posted here and runs when the test pumps it (`storageOwner().drain()`).
/// @return This thread's storage owner.
inline ::morph::exec::MainThreadExecutor& storageOwner() {
    thread_local ::morph::exec::MainThreadExecutor owner;
    return owner;
}

}  // namespace morph::ladder::testkit
