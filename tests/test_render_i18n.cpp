// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/render/i18n.hpp>
#include <optional>
#include <string>
#include <string_view>

using morph::render::resolveText;
using morph::render::TranslationProvider;

TEST_CASE("render::resolveText with no provider falls back to the schema literal", "[render][i18n]") {
    TranslationProvider const noProvider{};
    CHECK(resolveText(noProvider, "fr-FR", std::nullopt, "Action.field.label", "Field") == "Field");
}

TEST_CASE("render::resolveText prefers the explicit key over the derived key", "[render][i18n]") {
    TranslationProvider const provider = [](std::string_view key,
                                            std::string_view locale) -> std::optional<std::string> {
        if (locale == "fr-FR" && key == "custom.stem.label") {
            return std::string{"Étiquette"};
        }
        if (locale == "fr-FR" && key == "Action.field.label") {
            return std::string{"Wrong"};
        }
        return std::nullopt;
    };
    CHECK(resolveText(provider, "fr-FR", std::optional<std::string>{"custom.stem.label"}, "Action.field.label",
                      "Field") == "Étiquette");
}

TEST_CASE("render::resolveText falls through an explicit-key miss to the derived key", "[render][i18n]") {
    TranslationProvider const provider = [](std::string_view key,
                                            std::string_view locale) -> std::optional<std::string> {
        if (locale == "fr-FR" && key == "Action.field.label") {
            return std::string{"Champ"};
        }
        return std::nullopt;
    };
    CHECK(resolveText(provider, "fr-FR", std::optional<std::string>{"custom.stem.label"}, "Action.field.label",
                      "Field") == "Champ");
}

TEST_CASE("render::resolveText falls back to the schema literal on a full miss", "[render][i18n]") {
    TranslationProvider const provider = [](std::string_view, std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };
    CHECK(resolveText(provider, "de-DE", std::nullopt, "Action.field.label", "Field") == "Field");
}
