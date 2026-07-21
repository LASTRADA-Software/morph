// SPDX-License-Identifier: Apache-2.0

// libFuzzer harness over morph::backend::RemoteServer::handle / dispatchMessage
// -- the coverage-guided generalisation of tests/test_server_limits.cpp's
// hand-picked cases (a fixed 5000-deep nesting, a lone continuation byte,
// etc.). Built only under -DMORPH_BUILD_FUZZERS=ON; see
// tests/fuzz/CMakeLists.txt and docs/spec/testing_strategy.md.
//
// Invariant under fuzzing: every input, handed directly to RemoteServer::handle
// exactly as a transport would, yields a reply that itself decodes as a wire
// Envelope with kind "ok" or "err". Never crashes; never hangs (bounded by
// libFuzzer's own -timeout flag, not by anything in this harness).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <thread>

// Must have external linkage so Glaze's reflection can mangle the type name
// (matches the convention every other morph test fixture model follows).
struct FuzzDispatchAction {
    std::string s;
    int n = 0;
};
struct FuzzDispatchModel {
    std::string execute(const FuzzDispatchAction& act) { return act.s + std::to_string(act.n); }
};

BRIDGE_REGISTER_MODEL(FuzzDispatchModel, "Fuzz_DispatchModel")
BRIDGE_REGISTER_ACTION(FuzzDispatchModel, FuzzDispatchAction, "Fuzz_DispatchAction")

namespace {

// Long-lived server + one pre-registered model instance, built once on first
// use. registerModelWithContext's handleInline path is synchronous, so
// `modelId` is valid immediately after construction (it is always 1 -- the
// first register on a fresh RemoteServer -- which the seed corpus relies on).
struct Harness {
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(pool);
    uint64_t modelId = 0;

    Harness() {
        auto reply = morph::wire::decode(
            server->handleInline(morph::wire::encode(morph::wire::makeRegister("Fuzz_DispatchModel"))));
        modelId = reply.modelId;
    }
};

Harness& harness() {
    static Harness instance;
    return instance;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto& h = harness();

    // Fuzz bytes go straight to RemoteServer::handle exactly as a transport
    // would hand it raw wire input -- no attempt to shape it into a valid
    // envelope first, so libFuzzer's coverage-guided search is free to
    // discover the envelope shape (and, from the seed corpus, valid executes
    // against h.modelId) on its own.
    std::string msg(reinterpret_cast<const char*>(data), size);

    std::atomic<bool> done{false};
    std::string replyRaw;
    h.server->handle(std::move(msg), [&](std::string out) {
        replyRaw = std::move(out);
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto replyEnv = morph::wire::decode(replyRaw);
    if (replyEnv.kind != "ok" && replyEnv.kind != "err") {
        std::abort();
    }
    return 0;
}
