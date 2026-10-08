// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using morph::reactive::Runtime;
using morph::reactive::Scope;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;

class Witness {
public:
    Witness(std::vector<std::string>& log, std::string name) : _log{&log}, _name{std::move(name)} {}
    ~Witness() {
        try {
            _log->push_back(_name);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor must not throw; a lost entry fails the checks
        }
    }
    Witness(Witness const&) = delete;
    Witness& operator=(Witness const&) = delete;
    Witness(Witness&&) = delete;
    Witness& operator=(Witness&&) = delete;

private:
    std::vector<std::string>* _log;
    std::string _name;
};

}  // namespace

// Mutation: Scope::clear destroys the oldest object first.
TEST_CASE("reactive::Scope: destroys what it owns in reverse creation order", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    std::vector<std::string> log;
    {
        Scope scope{runtime};
        scope.make<Witness>(log, "first");
        scope.adopt(std::make_unique<Witness>(log, "second"));
        scope.make<Witness>(log, "third");
        CHECK(scope.size() == 3);
    }
    CHECK(log == std::vector<std::string>{"third", "second", "first"});
}

// Mutation: Scope::clear destroys nothing.
TEST_CASE("reactive::Scope: an effect it owns stops when the scope is cleared", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Scope scope{runtime};
    int runs = 0;
    scope.effect([&] {
        static_cast<void>(value.get());
        ++runs;
    });
    auto const& doubled = scope.computed([&] { return value.get() * 2; });
    CHECK(doubled.get() == 0);
    scope.clear();
    CHECK(scope.size() == 0);
    value.set(1);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}

// Mutation: Scope::clear destroys nothing.
TEST_CASE("reactive::Scope: an effect it owns can clear the scope from its own body", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Scope scope{runtime};
    std::vector<std::string> log;
    int runs = 0;
    int tail = 0;
    scope.make<Witness>(log, "witness");
    scope.effect([&] {
        ++runs;
        if (value.get() == 1) {
            scope.clear();
            // The body goes on running after the Effect that owns it is gone.
            ++tail;
        }
    });
    CHECK(scope.size() == 2);
    value.set(1);
    REQUIRE(owner.runOne());
    CHECK(runs == 2);
    CHECK(tail == 1);
    CHECK(scope.size() == 0);
    CHECK(log == std::vector<std::string>{"witness"});
    value.set(2);
    CHECK(owner.pending() == 0);
    CHECK(runs == 2);
}

// Mutation: Effect::recompute adopts its run's sources after the run destroyed the Effect (crashes).
TEST_CASE("reactive::Scope: an effect it owns can destroy the scope from its own body", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    auto scope = std::make_unique<Scope>(runtime);
    int runs = 0;
    int tail = 0;
    scope->effect([&] {
        ++runs;
        if (value.get() == 1) {
            scope.reset();
            ++tail;
        }
    });
    value.set(1);
    REQUIRE(owner.runOne());
    CHECK(scope == nullptr);
    CHECK(runs == 2);
    CHECK(tail == 1);
    value.set(2);
    CHECK(owner.pending() == 0);
}

// Mutation: Scope::clear destroys nothing.
TEST_CASE("reactive::Scope: an effect it owns can remount the scope from its own body", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> trigger{runtime, 0};
    Signal<int> value{runtime, 0};
    Scope scope{runtime};
    int innerRuns = 0;
    scope.effect([&] {
        if (trigger.get() == 1) {
            scope.clear();
            scope.effect([&] {
                static_cast<void>(value.get());
                ++innerRuns;
            });
        }
    });
    trigger.set(1);
    REQUIRE(owner.runOne());
    CHECK(scope.size() == 1);
    CHECK(innerRuns == 1);
    value.set(1);
    REQUIRE(owner.runOne());
    CHECK(innerRuns == 2);
}

