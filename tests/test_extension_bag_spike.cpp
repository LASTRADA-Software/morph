// SPDX-License-Identifier: Apache-2.0
//
// Extension-bag spike (examples/LADDER.md, no-app spike 4; examples/crm/README.md
// steps 9-10, "the endgame"). Feasibility probe, not application code — this file
// answers one question and its findings are recorded in
// examples/crm/EXTENSION-BAG-SPIKE.md; nothing here ships as part of any rung.
//
// The question: can a morph model carry an open extension bag (fields added at
// *runtime*, past the compiled C++ action struct) that appears in served schema,
// form rendering, validation, and the journal like a first-class field?
//
// One throwaway model, EB_ContactModel, with two fixed compiled fields (name,
// email) plus a runtime-registered custom field ("favoriteColor", added by an
// AddCustomField-style action) exercised end to end:
//   1. schema  — does schemaJson<A>() (or a decorated variant of it) grow a
//                property node for the new field for later callers?
//   2. forms   — does the shipped QML renderer need any change to show it?
//                (checked structurally here — QML itself isn't exercised by a
//                Catch2 unit test; see the finding doc for how this was verified.)
//   3. decode  — does the field's *value* survive dispatch decode, or does the
//                framework's lenient "unknown key" reader silently drop it?
//   4. journal — does a recorded entry replay the custom field's value back?
//
// Result headline (full writeup: examples/crm/EXTENSION-BAG-SPIKE.md): the
// journal and the QML renderer need no framework change at all — the journal is
// payload-shape-agnostic, and DynamicForm.qml already renders any schema property
// generically. Schema growth and decode preservation are both reachable using
// primitives the framework already ships but morph itself never plumbs together:
// glz::generic_u64 (already used internally for schema-DOM manipulation, see
// forms.hpp's mergeSchemaExtras/InstanceConstraints::decorate) as the bag's Value
// type, and glaze's own glz::meta<T>::unknown_read/unknown_write hook (used
// nowhere else in morph) to route an unrecognised wire key into a map member
// instead of dropping it. No new morph-owned Value type or dispatch change was
// needed for this probe to work.
//
// Model/action types need external (file-scope) linkage, not an anonymous
// namespace — glaze's reflection mangles a name for the type and needs it to
// be visible outside this TU's anonymous namespace (see
// examples/concepts/journal_and_outbox.cpp's file-scope comment for the same
// rule). "EB_" is this file's unique type-id prefix so it can never collide
// with another example file or another tests/test_*.cpp's own registrations.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using morph::journal::LogEntry;

// ── The extension-bag Value type ─────────────────────────────────────────────
//
// Deliberately narrow for this probe: string | double | bool. A unit-bearing
// Quantity-in-bag and a Choice-in-bag are stretch goals explicitly deferred to
// any real 7b build (crm/README.md's DoD doesn't require them of the spike).
// glz::generic_u64 (glaze's own JSON-DOM variant, already a morph dependency and
// already used the same way inside forms.hpp for schema manipulation) is reused
// directly rather than inventing a parallel morph::forms::Value — see the finding
// doc for why that reuse is judged fine for a spike but not necessarily for a
// real 7b.
using EB_Value = glz::generic_u64;

// ── The runtime custom-field registry ────────────────────────────────────────
//
// Stands in for a real "AddCustomField persists to a metadata table" — a single
// process-wide registry is enough to answer the schema/decode/journal question
// without building actual persistence. Thread-safe because morph_tests runs
// Catch2 test cases that may share global state across the binary.
class EB_CustomFieldRegistry {
public:
    struct Field {
        std::string name;
        std::string type;  // "string" | "number" | "bool" — mirrors AddCustomField's `type`.
        bool required = false;
    };

    void add(Field field) {
        std::lock_guard lock{_mutex};
        for (auto& existing : _fields) {
            if (existing.name == field.name) {
                existing = std::move(field);
                return;
            }
        }
        _fields.push_back(std::move(field));
    }

    void clear() {
        std::lock_guard lock{_mutex};
        _fields.clear();
    }

    [[nodiscard]] std::vector<Field> fields() const {
        std::lock_guard lock{_mutex};
        return _fields;
    }

private:
    mutable std::mutex _mutex;
    std::vector<Field> _fields;
};

inline EB_CustomFieldRegistry& ebRegistry() {
    static EB_CustomFieldRegistry registry;
    return registry;
}

// ── The action under test ────────────────────────────────────────────────────
//
// `extra` is NOT a normal reflected member from glaze's point of view: it is
// reached only through the unknown_read/unknown_write hook declared in
// glz::meta<EB_UpdateContact> below, so morph's own reflected-member walk
// (forms::detail::forEachNamedMember, which drives schemaJson<A>()) never sees
// it as a member — exactly the "hidden sink" shape this probe needs so the two
// compiled fields and the open bag don't collide in the emitted schema.
struct EB_UpdateContact {
    std::string name;
    std::string email;

