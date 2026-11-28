// SPDX-License-Identifier: Apache-2.0
//
// The document loader over every one-node corruption of the valid corpus: a node replaced by a value of another
// type, or a member dropped. Each result is a document or a refusal that names a JSON Pointer and a reason, the same
// refusal every time.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ui_corpus.hpp"

using morph::ui::ActionInfo;
using morph::ui::Json;
using morph::ui::loadDocument;
using morph::ui::LoadOptions;
using morph::ui::ModelInfo;
using morph::ui::testing::member;
using morph::ui::testing::optionalMember;

namespace {

Json const& corpus() {
    static Json const loaded = morph::ui::testing::loadCorpus(MORPH_UI_DOCUMENTS_CORPUS);
    return loaded;
}

std::vector<std::string> namesIn(Json const* json) {
    std::vector<std::string> out;
    if (json != nullptr) {
        for (Json const& entry : std::get<Json::array_t>(json->data)) {
            out.push_back(std::get<std::string>(entry.data));
        }
    }
    return out;
}

LoadOptions catalogOptions() {
    Json const& catalog = member(corpus(), "catalog");
    Json const& models = member(catalog, "models");
    Json const& actions = member(catalog, "actions");
    LoadOptions options;
    options.catalog.model = [&models](std::string_view type) -> std::optional<ModelInfo> {
        Json const* const entry = optionalMember(models, type);
        if (entry == nullptr) {
            return std::nullopt;
        }
        ModelInfo info;
        info.keyed = optionalMember(*entry, "keyed") != nullptr;
        info.stateless = optionalMember(*entry, "stateless") != nullptr;
        return info;
    };
    options.catalog.action = [&actions](std::string_view action) -> std::optional<ActionInfo> {
        Json const* const entry = optionalMember(actions, action);
        if (entry == nullptr) {
            return std::nullopt;
        }
        ActionInfo info;
        info.modelType = std::get<std::string>(member(*entry, "model").data);
        info.calculatedFields = namesIn(optionalMember(*entry, "calculated"));
        info.secretFields = namesIn(optionalMember(*entry, "secret"));
        info.credential = optionalMember(*entry, "credential") != nullptr;
        if (Json const* const lists = optionalMember(*entry, "lists")) {
            info.listFields = namesIn(lists);
        }
        return info;
    };
    return options;
}

using Path = std::vector<std::string>;

// NOLINTNEXTLINE(misc-no-recursion): a JSON tree nests, and the walk follows it.
void collect(Json const& node, Path& here, std::vector<Path>& out) {
    out.push_back(here);
    if (auto const* const object = std::get_if<Json::object_t>(&node.data)) {
        for (auto const& [key, value] : *object) {
            here.push_back(key);
            collect(value, here, out);
            here.pop_back();
        }
    } else if (auto const* const array = std::get_if<Json::array_t>(&node.data)) {
        for (std::size_t at = 0; at < array->size(); ++at) {
            here.push_back(std::to_string(at));
            collect(array->at(at), here, out);
            here.pop_back();
        }
    }
}

// The node at `path` in `root`, or null when a step is missing.
Json* nodeAt(Json& root, Path const& path, std::size_t steps) {
    Json* node = &root;
    for (std::size_t at = 0; at < steps; ++at) {
        if (auto* const object = std::get_if<Json::object_t>(&node->data)) {
            auto* const found = object->find(path.at(at));
            if (found == object->end()) {
                return nullptr;
            }
            node = &found->second;
        } else if (auto* const array = std::get_if<Json::array_t>(&node->data)) {
            node = &array->at(static_cast<std::size_t>(std::stoul(path.at(at))));
        } else {
            return nullptr;
        }
    }
    return node;
}

// `root` with the node at `path` replaced by `value`, or removed when `value` is null.
Json corrupted(Json root, Path const& path, Json const* value) {
    if (path.empty()) {
        return value != nullptr ? *value : root;
    }
    Json* const parent = nodeAt(root, path, path.size() - 1);
    if (parent == nullptr) {
        return root;
    }
    if (auto* const object = std::get_if<Json::object_t>(&parent->data)) {
        if (value != nullptr) {
            (*object)[path.back()] = *value;
        } else {
            object->erase(path.back());
        }
    } else if (auto* const array = std::get_if<Json::array_t>(&parent->data)) {
        auto const at = static_cast<std::size_t>(std::stoul(path.back()));
        if (value != nullptr) {
            array->at(at) = *value;
        } else {
            array->erase(array->begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
    return root;
}

}  // namespace

TEST_CASE("a corrupted document loads or is refused with a pointer, a reason and the same words twice",
          "[covc][ui][document]") {
    LoadOptions const options = catalogOptions();
    std::vector<Json> replacements;
    for (std::string_view const text : {"null", "-1", "2.5", "\"zz\"", "[]", "{}"}) {
        auto parsed = morph::ui::parseJson(text);
        REQUIRE(parsed.has_value());
        replacements.push_back(*std::move(parsed));
    }
    std::size_t loaded = 0;
    std::size_t refused = 0;
    for (Json const& entry : std::get<Json::array_t>(member(corpus(), "valid").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        CAPTURE(name);
        Json const& original = member(entry, "doc");
        std::vector<Path> paths;
        Path here;
        collect(original, here, paths);
        for (Path const& path : paths) {
            std::vector<Json const*> variants;
            variants.reserve(replacements.size() + 1);
            for (Json const& replacement : replacements) {
                variants.push_back(&replacement);
            }
            if (!path.empty()) {
                variants.push_back(nullptr);
            }
            for (Json const* const variant : variants) {
                Json const doc = corrupted(original, path, variant);
                auto const first = loadDocument(doc, options);
                if (first) {
                    ++loaded;
                    continue;
                }
                ++refused;
                auto const& refusal = first.error();
                INFO(refusal.message());
                REQUIRE((refusal.path.empty() || refusal.path.front() == '/'));
                REQUIRE_FALSE(refusal.reason.empty());
                auto const again = loadDocument(doc, options);
                REQUIRE_FALSE(again.has_value());
                REQUIRE(again.error() == refusal);
            }
        }
    }
    CHECK(refused > 5'000);
    CHECK(loaded > 500);
}
