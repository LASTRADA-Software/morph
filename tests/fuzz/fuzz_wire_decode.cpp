// SPDX-License-Identifier: Apache-2.0

// libFuzzer harness over morph::wire::decode (the outer envelope parse) and,
// for a decoded "execute" envelope carrying a non-empty body, over
// ActionTraits<FuzzInnerAction>::fromJson -- the inner re-parse the action
// codec performs on the opaque `body` string (see docs/spec/core/wire.md's
// "the body double-parse hazard"). Built only under -DMORPH_BUILD_FUZZERS=ON;
// see tests/fuzz/CMakeLists.txt and docs/spec/testing_strategy.md.
//
// Invariant under fuzzing: every input either decodes (and, for execute
// envelopes, re-parses) successfully or throws std::runtime_error. It must
// never crash, trip a sanitizer, or hang (the last is bounded by libFuzzer's
// own -timeout flag, not by anything internal to this harness).

#include <cstddef>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Must have external linkage so Glaze's reflection can mangle the type name
// (matches the convention every other morph test fixture model follows).
struct FuzzInnerAction {
    std::string text;
    int number = 0;
    std::vector<std::string> tags;
};
struct FuzzInnerModel {
    int execute(const FuzzInnerAction&) { return 0; }
};

BRIDGE_REGISTER_MODEL(FuzzInnerModel, "Fuzz_InnerModel")
BRIDGE_REGISTER_ACTION(FuzzInnerModel, FuzzInnerAction, "Fuzz_InnerAction")

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view input{reinterpret_cast<const char*>(data), size};
    try {
        auto env = morph::wire::decode(input);
        if (env.kind == "execute" && !env.body.empty()) {
            try {
                // The second (inner) parse the action codec performs on the
                // opaque `body` string -- decode() above never walks its
                // structure, so this is where a smuggled malformed or
                // pathologically-nested payload would actually detonate.
                (void)morph::model::ActionTraits<FuzzInnerAction>::fromJson(env.body);
            } catch (const std::runtime_error&) {
                // A rejected inner body is the DEFINED outcome.
            }
        }
    } catch (const std::runtime_error&) {
        // A rejected envelope (oversized or malformed) is the DEFINED outcome.
    }
    return 0;
}
