// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "reactive_fake_server.hpp"
#include "test_support.hpp"

namespace {

using morph::async::Completion;
using morph::reactive::Computed;
using morph::reactive::Effect;
using morph::reactive::Query;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
using FakeServer = morph::testing::FakeServer<>;
namespace site = morph::reactive::detail::site;

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

// No operator==: every re-run of the key re-issues.
struct OpaqueLookup {
    int id = 0;
};

// No operator==: a result made of these always propagates.
struct Opaque {
    int id = 0;
};

using Detail = Query<Lookup, std::string>;

}  // namespace

// Mutation: the Query's Effect reads the key but does not hand it to onKey.
TEST_CASE("reactive::Query: issues its key on construction and delivers the value", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail const detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    REQUIRE(server.calls() == 1);
    CHECK(detail.pending());
    server.resolve(0, "one");
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"one"});
    CHECK(detail.error() == nullptr);
}

// Mutation: Query::issue does not reset `_inflight`, so the call it replaces is neither stopped nor gated.
TEST_CASE("reactive::Query: a key change mid-flight drops the stale reply", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    CHECK(server.idOf(1) == 2);

    server.resolve(1, "two");
    owner.runAll();
    server.resolve(0, "one");  // A lands after B
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"two"});
    CHECK_FALSE(detail.pending());
}

// Mutation: Query::issue does not reset `_inflight`, so the call it replaces is neither stopped nor gated.
TEST_CASE("reactive::Query: a superseded call is asked to stop", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    CHECK_FALSE(server.stopRequested(0));
    selected.set(2);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    CHECK(server.stopRequested(0));
    CHECK_FALSE(server.stopRequested(1));
    CHECK(detail.pending());
}

// Mutation: Query::issue does not reset `_inflight`, so the call it replaces is neither stopped nor gated.
TEST_CASE("reactive::Query: a superseded failure leaves error() clear", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    owner.runAll();
    server.resolve(1, "two");
    server.reject(0, "late failure");
    owner.runAll();
    CHECK(detail.error() == nullptr);
    CHECK(detail.value() == std::optional<std::string>{"two"});
}

// Mutation: the idle branch of Query::issue keeps the value, or keeps the error.
TEST_CASE("reactive::Query: a nullopt key is idle", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<std::optional<int>> selected{runtime, std::nullopt};
    Detail detail{runtime, server.via<Lookup>(), [&]() -> std::optional<Lookup> {
                      if (auto const id = selected.get()) {
                          return Lookup{*id};
                      }
                      return std::nullopt;
                  }};
    CHECK(server.calls() == 0);
    CHECK_FALSE(detail.pending());
    CHECK_FALSE(detail.value().has_value());

    selected.set(1);
    owner.runAll();
    server.resolve(0, "one");
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"one"});
    detail.refetch();
    server.reject(1, "down");
    owner.runAll();
    REQUIRE(detail.error() != nullptr);

    selected.set(std::nullopt);
    owner.runAll();
    CHECK_FALSE(detail.value().has_value());
    CHECK(detail.error() == nullptr);
    CHECK_FALSE(detail.pending());
    CHECK(server.calls() == 2);
    detail.refetch();  // idle stays idle
    CHECK(server.calls() == 2);
}

// Mutation: Query::issue does not reset `_inflight`, so the call it replaces is neither stopped nor gated.
TEST_CASE("reactive::Query: going idle stops the call in flight and drops its reply", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<std::optional<int>> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&]() -> std::optional<Lookup> {
                            if (auto const id = selected.get()) {
                                return Lookup{*id};
                            }
                            return std::nullopt;
                        }};
    REQUIRE(detail.pending());
    selected.set(std::nullopt);
    owner.runAll();
    CHECK(server.stopRequested(0));
    CHECK_FALSE(detail.pending());

    server.resolve(0, "one");
    owner.runAll();
    CHECK_FALSE(detail.value().has_value());
    CHECK_FALSE(detail.pending());
}

// Mutation: Query::issue does not reset `_inflight`, so the call it replaces is neither stopped nor gated.
TEST_CASE("reactive::Query: A, B, A across flushes re-issues A and drops both stale replies", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    owner.runAll();
    selected.set(1);
    owner.runAll();
    REQUIRE(server.calls() == 3);
    CHECK(server.idOf(2) == 1);
    CHECK(server.stopRequested(0));
    CHECK(server.stopRequested(1));
    CHECK_FALSE(server.stopRequested(2));

    server.resolve(0, "first one");
    server.resolve(1, "two");
    owner.runAll();
    CHECK_FALSE(detail.value().has_value());
    CHECK(detail.pending());

    server.resolve(2, "one");
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"one"});
    CHECK_FALSE(detail.pending());
}

