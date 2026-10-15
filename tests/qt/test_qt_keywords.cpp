// SPDX-License-Identifier: Apache-2.0
//
// A Qt server writes its screens with the builders, so the builders compile after Qt's headers: Qt defines `emit`,
// `signals`, `slots` and `foreach` as macros unless QT_NO_KEYWORDS is set, and a public name spelled like one of them
// stops compiling here. The include order below is the point of this file.

#include <QObject>
#include <catch2/catch_test_macros.hpp>
#include <morph/forms/forms.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>

namespace ui = morph::ui::author;

TEST_CASE("The builders and the UI service compile after Qt's keyword macros", "[qt][keywords]") {
    auto const written = ui::screen("keywords.home", [](ui::Scope&) { return ui::spacer(); }).write();
    REQUIRE(written.has_value());
    CHECK(written->text.contains(R"("id":"keywords.home")"));
}
