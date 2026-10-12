// SPDX-License-Identifier: Apache-2.0

// The UI service's shared vocabulary: content hashes, the manifest and the
// ui-hello, ui-catalog and ui-fetch bodies, with every bound and refusal of
// their parsers.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/bundle.hpp>
#include <string>
#include <string_view>

using morph::ui::contentHash;
using morph::ui::isContentHash;
using morph::ui::Manifest;
using morph::ui::ManifestFile;

namespace {

/// A content hash for a file the test never serves.
std::string hashA() { return contentHash("a"); }

/// Another.
std::string hashB() { return contentHash("b"); }

/// Repeats @p piece @p count times.
std::string repeat(std::string_view piece, std::size_t count) {
    std::string out;
    out.reserve(piece.size() * count);
    for (std::size_t index = 0; index < count; ++index) {
        out += piece;
    }
    return out;
}

/// A JSON array of @p count copies of @p item.
std::string arrayOf(std::string_view item, std::size_t count) {
    std::string out = "[";
    for (std::size_t index = 0; index < count; ++index) {
        out += index == 0 ? "" : ",";
        out += item;
    }
    return out + "]";
}

/// The reason a refusal carries, or a marker when the value was accepted.
template <typename Expected>
std::string refusal(Expected const& result) {
    if (result) {
        return "<accepted>";
    }
    return result.error();
}

}  // namespace