// Mutation: drop the key equality gate in Query::onKey.
TEST_CASE("reactive::Query: A, B, A within one flush issues nothing and stops nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    selected.set(1);
    owner.runAll();
    CHECK(server.calls() == 1);
    CHECK_FALSE(server.stopRequested(0));
    CHECK(detail.pending());

    server.resolve(0, "one");
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"one"});
}

// Mutation: Query::issue does not reset `_inflight`, so the call it replaces is neither stopped nor gated.
TEST_CASE("reactive::Query: refetch() while in flight supersedes the call it replaces", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    detail.refetch();
    REQUIRE(server.calls() == 2);
    CHECK(server.stopRequested(0));
    CHECK_FALSE(server.stopRequested(1));

    server.reject(0, "stale");
    owner.runAll();
    CHECK(detail.error() == nullptr);
    CHECK(detail.pending());

    server.resolve(1, "one");
    owner.runAll();
    CHECK(detail.value() == std::optional<std::string>{"one"});
    CHECK_FALSE(detail.pending());
}

// Mutation: the success handler does not clear `_error`.
TEST_CASE("reactive::Query: error, then success clears it; value survives a refetch", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    server.reject(0, "down");
    owner.runAll();
    CHECK(morph::reactive::errorMessage(detail.error()) == "down");
    CHECK_FALSE(detail.pending());

    detail.refetch();
    CHECK(server.calls() == 2);
    CHECK(detail.pending());
    server.resolve(1, "one");
    owner.runAll();
    CHECK(detail.error() == nullptr);

    detail.refetch();
    CHECK(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"one"});
}

// Mutation: rethrow a synchronous fetcher exception from Query::issue instead of recording it.
TEST_CASE("reactive::Query: a fetcher that throws is an error, not a stuck pending()", "[reactive][control]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    bool down = false;
    Detail detail{runtime,
                  [&](Lookup const& action) {
                      if (down) {
                          throw std::runtime_error{"no route"};
                      }
                      return server.fetch(action);
                  },
                  [&] { return std::optional{Lookup{selected.get()}}; }};
    REQUIRE(detail.pending());

    // A key change while call 0 is in flight: the throw supersedes it, so nothing is pending any more.
    down = true;
    selected.set(2);
    owner.runAll();
    CHECK(server.calls() == 1);
    CHECK_FALSE(detail.pending());
    CHECK(morph::reactive::errorMessage(detail.error()) == "no route");
    CHECK(probe.count(site::kEffectThrew) == 0);
    server.resolve(0, "one");
    owner.runAll();
    CHECK_FALSE(detail.value().has_value());

    // refetch() takes the same path, and a later success clears the error.
    detail.refetch();
    CHECK_FALSE(detail.pending());
    down = false;
    detail.refetch();
    CHECK(detail.pending());
    server.resolve(1, "two");
    owner.runAll();
    CHECK(detail.error() == nullptr);
    CHECK(detail.value() == std::optional<std::string>{"two"});
}

// Mutation: drop the key equality gate in Query::onKey.
TEST_CASE("reactive::Query: an unchanged comparable key issues nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> unrelated{runtime, 0};
    Detail const detail{runtime, server.via<Lookup>(), [&] {
                            static_cast<void>(unrelated.get());
                            return std::optional{Lookup{1}};
                        }};
    unrelated.set(1);
    owner.runAll();
    CHECK(server.calls() == 1);
}

// Mutation: Query::onKey treats a re-run of a key without == as unchanged when both are engaged.
TEST_CASE("reactive::Query: a non-comparable key re-issues on every re-run", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> unrelated{runtime, 0};
    Query<OpaqueLookup, std::string> const detail{runtime, server.via<OpaqueLookup>(), [&] {
                                                      static_cast<void>(unrelated.get());
                                                      return std::optional{OpaqueLookup{1}};
                                                  }};
    unrelated.set(1);
    owner.runAll();
    CHECK(server.calls() == 2);
}

// Mutation: the success handler writes `_value` only when it is empty.
TEST_CASE("reactive::Query: a result without == compiles and always propagates", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    morph::testing::FakeServer<std::vector<Opaque>> server{owner};
    Query<Lookup, std::vector<Opaque>> rows{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    int runs = 0;
    Effect const render{runtime, [&] {
                            static_cast<void>(rows.value());
                            ++runs;
                        }};
    server.resolve(0, {Opaque{1}, Opaque{2}});
    owner.runAll();
    REQUIRE(rows.value().has_value());
    CHECK(rows.value()->size() == 2);

    rows.refetch();
    owner.runAll();
    server.resolve(1, {Opaque{1}, Opaque{2}});
    owner.runAll();
    // Construction, the first result, and the equal second one: nothing can tell it is equal.
    CHECK(runs == 3);
}

// Mutation: attach the success handler without `_inflight` (crashes).
TEST_CASE("reactive::Query: a destroyed Query gates late replies", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    auto detail = std::make_unique<Detail>(runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; });
    detail.reset();
    CHECK(server.stopRequested(0));
    server.resolve(0, "late");
    owner.runAll();  // a delivery into the destroyed Query would be a use-after-free under ASan
    CHECK(server.calls() == 1);
}