// Mutation: Scope::clear destroys the last object in place before popping it, so the destructor's clear() sees it
// still listed (aborts).
TEST_CASE("reactive::Scope: an owned object may clear the scope while it is being destroyed", "[reactive]") {
    class Clearer {
    public:
        Clearer(Scope& scope, std::size_t& seenSize) : _scope{&scope}, _seenSize{&seenSize} {}
        ~Clearer() {
            *_seenSize = _scope->size();
            _scope->clear();
        }
        Clearer(Clearer const&) = delete;
        Clearer& operator=(Clearer const&) = delete;
        Clearer(Clearer&&) = delete;
        Clearer& operator=(Clearer&&) = delete;

    private:
        Scope* _scope;
        std::size_t* _seenSize;
    };

    Owner owner;
    Runtime runtime{owner};
    std::vector<std::string> log;
    std::size_t seenSize = 99;
    Scope scope{runtime};
    scope.make<Witness>(log, "first");
    scope.make<Witness>(log, "second");
    scope.make<Clearer>(scope, seenSize);
    scope.clear();
    // An object has left the list before its destructor runs; a plain pop_back would still count it.
    CHECK(seenSize == 2);
    CHECK(scope.size() == 0);
    CHECK(log == std::vector<std::string>{"second", "first"});
}

// Mutation: ~Scope does not mark the makes in progress, so make() adopts into the dead scope (aborts).
TEST_CASE("reactive::Scope: an effect whose first run destroys the scope leaves nothing behind", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    std::size_t const nodesBefore = runtime.core()->liveNodes();
    auto scope = std::make_unique<Scope>(runtime);
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> const weakToken = token;
    int runs = 0;
    int tail = 0;
    scope->effect([&, held = std::move(token)] {
        static_cast<void>(value.get());
        static_cast<void>(held);
        ++runs;
        scope.reset();
        ++tail;
    });
    CHECK(scope == nullptr);
    CHECK(runs == 1);
    CHECK(tail == 1);
    // The Effect was destroyed at once, with its closure: the dead scope owns nothing.
    CHECK(weakToken.expired());
    CHECK(runtime.core()->liveNodes() == nodesBefore);
    value.set(1);
    CHECK(owner.pending() == 0);
    CHECK(runs == 1);
}

// Mutation: Scope::clear marks the makes in progress as dead, as ~Scope does (the Effect is destroyed; crashes).
TEST_CASE("reactive::Scope: an effect whose first run clears the scope is kept", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<int> value{runtime, 0};
    Scope scope{runtime};
    std::vector<std::string> log;
    int runs = 0;
    scope.make<Witness>(log, "before");
    scope.effect([&] {
        static_cast<void>(value.get());
        if (++runs == 1) {
            scope.clear();
        }
    });
    // The clear ran inside the constructor: the Effect is stored after it, so it is the only owner left.
    CHECK(log == std::vector<std::string>{"before"});
    CHECK(scope.size() == 1);
    value.set(1);
    REQUIRE(owner.runOne());
    CHECK(runs == 2);
}

// Mutation: Scope::make adopts the object even when the scope died during its construction (aborts).
TEST_CASE("reactive::Scope: a nested make whose first run destroys the scope leaves nothing behind", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    auto scope = std::make_unique<Scope>(runtime);
    int innerRuns = 0;
    scope->effect([&] {
        if (scope == nullptr) {
            return;
        }
        // A make inside a make: destroying the scope in the inner first run must reach the outer make too.
        scope->effect([&] {
            ++innerRuns;
            scope.reset();
        });
    });
    CHECK(scope == nullptr);
    CHECK(innerRuns == 1);
    CHECK(runtime.core()->liveNodes() == 0);
}

// Mutation: Scope::make does not leave() its frame when the constructor throws (crashes).
TEST_CASE("reactive::Scope: a constructor that throws leaves the scope usable and destroyable", "[reactive]") {
    class Thrower {
    public:
        Thrower() { throw std::runtime_error{"boom"}; }
    };

    Owner owner;
    Runtime runtime{owner};
    std::vector<std::string> log;
    {
        Scope scope{runtime};
        scope.make<Witness>(log, "kept");
        CHECK_THROWS_AS(scope.make<Thrower>(), std::runtime_error);
        CHECK(scope.size() == 1);
        scope.make<Witness>(log, "after");
        CHECK(scope.size() == 2);
        // The destructor walks the frames of makes in progress: one left behind by the throw would
        // point into a dead stack frame.
    }
    CHECK(log == std::vector<std::string>{"after", "kept"});
}