TEST_CASE("contentHash is SHA-256 in lower-case hex", "[ui-service][bundle]") {
    REQUIRE(contentHash("abc") == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(contentHash("") == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(isContentHash(contentHash("abc")));
}

TEST_CASE("isContentHash accepts only sha256: and 64 lower-case hex digits", "[ui-service][bundle]") {
    std::string const good = contentHash("x");
    REQUIRE(isContentHash(good));
    REQUIRE_FALSE(isContentHash(good.substr(0, good.size() - 1)));
    REQUIRE_FALSE(isContentHash(good + "0"));
    REQUIRE_FALSE(isContentHash("sha512:" + good.substr(7)));
    std::string upper = good;
    upper.back() = 'A';
    REQUIRE_FALSE(isContentHash(upper));
    std::string other = good;
    other.back() = 'g';
    REQUIRE_FALSE(isContentHash(other));
    other.back() = '/';
    REQUIRE_FALSE(isContentHash(other));
    other.back() = ':';
    REQUIRE_FALSE(isContentHash(other));
}

TEST_CASE("a manifest round-trips, and public travels as `public`", "[ui-service][bundle]") {
    Manifest const manifest{
        .application = "lims",
        .version = "1.4.0",
        .files = {ManifestFile{.id = "app", .kind = "app", .hash = hashA(), .size = 1},
                  ManifestFile{.id = "login", .kind = "screen", .hash = hashB(), .size = 1, .isPublic = true}}};
    std::string const text = morph::ui::manifestText(manifest);
    REQUIRE(text.contains("\"public\":true"));
    REQUIRE(morph::ui::manifestText(manifest) == text);
    auto const parsed = morph::ui::parseManifest(text);
    REQUIRE(parsed);
    REQUIRE(parsed->application == "lims");
    REQUIRE(parsed->files == manifest.files);
    REQUIRE(parsed->find("screen", "login") != nullptr);
    REQUIRE(parsed->find("screen", "app") == nullptr);
    REQUIRE(parsed->find("app", "login") == nullptr);
}

TEST_CASE("parseManifest refuses what it cannot trust", "[ui-service][bundle]") {
    std::string const file = R"({"id":"a","kind":"screen","hash":")" + hashA() + R"(","size":1})";
    auto manifestWith = [](std::string const& files) { return R"({"application":"x","files":)" + files + "}"; };

    SECTION("an unknown optional key is ignored") {
        auto const parsed =
            morph::ui::parseManifest(R"({"application":"x","later":1,"files":[{"id":"a","kind":"screen","hash":")" +
                                     hashA() + R"(","size":1,"later":true}]})");
        REQUIRE(parsed);
        REQUIRE(parsed->files.size() == 1U);
    }
    SECTION("a must-understand key at the top") {
        REQUIRE(refusal(morph::ui::parseManifest(R"({"!signed":true,"files":[]})")) ==
                "the must-understand key `!signed` in the manifest is not understood");
    }
    SECTION("a must-understand key in a file") {
        std::string const marked = R"({"!encrypted":true,"id":"a","kind":"screen","hash":")" + hashA() + R"("})";
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith("[" + marked + "]"))) ==
                "the must-understand key `!encrypted` in the manifest files is not understood");
    }
    SECTION("text that is not JSON") {
        REQUIRE(refusal(morph::ui::parseManifest("{")).starts_with("the manifest: not JSON"));
    }
    SECTION("JSON nested deeper than the bound") {
        std::string const deep = repeat("[", 70) + repeat("]", 70);
        REQUIRE(refusal(morph::ui::parseManifest(R"({"files":[],"x":)" + deep + "}"))
                    .starts_with("the manifest: the JSON nests deeper than 64"));
    }
    SECTION("JSON that is not an object, or a file that is not one") {
        REQUIRE(refusal(morph::ui::parseManifest("[]")).starts_with("the manifest: "));
        REQUIRE(refusal(morph::ui::parseManifest(R"({"files":[1]})")).starts_with("the manifest: "));
    }
    SECTION("a value of the wrong type") {
        REQUIRE(refusal(morph::ui::parseManifest(R"({"files":"none"})")).starts_with("the manifest: "));
    }
    SECTION("too many files") {
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith(arrayOf(file, morph::ui::kMaxBundleFiles + 1)))) ==
                "the manifest lists more than 4096 files");
    }
    SECTION("names longer than the bound") {
        std::string const longName = repeat("n", morph::ui::kMaxNameBytes + 1);
        REQUIRE(refusal(morph::ui::parseManifest(R"({"application":")" + longName + R"(","files":[]})")) ==
                "the application id is longer than 256 bytes");
        REQUIRE(refusal(morph::ui::parseManifest(R"({"version":")" + longName + R"(","files":[]})")) ==
                "the application version is longer than 256 bytes");
        std::string const longId = R"({"id":")" + longName + R"(","kind":"screen","hash":")" + hashA() + R"("})";
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith("[" + longId + "]"))) ==
                "a file id is longer than 256 bytes");
        std::string const longKind = R"({"id":"a","kind":")" + longName + R"(","hash":")" + hashA() + R"("})";
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith("[" + longKind + "]"))) ==
                "a file kind is longer than 256 bytes");
    }
    SECTION("a malformed hash") {
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith(R"([{"id":"a","kind":"screen","hash":"md5:00"}])"))) ==
                "the manifest file `a` has a malformed hash");
    }
    SECTION("a file larger than the bound") {
        std::string const big = R"({"id":"a","kind":"screen","hash":")" + hashA() + R"(","size":4194305})";
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith("[" + big + "]"))) ==
                "the manifest file `a` is larger than 4194304 bytes");
    }
    SECTION("one file listed twice") {
        REQUIRE(refusal(morph::ui::parseManifest(manifestWith("[" + file + "," + file + "]"))) ==
                "the manifest lists the screen `a` twice");
    }
}

