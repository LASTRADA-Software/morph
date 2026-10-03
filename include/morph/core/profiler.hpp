// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// @brief Compile-time profiler instrumentation: Tracy zones, plots, messages
///        and thread names, behind morph's own macro names.
///
/// Configure with `-DMORPH_ENABLE_TRACY=ON` and the macros below forward to
/// Tracy (`MORPH_TRACY_ENABLED` is then defined on every target that links
/// `morph::morph`). Otherwise each macro is a stub that names its arguments
/// only inside `decltype`, an unevaluated operand: a disabled build evaluates
/// nothing, warns about no unused argument, and needs no Tracy headers.
///
/// @par Why morph's names and not Tracy's
/// Lightweight defines stubs under Tracy's own names (`ZoneScoped`,
/// `ZoneScopedN`, `TracyPlot`, ...) when its own Tracy option is off, and an
/// application may include Lightweight and morph in one translation unit. Were
/// morph to do the same, a build with one library's Tracy on and the other's
/// off would have one header's stubs redefine the other's real macros. Every
/// macro here is spelled `MORPH_*` and none of Tracy's names is defined.
///
/// @par One zone, one phase, one thread
/// A Tracy zone must begin and end on the same thread. A dispatch crosses
/// threads -- the caller, the model's strand, the callback executor -- so each
/// phase is its own zone on the thread that runs it, and the phases of one
/// call are linked by writing `session::Context::requestId` as zone text
/// (`MORPH_ZONE_TEXT`). No zone spans a hand-off, and none stays open across a
/// `co_await`: a suspended coroutine's thread goes on to run other work, whose
/// zones would then close out of order with the open one.
///
/// @par Every translation unit must agree
/// morph is header-only, so these macros change the bodies of inline
/// functions. A program in which one translation unit sees
/// `MORPH_TRACY_ENABLED` and another does not holds two different definitions
/// of the same inline function, which is an ODR violation: the linker keeps
/// one of them, silently. CMake consumers get agreement by construction,
/// because the definition is an `INTERFACE` property of `morph::morph`. A
/// build that does not use morph's CMake package must define
/// `MORPH_TRACY_ENABLED` for all of its translation units or for none, and
/// link exactly one `TracyClient`. MSVC and clang-cl turn a mismatch into a
/// link error (`detect_mismatch` below); other toolchains do not detect it.

// NOLINTBEGIN(cppcoreguidelines-macro-usage) — the macros are the API: a zone
// is a scoped local the call site's scope has to own, and a disabled argument
// must not be evaluated, neither of which a function can do.

#ifdef MORPH_TRACY_ENABLED

#include <string_view>
#include <tracy/Tracy.hpp>

namespace morph::profiler::detail {

/// Attaches @p text to @p zone, skipping an empty one so a zone without a
/// request id carries no blank annotation.
inline void zoneText(::tracy::ScopedZone& zone, std::string_view text) {
    if (!text.empty()) {
        zone.Text(text.data(), text.size());
    }
}

/// Sends @p text to the timeline as a message.
inline void message(std::string_view text) { TracyMessage(text.data(), text.size()); }

}  // namespace morph::profiler::detail

// Every zone is a local named `morphProfilerZone`, so MORPH_ZONE_TEXT finds
// the innermost one; a nested zone therefore shadows the outer one on
// purpose, and the warning is silenced around the declaration. Not Tracy's
// own `___tracy_scoped_zone`: spelled in this header, a name with a double
// underscore is a reserved identifier at every call site.
// Not Tracy's ZoneScopedN: whether that expansion ends in a ';' depends on
// the compiler, and the one the call site writes would then be an empty
// statement. The trailing static_assert takes the call site's ';' instead.
#if defined(__clang__)
#define MORPH_ZONE(name)                                                              \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wshadow\"") \
        ZoneNamedN(morphProfilerZone, name, true);                                    \
    _Pragma("clang diagnostic pop") static_assert(true)
#elif defined(__GNUC__)
#define MORPH_ZONE(name)                                                          \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"") \
        ZoneNamedN(morphProfilerZone, name, true);                                \
    _Pragma("GCC diagnostic pop") static_assert(true)
#elif defined(_MSC_VER)
#define MORPH_ZONE(name)                                                                                   \
    _Pragma("warning(push)") _Pragma("warning(disable : 4456)") ZoneNamedN(morphProfilerZone, name, true); \
    _Pragma("warning(pop)") static_assert(true)
#else
#define MORPH_ZONE(name) ZoneNamedN(morphProfilerZone, name, true)
#endif
#define MORPH_ZONE_TEXT(text) ::morph::profiler::detail::zoneText(morphProfilerZone, (text))
#define MORPH_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#define MORPH_THREAD_NAME(name) ::tracy::SetThreadName(name)
#define MORPH_MESSAGE(text) ::morph::profiler::detail::message((text))

#ifdef _MSC_VER
#pragma detect_mismatch("morph_tracy_enabled", "1")
#endif

#else

namespace morph::profiler::detail {

/// What a disabled macro builds from its arguments' types: naming an argument
/// in `decltype` uses it without evaluating it, and an empty object costs
/// nothing to discard.
template <typename... Args>
struct Unevaluated {};

}  // namespace morph::profiler::detail

/// @brief Opens a profiler zone named @p name that ends with the enclosing
///        scope. At most one per scope.
/// @param name A string literal: Tracy keeps a pointer to it for the life of
///        the process.
#define MORPH_ZONE(name) static_cast<void>(::morph::profiler::detail::Unevaluated<decltype(name)>{})

/// @brief Attaches @p text to the enclosing scope's `MORPH_ZONE`; an empty
///        text attaches nothing.
///
/// Only valid after a `MORPH_ZONE` in the same or an enclosing scope.
/// @param text Anything convertible to `std::string_view`; Tracy copies it.
///        Not evaluated in a build without Tracy.
#define MORPH_ZONE_TEXT(text) static_cast<void>(::morph::profiler::detail::Unevaluated<decltype(text)>{})

/// @brief Records @p value on the plot named @p name.
/// @param name  A string literal, the plot's identity in the capture.
/// @param value An arithmetic value, recorded as a `double`. Not evaluated in
///        a build without Tracy.
#define MORPH_PLOT(name, value) \
    static_cast<void>(::morph::profiler::detail::Unevaluated<decltype(name), decltype(value)>{})

/// @brief Names the calling thread in the capture.
/// @param name A NUL-terminated string; Tracy copies it.
#define MORPH_THREAD_NAME(name) static_cast<void>(::morph::profiler::detail::Unevaluated<decltype(name)>{})

/// @brief Sends @p text to the capture's timeline as a message.
/// @param text Anything convertible to `std::string_view`; Tracy copies it.
///        Not evaluated in a build without Tracy.
#define MORPH_MESSAGE(text) static_cast<void>(::morph::profiler::detail::Unevaluated<decltype(text)>{})

#ifdef _MSC_VER
#pragma detect_mismatch("morph_tracy_enabled", "0")
#endif

#endif

// NOLINTEND(cppcoreguidelines-macro-usage)
