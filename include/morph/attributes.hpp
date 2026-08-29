// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @brief Declares that the annotated parameter is *borrowed*, not owned: what
/// it refers to must outlive the value the call produces.
///
/// Applied to a function parameter, it says the return value may refer to that
/// parameter's referent. Applied to the implicit object parameter — written
/// after the parameter list and any `const`/`noexcept` — it says the same of
/// `*this`, which is the shape of every accessor that hands out a reference
/// into a member, and of every fluent method that returns `*this`. Applied to a
/// constructor parameter, it says the constructed object keeps referring to the
/// argument for as long as it lives, which is the "must outlive" half of the
/// destruction-ordering rules in `docs/spec/concurrency_and_lifetimes.md`.
///
/// The attribute states a contract that already holds; it generates no code and
/// changes no behaviour. What it buys is that Clang can now diagnose a call site
/// that breaks the contract — and, under `-Weverything`, that it stops asking
/// for the annotation on every declaration that visibly needs one.
///
/// Clang spells it `[[clang::lifetimebound]]` and MSVC `[[msvc::lifetimebound]]`;
/// GCC has no equivalent, and a raw `[[clang::…]]` there is a `-Wattributes`
/// diagnostic — a build break under the project's `-Werror`. So every use in
/// morph goes through this macro, which expands to nothing on a compiler that
/// does not know the attribute.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define MORPH_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define MORPH_LIFETIMEBOUND [[msvc::lifetimebound]]
#endif
#endif

#ifndef MORPH_LIFETIMEBOUND
#define MORPH_LIFETIMEBOUND
#endif