// Mutation: reverse the comparison in Node::runsAfter.
TEST_CASE("reactive::Scope: a parent Effect runs before the child it mounted, whatever order the writes came in",
          "[reactive]") {
    // The child exists only while `show` is true and reads `selected` as if it were engaged; one update
    // deselects and hides. Whichever field it writes first, the parent must unmount the child before the
    // child can see the deselection.
    bool const deselectFirst = GENERATE(true, false);
    Owner owner;
    Runtime runtime{owner};
    Signal<bool> show{runtime, true};
    Signal<std::optional<int>> selected{runtime, 7};
    Scope root{runtime};
    Scope child{runtime};
    std::vector<std::string> log;
    root.effect([&] {
        child.clear();
        if (show.get()) {
            child.effect([&] { log.emplace_back(selected.get().has_value() ? "child: selected" : "child: none"); });
        } else {
            log.emplace_back("parent: unmounted");
        }
    });
    REQUIRE(log == std::vector<std::string>{"child: selected"});
    log.clear();
    runtime.batch([&] {
        if (deselectFirst) {
            selected.set(std::nullopt);
            show.set(false);
        } else {
            show.set(false);
            selected.set(std::nullopt);
        }
    });
    owner.runAll();
    CHECK(log == std::vector<std::string>{"parent: unmounted"});
}

// Mutation: reverse the comparison in Node::runsAfter.
TEST_CASE("reactive::Scope: a remounted child still runs after its parent", "[reactive]") {
    // A parent that re-runs and remounts gives the child a newer place in the order, so the next write
    // that reaches both still runs the parent first.
    Owner owner;
    Runtime runtime{owner};
    Signal<int> generation{runtime, 0};
    Signal<int> detail{runtime, 0};
    Scope root{runtime};
    Scope child{runtime};
    std::vector<std::string> log;
    root.effect([&] {
        log.push_back("parent " + std::to_string(generation.get()));
        child.clear();
        child.effect([&] { log.push_back("child " + std::to_string(detail.get())); });
    });
    generation.set(1);
    owner.runAll();
    log.clear();
    runtime.batch([&] {
        detail.set(1);
        generation.set(2);
    });
    owner.runAll();
    CHECK(log == std::vector<std::string>{"parent 2", "child 1"});
}

// Mutation: Scope::child() passes its own depth instead of one more.
TEST_CASE("reactive::Scope: a child scope is one level deeper than its parent", "[reactive]") {
    Owner owner;
    Runtime runtime{owner};
    Scope root{runtime};
    Scope& child = root.child();
    Scope& grandchild = child.child();
    CHECK(root.depth() == 0);
    CHECK(child.depth() == 1);
    CHECK(grandchild.depth() == 2);
    CHECK(&grandchild.runtime() == &runtime);
    CHECK(root.size() == 1);
    CHECK(child.size() == 1);
}

// The row binding is made before its owner's binding, as a renderer that mounts children before binding its own
// state does, so creation order alone would run it first. Mutations: order the flush queue by creation alone
// (Node::runsAfter ignores depth); Scope::effect makes its Effect at depth zero.
TEST_CASE("reactive::Scope: an owner's Effect runs before a deeper scope's Effect made before it", "[reactive]") {
    bool const deselectFirst = GENERATE(true, false);
    Owner owner;
    Runtime runtime{owner};
    Signal<bool> show{runtime, true};
    Signal<std::optional<int>> selected{runtime, 7};
    Scope root{runtime};
    Scope& rows = root.child();
    std::vector<std::string> log;
    rows.effect([&] { log.emplace_back(selected.get().has_value() ? "row: selected" : "row: none"); });
    root.effect([&] {
        if (!show.get()) {
            rows.clear();
            log.emplace_back("owner: removed the row");
        }
    });
    REQUIRE(log == std::vector<std::string>{"row: selected"});
    log.clear();
    runtime.batch([&] {
        if (deselectFirst) {
            selected.set(std::nullopt);
            show.set(false);
        } else {
            show.set(false);
            selected.set(std::nullopt);
        }
    });
    owner.runAll();
    CHECK(log == std::vector<std::string>{"owner: removed the row"});
}
