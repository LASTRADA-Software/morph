// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief The asynchronous primitives, on their own — the cheap include.
///
/// `Completion`, `IExecutor` and its implementations, `StrandExecutor` and
/// `CallbackScope` are useful without a model, a registry, a wire envelope or a
/// schema, and they are the part of morph that costs almost nothing to compile.
/// Nothing here reaches glaze.
///
/// Measured with clang 22.1.8, `-O2 -fsyntax-only`, one translation unit per
/// header, best of three:
///
/// | header | CPU s | preprocessed lines |
/// |---|---|---|
/// | `core/executor.hpp` | 1.16 | 124,855 |
/// | `core/completion.hpp` | 1.20 | 127,450 |
/// | `core/strand.hpp` | 1.20 | 127,217 |
/// | `core/bridge.hpp` | 3.76 | 267,827 |
///
/// A consumer that wants the primitives and reaches for `bridge.hpp` — the
/// obvious header, and the one every example includes — pays roughly three
/// times over for a schema generator and a JSON codec it never calls. This
/// header exists so the cheap path has a name.
///
/// It is a facade and nothing else: it declares no symbol of its own, so
/// including it is exactly equivalent to including the four headers below.

#include "callback_scope.hpp"
#include "completion.hpp"
#include "executor.hpp"
#include "strand.hpp"