// Mutation: attach the success handler without `_inflight` (crashes).
TEST_CASE("reactive::Query: a reply whose earlier handler destroys the Query is not delivered into it",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    std::unique_ptr<Detail> detail;
    auto const fetch = [&](Lookup const& action) {
        Completion<std::string> completion = server.fetch(action);
        completion.thenDetached([&](std::string const&) { detail.reset(); });
        return completion;
    };
    detail = std::make_unique<Detail>(runtime, fetch, [] { return std::optional{Lookup{1}}; });
    server.resolve(0, "one");
    owner.runAll();  // the Query's own handler runs after the reset: ASan sees a use-after-free if it is let in
    CHECK(detail == nullptr);
}

// Mutation: the Query's Effect carries on after the key expired the Query's lifetime token (crashes).
TEST_CASE("reactive::Query: a key that destroys the Query leaves it alone afterwards",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    std::unique_ptr<Detail> detail;
    detail = std::make_unique<Detail>(runtime, server.via<Lookup>(), [&] {
        int const id = selected.get();
        if (id == 2) {
            detail.reset();
        }
        // The key's own captures are still alive after it destroyed the Query.
        return std::optional{Lookup{selected.peek()}};
    });
    selected.set(2);
    owner.runAll();
    CHECK(detail == nullptr);
    CHECK(server.calls() == 1);
}

// Mutation: Query::issue holds the fetcher by reference, or carries on after the fetcher returned or threw having
// destroyed the Query (crashes).
TEST_CASE("reactive::Query: a fetcher that destroys the Query leaves it alone afterwards",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    std::unique_ptr<Detail> detail;
    bool throws = false;
    auto const fetch = [&](Lookup const& action) {
        // The action belongs to the Query: read it before destroying the Query.
        Lookup const copy = action;
        if (copy.id == 2) {
            detail.reset();
            if (throws) {
                throw std::runtime_error{"gone"};
            }
        }
        return server.fetch(copy);
    };
    detail = std::make_unique<Detail>(runtime, fetch, [&] { return std::optional{Lookup{selected.get()}}; });
    SECTION("and returns") {
        selected.set(2);
        owner.runAll();
        CHECK(server.calls() == 2);
        server.resolve(1, "two");
        owner.runAll();
    }
    SECTION("and throws") {
        throws = true;
        selected.set(2);
        owner.runAll();
        CHECK(server.calls() == 1);
    }
    CHECK(detail == nullptr);
}

// Mutation: flush inline at the end of the success handler and read `_value` after it: the Effect destroys the Query
// inside its own handler (ASan heap-use-after-free; a plain build does not crash).
TEST_CASE("reactive::Query: an Effect reading it may destroy it when the reply lands",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    auto detail = std::make_unique<Detail>(runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; });
    std::vector<std::string> seen;
    Effect const closer{runtime, [&] {
                            if (detail == nullptr) {
                                return;
                            }
                            if (auto const& value = detail->value()) {
                                seen.push_back(*value);
                                detail.reset();
                            }
                        }};
    server.resolve(0, "one");
    owner.runAll();
    CHECK(detail == nullptr);
    CHECK(seen == std::vector<std::string>{"one"});
}

// Mutation: the success handler does not lower `_pending`.
TEST_CASE("reactive::Query: an Effect reading it re-runs on each state change", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail const detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    std::vector<std::string> frames;
    Effect const render{runtime,
                        [&] { frames.push_back(detail.pending() ? "loading" : detail.value().value_or("empty")); }};
    server.resolve(0, "one");
    owner.runAll();
    CHECK(frames == std::vector<std::string>{"loading", "one"});
}

// Mutation: the Query's Effect catches a throwing key and treats it as idle.
TEST_CASE("reactive::Query: a throwing key is reported and leaves the query as it was",
          "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] {
                            if (selected.get() == 2) {
                                throw std::invalid_argument{"no such id"};
                            }
                            return std::optional{Lookup{selected.get()}};
                        }};
    server.resolve(0, "one");
    owner.runAll();
    selected.set(2);
    owner.runAll();
    CHECK(probe.count(site::kEffectThrew) == 1);
    CHECK(server.calls() == 1);
    CHECK(detail.value() == std::optional<std::string>{"one"});
}

