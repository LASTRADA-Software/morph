// SPDX-License-Identifier: Apache-2.0
//
// `ui-texts`: the server hands a client the application's texts in its session's locale, from a source the host
// provides, and the client reads them before the bundle's own catalogues.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/forms/i18n.hpp>
#include <morph/render/i18n.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ui_service_support.hpp"

namespace {

using morph::ui::FieldTexts;
using morph::ui::UiService;
using morph::wire::Envelope;

// The help text a host has for one field, in German and in English.
FieldTexts germanTexts(std::string_view locale) {
    if (locale == "de") {
        return {{"Lims_Save.note.help", "Was der Analyst gesehen hat"}};
    }
    return {{"Lims_Save.note.help", "What the analyst saw"}};
}

std::shared_ptr<morph::ui::ServedBundle const> bundleWith(morph::ui::FieldTextSource source) {
    morph::ui::UiRegistry registry = uiservice::registry();
    registry.setFieldTexts(std::move(source));
    auto built = registry.build();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    return *built;
}

// A `ui-texts` request from @p principal whose session names @p locale.
Envelope textsRequest(std::string locale, std::string principal = "tech") {
    Envelope env;
    env.kind = std::string{morph::ui::kTextsKind};
    env.callId = 7;
    env.session.principal = std::move(principal);
    env.session.locale = std::move(locale);
    return env;
}

Envelope ask(UiService& service, morph::session::IAuthorizer const& authorizer, Envelope const& env) {
    return morph::wire::decode(service.handle(env, authorizer));
}

// The texts a source answers when it holds @p count entries of @p bytes each.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): how many, then how long, as the bounds read.
FieldTexts many(std::size_t count, std::size_t bytes) {
    FieldTexts out;
    for (std::size_t index = 0; index < count; ++index) {
        out.emplace("key." + std::to_string(index), std::string(bytes, 'x'));
    }
    return out;
}

}  // namespace

// Mutation: answer the texts in another locale than the session's, or leave out the locale.
TEST_CASE("ui-texts answers the host's texts in the session's locale", "[ui-texts]") {
    UiService service{bundleWith(germanTexts)};
    auto const authorizer = uiservice::authorizer();
    REQUIRE(service.serves("ui-texts"));

    Envelope const reply = ask(service, *authorizer, textsRequest("de"));
    REQUIRE(reply.kind == "ok");
    REQUIRE(reply.callId == 7U);
    auto const texts = morph::ui::parseTextsReply(reply.body);
    REQUIRE(texts.has_value());
    CHECK(texts->locale == "de");
    CHECK(texts->messages.at("Lims_Save.note.help") == "Was der Analyst gesehen hat");

    auto const english = morph::ui::parseTextsReply(ask(service, *authorizer, textsRequest("en-GB")).body);
    REQUIRE(english.has_value());
    CHECK(english->locale == "en-GB");
    CHECK(english->messages.at("Lims_Save.note.help") == "What the analyst saw");
}

// Mutation: serve an empty reply, or nothing at all, when no source is configured.
TEST_CASE("ui-texts is refused, by name, when the server has no text source", "[ui-texts]") {
    UiService service{uiservice::bundle()};
    auto const authorizer = uiservice::authorizer();
    Envelope const reply = ask(service, *authorizer, textsRequest("de"));
    REQUIRE(reply.kind == "err");
    CHECK(reply.message == morph::ui::kNoTextsReason);
}

// Mutation: hand the source a locale the check did not admit, or skip the gate.
TEST_CASE("ui-texts refuses a missing or malformed locale and a principal the gate refuses", "[ui-texts]") {
    std::size_t asked = 0;
    UiService service{bundleWith([&asked](std::string_view locale) {
        ++asked;
        return germanTexts(locale);
    })};
    auto const authorizer = uiservice::authorizer();

    Envelope const none = ask(service, *authorizer, textsRequest(""));
    REQUIRE(none.kind == "err");
    CHECK(none.message == "the session names no locale");
    std::string const tooLong(36, 'a');
    for (std::string_view const bad : {"d", "de_AT", "de-", "-de", "de--AT", "de-ABCDEFGHI", "1e", tooLong.c_str()}) {
        INFO(bad);
        Envelope const reply = ask(service, *authorizer, textsRequest(std::string{bad}));
        REQUIRE(reply.kind == "err");
        CHECK(reply.message == "the session's locale is not a locale tag of at most 35 bytes");
    }
    CHECK(asked == 0);

    authorizer->denied["tech"].insert({"ui", "texts"});
    Envelope const refused = ask(service, *authorizer, textsRequest("de"));
    REQUIRE(refused.kind == "err");
    CHECK(refused.message == "unauthorized");
    CHECK(asked == 0);
    CHECK(morph::ui::isUiServiceRead("ui", "texts"));
}

