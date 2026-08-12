// SPDX-License-Identifier: Apache-2.0
//
// Compile/link-check fixture for issue #61: proves that a MORPH_CLIENT_ONLY
// client's translation unit never needs to see a model's complete class body
// (and therefore never needs the header for a persistence mixin the model
// happens to inherit from/embed) merely to dispatch one of its actions.
//
// ClientOnlyFacadeModel below is *declared* (forward declaration) but never
// *defined* anywhere in this link -- deliberately, to prove the point: the
// plain BRIDGE_REGISTER_ACTION(...) macro cannot be used here at all (its
// `Result` deduction, `decltype(std::declval<M&>().execute(std::declval<A>()))`,
// requires M to be a complete type with execute() declared -- see
// docs/spec/core/registry.md, "MORPH_CLIENT_ONLY" and
// "BRIDGE_REGISTER_ACTION_FOR_CLIENT"). BRIDGE_REGISTER_ACTION_FOR_CLIENT names
// the result type explicitly instead, so it never needs to look at M's members
// at all -- only forward-declare it, exactly as a client whose model mixes in
// an ORM persistence layer (or, for a WASM/browser build, a native database
// client library with no include path at all) needs to be able to do.
//
// Uses BRIDGE_REGISTER_ACTION_FOR_CLIENT_5 directly rather than the public
// variadic-dispatch macro, mirroring client_only_no_model_link.cpp's own
// workaround for MSVC's C4003 "not enough arguments" preprocessor warning on
// the 3-arg invocation form combined with MORPH_CLIENT_ONLY's empty
// MORPH_DETAIL_REGISTER_ACTION_LOCAL expansion.

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// Forward-declared, never defined anywhere in this translation unit or this
// link -- the whole point of the probe. A real app's model would instead be
// forward-declared here while its actual definition (with its persistence
// mixin) lives only in the server-side translation unit that owns it.
struct ClientOnlyFacadeModel;

struct ClientOnlyFacadeAction {
    int x = 0;
};

struct ClientOnlyFacadeResult {
    int doubled = 0;
};

BRIDGE_REGISTER_MODEL(ClientOnlyFacadeModel, "ClientOnlyFacadeModel")
BRIDGE_REGISTER_ACTION_FOR_CLIENT_5(ClientOnlyFacadeModel, ClientOnlyFacadeAction, ClientOnlyFacadeResult,
                                    "ClientOnlyFacadeAction", ::morph::model::Loggable::Yes)

// A second forward-declared-only model/action pair, this time exercising the
// 4-argument (implicit Loggable::Yes) arity via BRIDGE_REGISTER_ACTION_FOR_CLIENT_4
// directly -- NOT the public variadic-dispatch macro
// (BRIDGE_REGISTER_ACTION_FOR_CLIENT(...)) itself: that form hits the exact
// same MSVC C4003 "not enough arguments for function-like macro invocation
// 'BRIDGE_REGISTER_ACTION_FOR_CLIENT_PICK'" issue client_only_no_model_link.cpp
// documents for BRIDGE_REGISTER_ACTION's own 3-arg form -- the spurious
// warning corrupts the rest of this macro's expansion under
// MORPH_CLIENT_ONLY's empty MORPH_DETAIL_REGISTER_ACTION_LOCAL, and the
// probe fails to compile. Calling _4 directly sidesteps the variadic
// dispatch while still exercising the arity this macro's public form maps
// 4 arguments to.
struct ClientOnlyFacadeModel2;

struct ClientOnlyFacadeAction2 {
    int x = 0;
};

struct ClientOnlyFacadeResult2 {
    int doubled = 0;
};

BRIDGE_REGISTER_MODEL(ClientOnlyFacadeModel2, "ClientOnlyFacadeModel2")
BRIDGE_REGISTER_ACTION_FOR_CLIENT_4(ClientOnlyFacadeModel2, ClientOnlyFacadeAction2, ClientOnlyFacadeResult2,
                                    "ClientOnlyFacadeAction2")