// Mutation: the success handler writes whatever checkOwner() says.
TEST_CASE("reactive::Query: a delivery off the owner is reported and dropped", "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    morph::exec::MainThreadExecutor foreign;
    FakeServer server{foreign};
    Detail const detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    SECTION("a result") { server.resolve(0, "one"); }
    SECTION("a failure") { server.reject(0, "down"); }
    bool reported = false;
    std::thread{[&] {
        reported = morph::testing::pumpOwnerUntil(foreign, [&] { return probe.count(site::kOffOwner) >= 1; });
    }}.join();
    CHECK(reported);
    // Refused once, at the handler: no write inside it is attempted.
    CHECK(probe.count(site::kOffOwner) == 1);
    CHECK_FALSE(detail.value().has_value());
    CHECK(detail.error() == nullptr);
    CHECK(detail.pending());
}

// Mutation: Query::refetch ignores checkOwner().
TEST_CASE("reactive::Query: refetch() off the owner is reported and refused", "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    std::thread{[&] { detail.refetch(); }}.join();
    CHECK(probe.count(site::kOffOwner) == 1);
    CHECK(server.calls() == 1);
    CHECK_FALSE(server.stopRequested(0));
}

// Mutation: Query::issue calls the fetcher tracked.
TEST_CASE("reactive::Query: refetch() from an Effect does not subscribe it to what the fetcher reads",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> token{runtime, 0};  // read only by the fetcher, as a credential would be
    Signal<int> trigger{runtime, 0};
    Detail detail{runtime,
                  [&](Lookup const& action) {
                      static_cast<void>(token.get());
                      return server.fetch(action);
                  },
                  [] { return std::optional{Lookup{1}}; }};
    int runs = 0;
    Effect const reload{runtime, [&] {
                            ++runs;
                            if (trigger.get() != 0) {
                                detail.refetch();
                            }
                        }};
    trigger.set(1);
    owner.runAll();
    REQUIRE(runs == 2);
    REQUIRE(server.calls() == 2);

    token.set(1);
    owner.runAll();
    CHECK(runs == 2);
    CHECK(server.calls() == 2);
}

// Mutation: drop the isComputing() refusal in Query::refetch.
TEST_CASE("reactive::Query: refetch() inside a Computed is reported and issues nothing",
          "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    FakeServer server{owner};
    Detail detail{runtime, server.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    server.resolve(0, "one");
    owner.runAll();
    REQUIRE(server.calls() == 1);

    Computed<int> const sneaky{runtime, [&] {
                                   detail.refetch();
                                   return 1;
                               }};
    CHECK(sneaky.get() == 1);
    CHECK(probe.count(site::kIssueInComputed) == 1);
    CHECK(server.calls() == 1);
    CHECK_FALSE(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"one"});
}

// Mutation: Query::fail never clears the value.
TEST_CASE("reactive::Query: a failure for a new key clears the previous key's value", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    server.resolve(0, "one");
    owner.runAll();

    selected.set(2);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    CHECK(detail.value() == std::optional<std::string>{"one"});  // kept while the new key is in flight

    server.reject(1, "two failed");
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK_FALSE(detail.value().has_value());
    CHECK(morph::reactive::errorMessage(detail.error()) == "two failed");
}

// Mutation: Query::fail never clears the value.
TEST_CASE("reactive::Query: a fetcher that throws for a new key clears the previous key's value",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail const detail{runtime,
                        [&](Lookup const& action) {
                            if (action.id == 2) {
                                throw std::runtime_error{"no route"};
                            }
                            return server.fetch(action);
                        },
                        [&] { return std::optional{Lookup{selected.get()}}; }};
    server.resolve(0, "one");
    owner.runAll();

    selected.set(2);
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK_FALSE(detail.value().has_value());
    CHECK(morph::reactive::errorMessage(detail.error()) == "no route");
}

// Mutation: Query::fail clears the value whatever key it was fetched for.
TEST_CASE("reactive::Query: a failed refetch of the same key keeps its value", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    FakeServer server{owner};
    Signal<int> selected{runtime, 1};
    Detail detail{runtime, server.via<Lookup>(), [&] { return std::optional{Lookup{selected.get()}}; }};
    selected.set(2);
    owner.runAll();
    server.resolve(1, "two");
    owner.runAll();

    detail.refetch();
    server.reject(2, "refetch failed");
    owner.runAll();
    CHECK_FALSE(detail.pending());
    CHECK(detail.value() == std::optional<std::string>{"two"});
    CHECK(morph::reactive::errorMessage(detail.error()) == "refetch failed");
}

// Mutation: errorMessage returns "unknown error" for a std::exception too.
TEST_CASE("reactive::errorMessage: what(), a foreign throw, and null", "[reactive][control]") {
    using morph::reactive::errorMessage;
    CHECK(errorMessage(std::make_exception_ptr(std::runtime_error{"boom"})) == "boom");
    CHECK(errorMessage(std::make_exception_ptr(42)) == "unknown error");
    CHECK(errorMessage(nullptr).empty());
}
