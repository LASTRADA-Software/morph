// SPDX-License-Identifier: Apache-2.0
//
// Tests for morph::ladder::gui::errorText (morph#168).
//
// The extraction replaced 13 hand-written copies across six rungs. Most of
// them lacked a `catch (...)` arm, so this is not a pure refactor -- the
// behaviour for a non-std::exception throw changes from "escapes the callback"
// to "returns text". These pin both the preserved behaviour and the fixed one.

#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <stdexcept>
#include <string>

#include "gui/error_text.hpp"

using morph::ladder::gui::errorText;

namespace {

/// Wraps a throw in an exception_ptr the way a Completion's error path does.
template <typename F>
std::exception_ptr captured(F&& thrower) {
    try {
        thrower();
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

}  // namespace

TEST_CASE("errorText renders a std::exception as its what() text", "[error-text]") {
    const auto err = captured([] { throw std::runtime_error{"the ledger is closed"}; });
    REQUIRE(errorText(err) == QStringLiteral("the ledger is closed"));
}

TEST_CASE("errorText decodes what() as UTF-8", "[error-text]") {
    // The call sites this replaced were split between QString::fromStdString
    // and QString::fromUtf8. In Qt 6 fromStdString forwards to fromUtf8, so the
    // two spellings agreed -- but the ladder relied on that without saying so.
    // This pins it, so the extraction is a no-op for text rather than a
    // re-encoding of every error message that is not plain ASCII.
    const std::string text = "µg/L below détection limit";
    const auto err = captured([&] { throw std::runtime_error{text}; });

    REQUIRE(errorText(err) == QString::fromUtf8(text));
    REQUIRE(errorText(err) == QString::fromStdString(text));
    REQUIRE(errorText(err).toStdString() == text);
}

TEST_CASE("errorText returns text for a throw that is not a std::exception", "[error-text]") {
    // The arm most hand-written copies omitted. Before the extraction this
    // exception escaped the error callback; a Completion callback is not a
    // context that can absorb that.
    REQUIRE(errorText(captured([] { throw 42; })) == QStringLiteral("unknown error"));
    REQUIRE(errorText(captured([] { throw std::string{"bare"}; })) == QStringLiteral("unknown error"));
}

TEST_CASE("errorText handles a null exception pointer", "[error-text]") {
    // std::rethrow_exception on a null pointer is undefined behaviour, so this
    // is checked before the rethrow rather than caught after it.
    REQUIRE(errorText(std::exception_ptr{}) == QStringLiteral("unknown error"));
}

TEST_CASE("errorText preserves an empty what() rather than substituting text", "[error-text]") {
    // An empty message is still a std::exception, so it must not be reported as
    // "unknown error" -- that would misattribute a thrown-and-described failure
    // to an undescribed one.
    struct Silent : std::exception {
        [[nodiscard]] const char* what() const noexcept override { return ""; }
    };
    REQUIRE(errorText(captured([] { throw Silent{}; })).isEmpty());
}