    std::map<std::string, EB_Value> extra{};

    [[nodiscard]] bool validate() const noexcept {
        if (name.empty() || email.empty()) {
            return false;
        }
        // Custom-field required-ness, checked by hand against the live
        // registry — schemaJson<A>()'s own `required` array is a function of
        // the compiled type (round-5 ground truth, examples/crm/README.md) and
        // cannot express this, so a model that opts into an extension bag is
        // responsible for its own required check. See the finding doc.
        for (auto const& field : ebRegistry().fields()) {
            if (field.required && !extra.contains(field.name)) {
                return false;
            }
        }
        return true;
    }
};

// glz::meta must be declared in the same namespace glaze looks it up from, at
// namespace scope. This is the one declaration that answers the "does decode
// drop the key" half of the probe: without it, glz::read's lenient
// error_on_unknown_keys=false path silently discards any JSON key that isn't
// `name` or `email` (registry.hpp's BRIDGE_REGISTER_ACTION-generated fromJson
// uses exactly that lenient mode). With it, an unrecognised key is routed into
// `extra` instead of discarded — and the identical declaration makes toJson
// (glz::write) re-emit `extra`'s contents merged into the object's top level on
// the way out, so the wire shape is flat: {"name":...,"email":...,"favoriteColor":...},
// not a nested "extra" object.
//
// The explicit `value = object(...)` line is NOT optional sugar: glaze's own
// test suite (json_test.cpp's unknown_fields_member/unknown_fields_2/
// unknown_write) never declares unknown_read/unknown_write without it, and
// omitting it fails write.hpp's static_assert("unknown_write type not
// handled") — the unknown-field hooks are only wired up for the
// glz::meta::value-declared object path, not morph's usual pure-reflection
// (glz::reflectable<T>, no glz::meta at all) path every other action in this
// codebase relies on. This is itself a finding: an action opting into an
// extension bag gives up pure reflection and must hand-list its own compiled
// members here — see the finding doc for what that costs a real 7b (every
// BRIDGE_REGISTER_ACTION-registered action would need this if the framework
// grew this into a real feature, likely via a new opt-in macro rather than
// asking every author to hand-write glz::object()).
template <>
struct glz::meta<EB_UpdateContact> {
    using T = EB_UpdateContact;
    static constexpr auto value = glz::object("name", &T::name, "email", &T::email);
    static constexpr auto unknown_read = &T::extra;
    static constexpr auto unknown_write = &T::extra;
};

// A plain reflectable action with no unknown_read hook — the framework
// default this spike routes around.
struct EB_PlainNoBag {
    std::string name;
};

struct EB_ContactModel {
    std::string name;
    std::string email;
    std::map<std::string, EB_Value> extra{};

    std::string execute(const EB_UpdateContact& action) {
        name = action.name;
        email = action.email;
        extra = action.extra;
        return name;
    }
};

BRIDGE_REGISTER_MODEL(EB_ContactModel, "EB_ContactModel")
BRIDGE_REGISTER_ACTION(EB_ContactModel, EB_UpdateContact, "EB_UpdateContact")