// Mutation: send what the source answered whatever its size, or let a throwing source fault the server.
TEST_CASE("ui-texts refuses an answer over its bounds and a source that throws", "[ui-texts]") {
    auto const authorizer = uiservice::authorizer();
    auto const refusalFor = [&authorizer](FieldTexts texts) {
        UiService service{bundleWith([held = std::move(texts)](std::string_view) { return held; })};
        Envelope const reply = ask(service, *authorizer, textsRequest("de"));
        REQUIRE(reply.kind == "err");
        return reply.message;
    };
    // 1100 texts of 1000 bytes are over the 1 MiB reply bound, though each is small.
    CHECK(refusalFor(many(1100, 1000)) == "the texts for `de` are larger than 1048576 bytes");
    CHECK(refusalFor(many(morph::ui::kMaxTextEntries + 1, 1)) == "the texts for `de` are more than 16384");
    CHECK(refusalFor({{"", "empty key"}}) == "a text's key for `de` is not 1 to 256 bytes");
    CHECK(refusalFor({{std::string(257, 'k'), "long key"}}) == "a text's key for `de` is not 1 to 256 bytes");

    UiService throwing{bundleWith([](std::string_view) -> FieldTexts { throw std::runtime_error{"database down"}; })};
    Envelope const failed = ask(throwing, *authorizer, textsRequest("de"));
    REQUIRE(failed.kind == "err");
    CHECK(failed.message == "the texts for `de` could not be read");

    // Just inside the bounds is served.
    UiService full{bundleWith([](std::string_view) { return many(morph::ui::kMaxTextEntries, 1); })};
    CHECK(ask(full, *authorizer, textsRequest("de")).kind == "ok");
}

// Mutation: accept a tag with an empty, overlong or non-alphanumeric subtag, or a first subtag with a digit.
TEST_CASE("isLocaleTag takes the shape of BCP 47 and nothing else", "[ui-texts]") {
    using morph::ui::isLocaleTag;
    for (std::string_view const good : {"de", "de-AT", "zh-Hant-TW", "sl-rozaj-biske", "en-US-x-twain", "ABCDEFGH"}) {
        INFO(good);
        CHECK(isLocaleTag(good));
    }
    for (std::string_view const bad :
         {"", "d", "abcdefghi", "d3", "de-", "de--AT", "de-abcdefghi", "de-A_T", "de AT"}) {
        INFO(bad);
        CHECK_FALSE(isLocaleTag(bad));
    }
    CHECK(isLocaleTag("de-" + std::string(8, 'a') + "-" + std::string(8, 'b') + "-" + std::string(8, 'c') + "-abcde"));
    CHECK_FALSE(
        isLocaleTag("de-" + std::string(8, 'a') + "-" + std::string(8, 'b') + "-" + std::string(8, 'c') + "-abcdef"));
}

// Mutation: accept a reply over a bound, with a bad locale or key, or with a must-understand key.
TEST_CASE("parseTextsReply refuses a reply it must not trust", "[ui-texts]") {
    using morph::ui::parseTextsReply;
    auto const reason = [](std::string_view text) {
        auto const parsed = parseTextsReply(text);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };
    auto const good = parseTextsReply(R"({"locale":"de","messages":{"a.b.help":"Hilfe"},"later":1})");
    REQUIRE(good.has_value());
    CHECK(good->messages.at("a.b.help") == "Hilfe");

    CHECK(reason(std::string(morph::ui::kMaxTextsBytes + 1, ' ')) ==
          "the ui-texts reply is larger than 1048576 bytes");
    CHECK(reason(R"({"locale":"de","!signed":true})") ==
          "the must-understand key `!signed` in the ui-texts reply is not understood");
    CHECK(reason(R"({"locale":"d_e","messages":{}})") == "the ui-texts reply's locale is not a locale tag");
    CHECK(reason(R"({"locale":"de","messages":{"":"x"}})") == "a ui-texts key is 1 to 256 bytes");
    CHECK(reason(R"({"locale":"de","messages":{")" + std::string(257, 'k') + R"(":"x"}})") ==
          "a ui-texts key is 1 to 256 bytes");
    std::string tooMany = R"({"locale":"de","messages":{)";
    for (std::size_t index = 0; index <= morph::ui::kMaxTextEntries; ++index) {
        tooMany += (index == 0 ? R"("k)" : R"(,"k)") + std::to_string(index) + R"(":"")";
    }
    CHECK(reason(tooMany + "}}") == "the ui-texts reply carries more than 16384 texts");
    CHECK(reason("not json").starts_with("the ui-texts reply: "));
}

// Mutation: read the bundle's catalogue before the server's texts, or drop the schema literal as the last fallback.
TEST_CASE("a client reads the server's texts, then the bundle's catalogue, then the schema's text", "[ui-texts]") {
    morph::ui::MessageCatalogues server;
    server.add("de", morph::ui::Catalogue{.messages = {{"Lims_Save.note.help", "Vom Server"}}, .dropped = {}});
    morph::ui::MessageCatalogues bundled;
    bundled.add("de", morph::ui::Catalogue{.messages = {{"Lims_Save.note.help", "Aus dem Bundle"},
                                                        {"Lims_Save.code.help", "Nur im Bundle"}},
                                           .dropped = {}});
    morph::render::TranslationProvider const provider = morph::ui::layeredProvider(server, bundled);
    namespace i18n = morph::forms::i18n;
    auto const help = [&provider](std::string_view field, std::string_view locale) {
        return morph::render::resolveText(provider, locale, std::nullopt,
                                          i18n::fieldKey("Lims_Save", field, i18n::FieldSlot::Help),
                                          "From the schema");
    };
    CHECK(help("note", "de") == "Vom Server");
    // A region reads its language's texts, the server's first.
    CHECK(help("note", "de-AT") == "Vom Server");
    CHECK(help("code", "de") == "Nur im Bundle");
    CHECK(help("other", "de") == "From the schema");
    CHECK(help("note", "fr") == "From the schema");
    // With no server texts at all, the bundle's catalogue is what it was before.
    CHECK(morph::ui::layeredProvider(morph::ui::MessageCatalogues{}, bundled)("Lims_Save.note.help", "de") ==
          std::optional<std::string>{"Aus dem Bundle"});
}