namespace {

struct InlineExecutor : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// A minimal, self-contained IModelHolder that carries no Model payload at
// all -- deliberately NOT morph::model::detail::ModelHolder<ClientOnlyFacadeModel>,
// which would require ClientOnlyFacadeModel complete (it stores one by
// value). LocalBackend::registerModel calls its factory SYNCHRONOUSLY inside
// Bridge::registerHandler(binding) (registerModelWithContext has no async
// path for LocalBackend -- see IBackend::registerModelAsync's doc comment),
// so the factory must succeed and return a real holder rather than throw;
// throwing here would escape BridgeHandler's pre-built-binding constructor
// itself, uncaught, in this probe's main() -- not the MORPH_CLIENT_ONLY
// dispatch-time throw this probe means to observe.
//
// type()/into<Model>() are never reached on this probe's actual dispatch
// path: Bridge::executeVia's localOp is compiled (under MORPH_CLIENT_ONLY)
// to throw std::logic_error before ever calling holder.into<Model>(), for
// every backend LocalBackend or otherwise -- see docs/spec/core/registry.md,
// "MORPH_CLIENT_ONLY". type() returning typeid(void) here is therefore never
// observed; it exists only so IModelHolder's pure virtual is satisfied.
struct NullModelHolder : morph::model::detail::IModelHolder {
    [[nodiscard]] std::type_index type() const noexcept override { return std::type_index{typeid(void)}; }
    [[nodiscard]] bool isBackendChangeAware() const noexcept override { return false; }
};

}  // namespace

int main() {
    // Proves ActionTraits<ClientOnlyFacadeAction> is fully usable (JSON codecs,
    // Result type) without ClientOnlyFacadeModel ever being complete.
    using Traits = morph::model::ActionTraits<ClientOnlyFacadeAction>;
    static_assert(std::is_same_v<Traits::Result, ClientOnlyFacadeResult>);
    static_assert(Traits::loggable == ::morph::model::Loggable::Yes);

    using Traits2 = morph::model::ActionTraits<ClientOnlyFacadeAction2>;
    static_assert(std::is_same_v<Traits2::Result, ClientOnlyFacadeResult2>);
    static_assert(Traits2::loggable == ::morph::model::Loggable::Yes);

    ClientOnlyFacadeAction const action{.x = 21};
    std::string const json = Traits::toJson(action);
    ClientOnlyFacadeAction const roundTripped = Traits::fromJson(json);

    ClientOnlyFacadeAction2 const action2{.x = 7};
    std::string const json2 = Traits2::toJson(action2);
    ClientOnlyFacadeAction2 const roundTripped2 = Traits2::fromJson(json2);

    if (roundTripped.x != action.x || roundTripped2.x != action2.x) {
        return 1;
    }

    // Second half of the probe: prove BridgeHandler<ClientOnlyFacadeModel> --
    // still incomplete here -- can actually be constructed and dispatched
    // through, via the pre-built-binding constructor with a modelFactory that
    // returns a dummy NullModelHolder (see above) instead of a real
    // ModelHolder<ClientOnlyFacadeModel> (which would need
    // ClientOnlyFacadeModel complete -- reintroducing the exact problem this
    // macro exists to avoid). This is the construction path
    // BRIDGE_REGISTER_ACTION_FOR_CLIENT's doc comment recommends; the default
    // BridgeHandler constructor would call Bridge::registerHandler<Model>(),
    // which needs ModelFactory::create<Model>() and therefore Model complete.
    morph::exec::ThreadPoolExecutor pool{1};
    InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = std::string{morph::model::ModelTraits<ClientOnlyFacadeModel>::typeId()};
    binding->modelFactory = []() -> std::unique_ptr<morph::model::detail::IModelHolder> {
        return std::make_unique<NullModelHolder>();
    };
    morph::bridge::BridgeHandler<ClientOnlyFacadeModel> handler{bridge, &cbExec, binding};

    // executeJson dispatches through Bridge::executeVia -> LocalBackend, whose
    // localOp is compiled to throw std::logic_error under MORPH_CLIENT_ONLY
    // rather than call ClientOnlyFacadeModel::execute (never defined anywhere
    // in this link). Observing that specific throw proves the whole
    // construct-and-dispatch path compiles and runs without
    // ClientOnlyFacadeModel ever being complete.
    std::atomic<bool> completed{false};
    std::atomic<bool> gotExpectedError{false};
    handler.executeJson("ClientOnlyFacadeAction", json)
        .then([&](std::string) { completed.store(true); })
        .onError([&](const std::exception_ptr& eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::logic_error& exc) {
                std::string const what{exc.what()};
                gotExpectedError.store(what.find("MORPH_CLIENT_ONLY") != std::string::npos);
            } catch (...) {
            }
            completed.store(true);
        });

    for (int idx = 0; idx < 200 && !completed.load(); ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!completed.load() || !gotExpectedError.load()) {
        return 1;
    }
    return 0;
}
