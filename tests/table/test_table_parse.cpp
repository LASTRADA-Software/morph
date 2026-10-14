// SPDX-License-Identifier: Apache-2.0
//
// The table's number parsers: a whole integer, a whole real, and the classic-locale real parser a standard library
// without floating-point `from_chars` falls back on. That fallback reads the wire's `.` whatever locale the process
// runs in, so the test installs a global locale whose decimal point is `,`.

#include <catch2/catch_test_macros.hpp>
#include <locale>
#include <morph/table/sort.hpp>
#include <optional>
#include <string>

namespace {

/// A numeric punctuation whose decimal point is a comma.
class CommaDecimal final : public std::numpunct<char> {
protected:
    [[nodiscard]] char do_decimal_point() const override { return ','; }
};

/// Installs a global locale with a comma decimal point for its lifetime.
class CommaLocale {
public:
    CommaLocale() : _previous{std::locale::global(std::locale{std::locale::classic(), new CommaDecimal})} {}
    CommaLocale(CommaLocale const&) = delete;
    CommaLocale& operator=(CommaLocale const&) = delete;
    CommaLocale(CommaLocale&&) = delete;
    CommaLocale& operator=(CommaLocale&&) = delete;
    ~CommaLocale() { std::locale::global(_previous); }

private:
    std::locale _previous;
};

}  // namespace

using morph::table::detail::parseInteger;
using morph::table::detail::parseReal;
using morph::table::detail::parseRealClassic;

TEST_CASE("table parse: a whole integer, and nothing else", "[table][parse]") {
    CHECK(parseInteger("42") == 42);
    CHECK(parseInteger("-7") == -7);
    CHECK_FALSE(parseInteger("").has_value());
    CHECK_FALSE(parseInteger("4 2").has_value());
    CHECK_FALSE(parseInteger("1.5").has_value());
}

// Mutation: parse in the global locale (the comma locale reads "1.5" as 1).
TEST_CASE("table parse: a real is read with a point, in either parser, whatever the global locale", "[table][parse]") {
    CommaLocale const comma;
    for (auto const parse : {&parseReal, &parseRealClassic}) {
        CHECK(parse("1.5") == 1.5);
        CHECK(parse("-0.25") == -0.25);
        CHECK(parse("3") == 3.0);
        CHECK(parse("1e3") == 1000.0);
        CHECK_FALSE(parse("").has_value());
        CHECK_FALSE(parse("1,5").has_value());
        CHECK_FALSE(parse(" 1.5").has_value());
        CHECK_FALSE(parse("1.5 ").has_value());
        CHECK_FALSE(parse("+1.5").has_value());
        CHECK_FALSE(parse("abc").has_value());
        CHECK_FALSE(parse("nan").has_value());
    }
}
