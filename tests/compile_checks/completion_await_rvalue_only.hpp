// SPDX-License-Identifier: Apache-2.0
//
// Compile-time check: a morph::async::Completion<T> can be awaited only as an
// rvalue. Awaiting consumes the completion (its state moves into the awaiter),
// so `co_await completion` on an lvalue must not compile, and
// `co_await std::move(completion)` must. See docs/spec/core/coroutines.md.
// Included by tests/test_coroutine_client.cpp, which is what compiles it.

#pragma once
#include <morph/core/completion.hpp>
#include <utility>

namespace morph::compile_checks {

/// Whether `co_await` accepts a @p C lvalue.
template <typename C>
concept AwaitableAsLvalue = requires(C& completion) { completion.operator co_await(); };

/// Whether `co_await` accepts a @p C rvalue.
template <typename C>
concept AwaitableAsRvalue = requires(C& completion) { std::move(completion).operator co_await(); };

static_assert(AwaitableAsRvalue<::morph::async::Completion<int>>);
static_assert(!AwaitableAsLvalue<::morph::async::Completion<int>>);

}  // namespace morph::compile_checks