// ── Schema injection: the per-instance node the framework has no seam for ───
//
// Mirrors morph::forms::InstanceConstraints::decorate()'s idiom exactly
// (instance_constraints.hpp): read the compiled schemaJson<A>() into a
// glz::generic_u64 DOM, mutate `properties`, write back out. The difference
// from InstanceConstraints — and the gap the round-5 ground truth in
// crm/README.md names — is that decorate() only overwrites *values* on
// *existing* property keys, while this adds a whole new property node the
// compiled type never declared. Not memoised, for the same reason
// instanceSchemaJson<A>() isn't: the result varies with the registry's current
// contents, so a caller that wants caching does its own.
[[nodiscard]] inline std::string ebSchemaJsonWithCustomFields() {
    std::string base = morph::forms::schemaJson<EB_UpdateContact>();

    auto fields = ebRegistry().fields();
    if (fields.empty()) {
        return base;
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
    glz::generic_u64 dom{};
    if (glz::read_json(dom, base)) {
        return base;  // malformed input passes through unchanged, same rule mergeSchemaExtras follows
    }

    glz::generic_u64::array_t requiredNames{};
    if (dom.contains("required") && dom["required"].holds<glz::generic_u64::array_t>()) {
        requiredNames = dom["required"].get<glz::generic_u64::array_t>();
    }

    std::uint64_t nextOrder = 0;
    if (dom.contains("properties") && dom["properties"].holds<glz::generic_u64::object_t>()) {
        // No cast: `size()` returns `std::size_t`, which is the same type as
        // `std::uint64_t` on the 64-bit targets this file is built for, so an
        // explicit cast is a no-op GCC rejects under -Wuseless-cast. Were
        // `std::size_t` ever narrower, the assignment widens implicitly and
        // safely -- unsigned to wider unsigned preserves the value.
        nextOrder = dom["properties"].get<glz::generic_u64::object_t>().size();
    }

    for (auto const& field : fields) {
        auto& property = dom["properties"][field.name];
        property["type"] = (field.type == "number") ? std::string{"number"}
                           : (field.type == "bool") ? std::string{"boolean"}
                                                    : std::string{"string"};
        property["x-order"] = nextOrder++;
        property["x-custom"] = true;  // marks this property as runtime-added, for a renderer or reviewer to see
        if (field.required) {
            requiredNames.emplace_back(field.name);
        }
    }
    dom["required"] = requiredNames;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    return glz::write_json(dom).value_or(base);
}

// ── 1. Schema: does the registry's field show up as a served property? ──────

TEST_CASE("Extension-bag spike: unregistered custom field is absent from schema", "[spike][extension-bag]") {
    ebRegistry().clear();
    const std::string schema = ebSchemaJsonWithCustomFields();
    CHECK(schema.find("favoriteColor") == std::string::npos);
    // The two compiled fields are there unconditionally, from schemaJson<A>() itself.
    CHECK(schema.find("\"name\"") != std::string::npos);
    CHECK(schema.find("\"email\"") != std::string::npos);
}

TEST_CASE("Extension-bag spike: AddCustomField-style registration grows the served schema", "[spike][extension-bag]") {
    ebRegistry().clear();
    ebRegistry().add({.name = "favoriteColor", .type = "string", .required = false});

    const std::string schema = ebSchemaJsonWithCustomFields();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    REQUIRE(dom.contains("properties"));
    auto& properties = dom["properties"];
    REQUIRE(properties.contains("favoriteColor"));
    CHECK(properties["favoriteColor"]["type"].get<std::string>() == "string");
    CHECK(properties["favoriteColor"]["x-custom"].get<bool>() == true);

    // The compiled fields' own schema is untouched — this is additive, not a
    // rewrite (same guarantee InstanceConstraints::decorate() gives for value
    // overrides; here it holds for whole new nodes too).
    REQUIRE(properties.contains("name"));
    REQUIRE(properties.contains("email"));

    ebRegistry().clear();
}

TEST_CASE("Extension-bag spike: a required custom field is added to the served `required` array",
          "[spike][extension-bag]") {
    ebRegistry().clear();
    ebRegistry().add({.name = "leadSource", .type = "string", .required = true});

    const std::string schema = ebSchemaJsonWithCustomFields();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    REQUIRE(dom.contains("required"));
    auto const& required = dom["required"].get<glz::generic_u64::array_t>();
    bool found = false;
    for (auto const& entry : required) {
        if (entry.get<std::string>() == "leadSource") {
            found = true;
        }
    }
    CHECK(found);

    ebRegistry().clear();
}

// ── 2. Decode: does the field's value survive dispatch, or get silently dropped? ──

TEST_CASE("Extension-bag spike: without the unknown_read hook, an extra wire key would be silently dropped",
          "[spike][extension-bag]") {
    // This test documents the framework default this spike routes around,
    // rather than exercising EB_UpdateContact (which already opts in via
    // glz::meta above). EB_PlainNoBag has no unknown_read hook; decoded with
    // the same lenient options BRIDGE_REGISTER_ACTION's fromJson uses
    // (error_on_unknown_keys = false), it drops any key it doesn't recognise —
    // this is the concrete mechanism behind crm/README.md's "Compiled C++
    // action structs cannot grow members" line.
    static constexpr glz::opts kLenientRead{.error_on_unknown_keys = false};

    EB_PlainNoBag action{};
    std::string_view payload = R"({"name":"Ada","favoriteColor":"teal"})";
    auto errCode = glz::read<kLenientRead>(action, payload);
    REQUIRE_FALSE(errCode);
    CHECK(action.name == "Ada");
    // "favoriteColor" is gone; no member captured it and no error was raised.
}

TEST_CASE("Extension-bag spike: with the unknown_read hook, the custom field's value survives decode",
          "[spike][extension-bag]") {
    ebRegistry().clear();
    ebRegistry().add({.name = "favoriteColor", .type = "string", .required = false});

    // Exactly the codec BRIDGE_REGISTER_ACTION generates for EB_UpdateContact
    // (registry.hpp's fromJson): lenient glz::read against the wire body.
    auto action = morph::model::ActionTraits<EB_UpdateContact>::fromJson(
        R"({"name":"Ada Lovelace","email":"ada@example.test","favoriteColor":"teal"})");

    CHECK(action.name == "Ada Lovelace");
    CHECK(action.email == "ada@example.test");
    REQUIRE(action.extra.contains("favoriteColor"));
    CHECK(action.extra.at("favoriteColor").get<std::string>() == "teal");

    // And the model's own validate() (ActionValidator<A>::ready) sees the
    // decoded bag, not just the compiled members — required-custom-field
    // enforcement is reachable from ordinary dispatch.
    CHECK(morph::model::ActionValidator<EB_UpdateContact>::ready(action));

    ebRegistry().clear();
}

TEST_CASE("Extension-bag spike: a missing required custom field fails validate() through ordinary dispatch",
          "[spike][extension-bag]") {
    ebRegistry().clear();
    ebRegistry().add({.name = "leadSource", .type = "string", .required = true});

    auto action = morph::model::ActionTraits<EB_UpdateContact>::fromJson(
        R"({"name":"Ada Lovelace","email":"ada@example.test"})");
    CHECK_FALSE(morph::model::ActionValidator<EB_UpdateContact>::ready(action));

    ebRegistry().clear();
}

TEST_CASE("Extension-bag spike: round trip through toJson preserves the custom field on the wire",
          "[spike][extension-bag]") {
    ebRegistry().clear();
    ebRegistry().add({.name = "favoriteColor", .type = "string", .required = false});

    EB_UpdateContact action{.name = "Ada", .email = "ada@example.test", .extra = {}};
    action.extra["favoriteColor"] = std::string{"teal"};

    const std::string wire = morph::model::ActionTraits<EB_UpdateContact>::toJson(action);
    // unknown_write merges `extra`'s entries at the object's top level (glaze's
    // glz::merge behaviour, write.hpp) — flat wire shape, not a nested object.
    CHECK(wire.find("\"favoriteColor\":\"teal\"") != std::string::npos);
    CHECK(wire.find("\"extra\"") == std::string::npos);

    auto decoded = morph::model::ActionTraits<EB_UpdateContact>::fromJson(wire);
    REQUIRE(decoded.extra.contains("favoriteColor"));
    CHECK(decoded.extra.at("favoriteColor").get<std::string>() == "teal");

    ebRegistry().clear();
}

// ── 3. Journal: does a replayed entry reconstruct the custom field's value? ──

TEST_CASE("Extension-bag spike: journal replay reconstructs a custom field's recorded value",
          "[spike][extension-bag]") {
    ebRegistry().clear();
    ebRegistry().add({.name = "favoriteColor", .type = "string", .required = false});

    // A fresh registry/dispatcher pair, not the process-wide singletons
    // replay() defaults to — this test's PE_-sibling
    // (test_journal_payload_evolution.cpp's PEFixture) does the same, so no
    // test leans on whatever else morph_tests has registered.
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<EB_ContactModel>("EB_ContactModel");
    dispatcher.registerAction<EB_ContactModel, EB_UpdateContact>("EB_ContactModel", "EB_UpdateContact");

    EB_UpdateContact action{.name = "Ada", .email = "ada@example.test", .extra = {}};
    action.extra["favoriteColor"] = std::string{"teal"};

    // The payload bytes and fingerprint a real dispatch would have journaled
    // (LogEntry::payload is JSON text, action_log.hpp) — built by hand here
    // since this test's point is replay(), not dispatch itself.
    const std::string payload = morph::model::ActionTraits<EB_UpdateContact>::toJson(action);
    std::vector<LogEntry> entries{LogEntry{
        .modelType = "EB_ContactModel",
        .entityKey = "1",
        .actionType = "EB_UpdateContact",
        .payload = payload,
        .schema = morph::model::ActionTraits<EB_UpdateContact>::payloadSchema(),
        .principal = "spike",
    }};

    // The real replay() entry point (journal.hpp): decodes the stored payload
    // through fromJson (the identical lenient glz::read exercised above) and
    // dispatches it into a freshly-constructed model. The extension bag
    // survives because the *type* still declares the unknown_read hook at
    // replay time — replay() itself needed no bag-aware change. This is the
    // "journal needs no framework change" half of the finding.
    auto holder = morph::journal::replay("EB_ContactModel", entries, registry, dispatcher);
    auto* concrete = dynamic_cast<morph::model::detail::ModelHolder<EB_ContactModel>*>(holder.get());
    REQUIRE(concrete != nullptr);
    CHECK(concrete->model.name == "Ada");
    REQUIRE(concrete->model.extra.contains("favoriteColor"));
    CHECK(concrete->model.extra.at("favoriteColor").get<std::string>() == "teal");

    ebRegistry().clear();
}