TEST_CASE("parseHelloRequest bounds what a client may send", "[ui-service][bundle]") {
    SECTION("an empty body is a client that speaks nothing") {
        auto const parsed = morph::ui::parseHelloRequest("");
        REQUIRE(parsed);
        REQUIRE(parsed->vocabularies.empty());
    }
    SECTION("a full request") {
        auto const parsed = morph::ui::parseHelloRequest(
            R"({"vocabularies":["ui/1","expr/1"],"qt":"6.8","components":true,"hostComponents":{"pdf":2},)"
            R"("hostCommands":{"open":1},"hostValues":{"os":1},"later":[]})");
        REQUIRE(parsed);
        REQUIRE(parsed->vocabularies.size() == 2U);
        REQUIRE(parsed->components);
        REQUIRE(parsed->hostComponents.at("pdf") == 2);
    }
    SECTION("a body larger than the bound") {
        std::string const big = R"({"qt":")" + repeat("x", morph::ui::kMaxRequestBytes) + R"("})";
        REQUIRE(refusal(morph::ui::parseHelloRequest(big)) == "the ui-hello request is larger than 65536 bytes");
    }
    SECTION("a must-understand key") {
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"!push":true})")) ==
                "the must-understand key `!push` in the ui-hello request is not understood");
    }
    SECTION("too many vocabularies") {
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"vocabularies":)" + arrayOf(R"("v")", 257) + "}")) ==
                "the vocabularies list more than 256 items");
    }
    SECTION("a vocabulary longer than the bound") {
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"vocabularies":[")" + repeat("v", 257) + R"("]})")) ==
                "a vocabulary is longer than 256 bytes");
    }
    SECTION("a Qt version longer than the bound") {
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"qt":")" + repeat("6", 257) + R"("})")) ==
                "the Qt version is longer than 256 bytes");
    }
    SECTION("too many host names of a kind") {
        std::string many = "{";
        for (int index = 0; index < 257; ++index) {
            many += (index == 0 ? "\"" : ",\"") + std::to_string(index) + "\":1";
        }
        many += "}";
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"hostValues":)" + many + "}")) ==
                "the host values list more than 256 items");
    }
    SECTION("a host component name longer than the bound") {
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"hostComponents":{")" + repeat("c", 257) + R"(":1}})")) ==
                "a host component is longer than 256 bytes");
    }
    SECTION("a host name longer than the bound") {
        REQUIRE(refusal(morph::ui::parseHelloRequest(R"({"hostCommands":{")" + repeat("c", 257) + R"(":1}})")) ==
                "a host command is longer than 256 bytes");
    }
}

TEST_CASE("parseHelloReply refuses a reply it cannot trust", "[ui-service][bundle]") {
    std::string const digest = contentHash("manifest");
    SECTION("a good reply, with an unknown key ignored") {
        auto const parsed =
            morph::ui::parseHelloReply(R"({"application":"lims","version":"1","vocabularies":["ui/1"],)"
                                       R"("digest":")" +
                                       digest + R"(","later":{}})");
        REQUIRE(parsed);
        REQUIRE(parsed->digest == digest);
    }
    SECTION("a must-understand key") {
        REQUIRE(refusal(morph::ui::parseHelloReply(R"({"!push":1,"digest":")" + digest + R"("})")) ==
                "the must-understand key `!push` in the ui-hello reply is not understood");
    }
    SECTION("a digest that is not a content hash") {
        REQUIRE(refusal(morph::ui::parseHelloReply(R"({"digest":"abc"})")) ==
                "the ui-hello reply's digest is not a content hash");
    }
    SECTION("too many vocabularies") {
        REQUIRE(refusal(morph::ui::parseHelloReply(R"({"vocabularies":)" + arrayOf(R"("v")", 257) + "}")) ==
                "the vocabularies list more than 256 items");
    }
    SECTION("names longer than the bound") {
        REQUIRE(refusal(morph::ui::parseHelloReply(R"({"application":")" + repeat("a", 257) + R"("})")) ==
                "the application id is longer than 256 bytes");
        REQUIRE(refusal(morph::ui::parseHelloReply(R"({"version":")" + repeat("1", 257) + R"("})")) ==
                "the application version is longer than 256 bytes");
    }
    SECTION("text that is not JSON") {
        REQUIRE(refusal(morph::ui::parseHelloReply("nope")).starts_with("the ui-hello reply: not JSON"));
    }
}

