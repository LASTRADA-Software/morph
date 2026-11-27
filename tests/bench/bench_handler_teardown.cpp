// SPDX-License-Identifier: Apache-2.0

// Handler teardown cost as a function of how many handlers are live.
//
// Destroying one `BridgeHandler` should cost the same whether the bridge holds
// ten handlers or ten thousand. If it scans the bridge's other handlers, the
// cost per handler grows with the count and tearing a whole population down is
// quadratic: invisible at the sizes a unit test builds, a stall at the sizes a
// long-lived client reaches.
//
// What is gated is a RATIO: the per-handler destroy cost at a large count over
// the per-handler destroy cost at a small count. A constant-cost teardown gives
// about 1; a scan of every live handler gives about `large / small`. A ratio
// cancels machine speed, compiler and build type, which an absolute time per
// handler cannot, so one threshold holds on a loaded runner, under Debug and
// on a slower Windows scheduler alike.
//
// Method:
//   * `Bridge` over `LocalBackend` over a two-thread pool, owned by this thread.
//   * `N` handlers are created and bound, and a call through the last one
//     settles so every bind has completed; then they are destroyed one by one
//     in creation order and only the destruction is timed.
//   * The small count is timed as the best of `kSmallTrials` runs, the large
//     count as the best of `kLargeTrials`; the minimum is the run least
//     disturbed by the rest of the machine.
//
// Build: `-DMORPH_BUILD_LOAD_TESTS=ON`, target `morph_bench_teardown`.
// See docs/spec/testing_strategy.md.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// External linkage so glaze's reflection can mangle the type names.
struct BenchTeardownPing {
    int x = 0;
};
struct BenchTeardownPong {
    int y = 0;
};
struct BenchTeardownModel {
    BenchTeardownPong execute(const BenchTeardownPing& action) { return BenchTeardownPong{.y = action.x}; }
};

BRIDGE_REGISTER_MODEL(BenchTeardownModel, "BenchTeardown_Model")
BRIDGE_REGISTER_ACTION(BenchTeardownModel, BenchTeardownPing, "BenchTeardown_Ping")

namespace {

constexpr std::size_t kSmall = 2000;
constexpr std::size_t kLarge = 32000;
constexpr int kSmallTrials = 7;
constexpr int kLargeTrials = 3;
constexpr double kDefaultMaxRatio = 7.0;

// A serial executor the owning thread sleeps on.
class SleepingOwner : public ::morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override {
        {
            std::scoped_lock const lock{_mtx};
            _queue.push_back(std::move(task));
        }
        _cv.notify_one();
    }

    template <typename Pred>
    void runUntil(Pred done) {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{_mtx};
                _cv.wait(lock, [this] { return !_queue.empty(); });
                task = std::move(_queue.front());
                _queue.pop_front();
            }
            {
                ::core::async::ExecutorScope const scope{coreExecutor()};
                if (task) {
                    task();
                }
            }
            if (done()) {
                return;
            }
        }
    }

private:
    std::mutex _mtx;
    std::condition_variable _cv;
    std::deque<std::function<void()>> _queue;
};

// Microseconds to destroy @p count live handlers on one bridge, per handler.
double destroyMicrosPerHandler(std::size_t count) {
    ::morph::exec::ThreadPoolExecutor pool{2};
    SleepingOwner owner;
    auto backend = std::make_unique<::morph::backend::LocalBackend>(pool);
    ::morph::bridge::Bridge bridge{std::move(backend), owner};

    std::vector<std::unique_ptr<::morph::bridge::BridgeHandler<BenchTeardownModel>>> handlers;
    handlers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        handlers.push_back(std::make_unique<::morph::bridge::BridgeHandler<BenchTeardownModel>>(bridge, &owner));
    }
    // A call through the last handler settles only after every bind queued
    // ahead of it on the owner has run.
    int settled = 0;
    handlers.back()
        ->execute(BenchTeardownPing{.x = 1})
        .then([&settled](const BenchTeardownPong&) { ++settled; })
        .onError([&settled](const std::exception_ptr&) { ++settled; });
    owner.runUntil([&] { return settled >= 1; });

    auto const begin = std::chrono::steady_clock::now();
    for (auto& handler : handlers) {
        handler.reset();
    }
    auto const end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(end - begin).count() / static_cast<double>(count);
}

template <int Trials>
double bestOf(std::size_t count) {
    double best = 0.0;
    for (int i = 0; i < Trials; ++i) {
        double const micros = destroyMicrosPerHandler(count);
        best = (i == 0) ? micros : std::min(best, micros);
    }
    return best;
}

int run(double maxRatio) {
    double const small = bestOf<kSmallTrials>(kSmall);
    double const large = bestOf<kLargeTrials>(kLarge);
    double const ratio = large / small;
    std::cout << std::format("destroy, {:>6} live handlers : {:8.2f} us per handler (best of {})\n", kSmall, small,
                             kSmallTrials)
              << std::format("destroy, {:>6} live handlers : {:8.2f} us per handler (best of {})\n", kLarge, large,
                             kLargeTrials)
              << std::format("ratio large / small           : {:.2f} (linear total cost = 1, quadratic = {:.0f})\n",
                             ratio, static_cast<double>(kLarge) / static_cast<double>(kSmall));
    if (maxRatio > 0.0) {
        if (ratio > maxRatio) {
            std::cout << std::format(
                "FAIL: per-handler destroy cost grows {:.2f}x from {} to {} handlers, exceeds "
                "--max-ratio={:.2f}\n",
                ratio, kSmall, kLarge, maxRatio);
            return 1;
        }
        std::cout << std::format("ok: ratio {:.2f} within --max-ratio={:.2f}\n", ratio, maxRatio);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        double maxRatio = 0.0;
        std::span<char*> const args{argv, static_cast<std::size_t>(argc)};
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string_view const arg{args[i]};
            if (arg.starts_with("--max-ratio=")) {
                maxRatio = std::stod(std::string{arg.substr(std::string_view{"--max-ratio="}.size())});
            } else {
                std::cout << std::format("usage: morph_bench_teardown [--max-ratio=<large/small, e.g. {}>]\n",
                                         kDefaultMaxRatio);
                return 2;
            }
        }
        return run(maxRatio);
    } catch (const std::exception& exc) {
        std::cout << "FAIL: " << exc.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "FAIL: unknown exception\n";
        return 1;
    }
}