TEST_CASE("parseCatalogReply reads entries, permissions and facts", "[ui-service][bundle]") {
    SECTION("a reply with facts gives lookups") {
        auto const parsed = morph::ui::parseCatalogReply(
            R"({"entries":[{"id":"lims.sample","title":{"t":"x","fallback":"Sample"},"public":true,"reason":null}],)"
            R"("permitted":["Lims_Get"],"models":{"Lims":{"stateless":true,"keyed":false}},)"
            R"("actions":{"Lims_Get":{"modelType":"Lims","calculatedFields":["c"]}}})");
        REQUIRE(parsed);
        REQUIRE(parsed->entries.size() == 1U);
        REQUIRE(parsed->entries.front().isPublic);
        REQUIRE(parsed->entries.front().whenDenied == "hide");
        morph::ui::Catalog const facts = parsed->facts();
        REQUIRE(facts.model);
        REQUIRE(facts.model("Lims")->stateless);
        REQUIRE_FALSE(facts.model("Other"));
        REQUIRE(facts.action("Lims_Get")->calculatedFields.size() == 1U);
        REQUIRE_FALSE(facts.action("Other"));
    }
    SECTION("a reply without facts leaves the lookups empty") {
        auto const parsed = morph::ui::parseCatalogReply(R"({"entries":[],"permitted":[]})");
        REQUIRE(parsed);
        morph::ui::Catalog const facts = parsed->facts();
        REQUIRE_FALSE(facts.model);
        REQUIRE_FALSE(facts.action);
    }
    SECTION("a must-understand key in an entry") {
        REQUIRE(refusal(morph::ui::parseCatalogReply(R"({"entries":[{"id":"a","!licensed":true}]})")) ==
                "the must-understand key `!licensed` in the catalog entries is not understood");
    }
    SECTION("a must-understand key in the facts") {
        REQUIRE(refusal(morph::ui::parseCatalogReply(R"({"models":{"M":{"!sharded":true}}})")) ==
                "the must-understand key `!sharded` in the catalog models M is not understood");
    }
    SECTION("facts that are not a map are refused by the reader") {
        REQUIRE(refusal(morph::ui::parseCatalogReply(R"({"models":[1]})")).starts_with("the catalog: "));
    }
    SECTION("too many entries") {
        REQUIRE(refusal(morph::ui::parseCatalogReply(R"({"entries":)" + arrayOf(R"({"id":"a"})", 4097) + "}")) ==
                "the catalog lists more than 4096 entries");
    }
    SECTION("too many facts") {
        REQUIRE(refusal(morph::ui::parseCatalogReply(R"({"permitted":)" + arrayOf(R"("a")", 16385) + "}")) ==
                "the catalog carries more than 16384 facts of a kind");
    }
    SECTION("an entry id longer than the bound") {
        REQUIRE(refusal(morph::ui::parseCatalogReply(R"({"entries":[{"id":")" + repeat("i", 257) + R"("}]})")) ==
                "a catalog entry id is longer than 256 bytes");
    }
    SECTION("an unknown optional key is ignored, at every level") {
        auto const parsed =
            morph::ui::parseCatalogReply(R"({"entries":[{"id":"a","badge":3}],"later":1,"models":{"M":{"later":1}}})");
        REQUIRE(parsed);
        REQUIRE(parsed->models->size() == 1U);
    }
}

TEST_CASE("parseFetchRequest takes one well-formed hash", "[ui-service][bundle]") {
    auto const parsed = morph::ui::parseFetchRequest(R"({"hash":")" + hashA() + R"("})");
    REQUIRE(parsed);
    REQUIRE(parsed->hash == hashA());
    REQUIRE(refusal(morph::ui::parseFetchRequest(R"({"hash":"../etc/passwd"})")) ==
            "the ui-fetch request's hash is not a content hash");
    REQUIRE(refusal(morph::ui::parseFetchRequest(R"({"hash":")" + repeat("0", morph::ui::kMaxRequestBytes) +
                                                 R"("})")) == "the ui-fetch request is larger than 65536 bytes");
    REQUIRE(refusal(morph::ui::parseFetchRequest(R"({"!range":[0,1]})")) ==
            "the must-understand key `!range` in the ui-fetch request is not understood");
}
