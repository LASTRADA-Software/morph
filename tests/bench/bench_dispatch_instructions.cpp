// SPDX-License-Identifier: Apache-2.0

// User-space instructions retired per local `execute` round trip.
//
// An allocation count (`morph_bench_alloc`) is exact but blind to a cost that
// allocates nothing: a lock taken twice, a copy of a large capture, a scan that
// grew. Wall-clock time sees those and is too noisy to gate on a shared
// runner. A retired-instruction count sits between the two: it does not move
// with machine load or clock speed, and it moves with every extra instruction
// the dispatch path executes.
//
// The counter is Linux `perf_event_open` with `PERF_COUNT_HW_INSTRUCTIONS`,
// `exclude_kernel` and `inherit`. `inherit` only covers threads created after
// the counter is opened, so it is opened before the `ThreadPoolExecutor`
// exists; the pool's workers, where the action runs, are then counted too.
//
// **When the counter cannot be opened the run is reported as skipped, never as
// passed.** Containers, a `perf_event_paranoid` above 2 and non-Linux hosts all
// refuse it. The binary then prints the reason and exits with `kExitSkipped`,
// which `tests/bench/CMakeLists.txt` registers as ctest's skip return code, so
// a runner without counters shows "Skipped" rather than a green result that
// measured nothing.
//
// Method:
//   * `Bridge` over `LocalBackend` over a four-thread pool; the owner is a
//     serial executor whose `runUntil` sleeps on a condition variable. A
//     polling owner spends ~10x the instructions spinning, and the figure
//     would then measure the spin.
//   * The model holds one `double`; the action adds to it. The workload is
//     the smallest there is, so what remains is framework overhead.
//   * `kWarmup` calls are excluded; `kCalls` are counted, each waited out
//     before the next starts.
//   * `--baseline` also prints the instructions of a bare cross-thread round
//     trip (condition variable ping-pong, no morph), the floor a local call
//     cannot go under.
//
// The ceiling is `--budget=<instructions per call>`; the figure is a property
// of the compiler and standard library, so it is a per-configuration cache
// variable in `tests/bench/CMakeLists.txt` with its measurements beside it.
//
// Build: `-DMORPH_BUILD_LOAD_TESTS=ON`, target `morph_bench_instr`. See
// docs/spec/testing_strategy.md.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(__linux__)
    #include <linux/perf_event.h>
    #include <sys/ioctl.h>
    #include <sys/syscall.h>
    #include <unistd.h>

    #include <cerrno>
#endif

// External linkage so glaze's reflection can mangle the type names.
struct BenchInstrPing {
    double x = 0.0;
};
struct BenchInstrPong {
    double y = 0.0;
};
struct BenchInstrModel {
    double total = 0.0;
    BenchInstrPong execute(const BenchInstrPing& action) {
        total += action.x;
        return BenchInstrPong{.y = total};
    }
};

BRIDGE_REGISTER_MODEL(BenchInstrModel, "BenchInstr_Model")
BRIDGE_REGISTER_ACTION(BenchInstrModel, BenchInstrPing, "BenchInstr_Ping")

namespace {

/// Exit status meaning "no counter, nothing measured". ctest's
/// `SKIP_RETURN_CODE` is set to the same value.
constexpr int kExitSkipped = 77;
constexpr int kExitUsage = 2;

constexpr int kWarmup = 200;
constexpr int kCalls = 2000;
constexpr std::size_t kPoolThreads = 4;

#if defined(__linux__)

/// A user-space retired-instruction counter that follows threads created after
/// it is opened.
class InstructionCounter {
public:
    /// @return The counter, or the reason it could not be opened.
    static std::optional<InstructionCounter> open(std::string& why) {
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_INSTRUCTIONS;
        attr.disabled = 1;
        attr.inherit = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        long const fd = ::syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
        if (fd < 0) {
            why = std::format("perf_event_open(PERF_COUNT_HW_INSTRUCTIONS) failed: {}", std::strerror(errno));
            return std::nullopt;
        }
        return InstructionCounter{static_cast<int>(fd)};
    }

    InstructionCounter(InstructionCounter&& other) noexcept : _fd{std::exchange(other._fd, -1)} {}
    InstructionCounter& operator=(InstructionCounter&&) = delete;
    InstructionCounter(const InstructionCounter&) = delete;
    InstructionCounter& operator=(const InstructionCounter&) = delete;
    ~InstructionCounter() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    void start() const {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::ioctl(_fd, PERF_EVENT_IOC_RESET, 0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::ioctl(_fd, PERF_EVENT_IOC_ENABLE, 0);
    }

    /// @return Instructions retired since `start()`, over this thread and
    ///         every thread it spawned after the counter was opened.
    [[nodiscard]] std::uint64_t stop() const {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::ioctl(_fd, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t value = 0;
        if (::read(_fd, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) {
            return 0;
        }
        return value;
    }

private:
    explicit InstructionCounter(int fd) : _fd{fd} {}
    int _fd;
};

#else

class InstructionCounter {
public:
    static std::optional<InstructionCounter> open(std::string& why) {
        why = "hardware instruction counters are only read on Linux";
        return std::nullopt;
    }
    void start() const {}
    [[nodiscard]] std::uint64_t stop() const { return 0; }
};

#endif

// A serial executor the owning thread sleeps on. `runUntil` runs queued tasks
// on the calling thread, each inside the executor scope the bridge expects, and
// waits on a condition variable when there is nothing to run.
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

// Instructions per cross-thread round trip with no morph in it: one thread
// wakes another through a condition variable and waits to be woken back.
double baselinePerCall(InstructionCounter const& counter, int calls) {
    std::mutex mtx;
    std::condition_variable cv;
    int turn = 0;  // even: main's turn, odd: the worker's
    bool stop = false;
    std::thread worker{[&] {
        std::unique_lock lock{mtx};
        for (;;) {
            cv.wait(lock, [&] { return stop || turn % 2 == 1; });
            if (stop) {
                return;
            }
            ++turn;
            cv.notify_all();
        }
    }};
    auto const ping = [&] {
        std::unique_lock lock{mtx};
        ++turn;
        cv.notify_all();
        int const target = turn + 1;
        cv.wait(lock, [&] { return turn >= target; });
    };
    for (int i = 0; i < kWarmup; ++i) {
        ping();
    }
    counter.start();
    for (int i = 0; i < calls; ++i) {
        ping();
    }
    auto const total = counter.stop();
    {
        std::scoped_lock const lock{mtx};
        stop = true;
    }
    cv.notify_all();
    worker.join();
    return static_cast<double>(total) / static_cast<double>(calls);
}

int run(bool baseline, double budget) {
    std::string why;
    // Opened before any thread of this process's own is created: `inherit`
    // reaches only threads started after the counter exists.
    auto counter = InstructionCounter::open(why);
    if (!counter) {
        std::cout << "SKIP: no instruction counter, nothing was measured: " << why << "\n";
        return kExitSkipped;
    }

    ::morph::exec::ThreadPoolExecutor pool{kPoolThreads};
    SleepingOwner owner;
    double perCall = 0.0;
    {
        ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool), owner};
        ::morph::bridge::BridgeHandler<BenchInstrModel> handler{bridge, &owner};

        int settled = 0;  // touched only by the owner's tasks and this thread between them
        auto roundTrip = [&](int value) {
            int const target = settled + 1;
            handler.execute(BenchInstrPing{.x = static_cast<double>(value)})
                .then([&settled](const BenchInstrPong&) { ++settled; })
                .onError([&settled](const std::exception_ptr&) { ++settled; });
            owner.runUntil([&] { return settled >= target; });
        };
        for (int i = 0; i < kWarmup; ++i) {
            roundTrip(i);
        }
        counter->start();
        for (int i = 0; i < kCalls; ++i) {
            roundTrip(i);
        }
        auto const total = counter->stop();
        perCall = static_cast<double>(total) / static_cast<double>(kCalls);
        std::cout << std::format("local execute round-trips  : {}\n", kCalls)
                  << std::format("user instructions total    : {} ({:.0f} per call)\n", total, perCall);
    }
    if (baseline) {
        std::cout << std::format("bare cross-thread round trip: {:.0f} instructions per call\n",
                                 baselinePerCall(*counter, kCalls));
    }
    if (perCall <= 0.0) {
        std::cout << "SKIP: the counter read back zero; nothing was measured\n";
        return kExitSkipped;
    }
    if (budget > 0.0) {
        if (perCall > budget) {
            std::cout << std::format("FAIL: {:.0f} instructions per call exceeds --budget={:.0f}\n", perCall, budget);
            return 1;
        }
        std::cout << std::format("ok: {:.0f} instructions per call within --budget={:.0f}\n", perCall, budget);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool baseline = false;
        double budget = 0.0;
        std::span<char*> const args{argv, static_cast<std::size_t>(argc)};
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string_view const arg{args[i]};
            if (arg == "--baseline") {
                baseline = true;
            } else if (arg.starts_with("--budget=")) {
                budget = std::stod(std::string{arg.substr(std::string_view{"--budget="}.size())});
            } else {
                std::cout << "usage: morph_bench_instr [--baseline] [--budget=<instructions per call>]\n";
                return kExitUsage;
            }
        }
        return run(baseline, budget);
    } catch (const std::exception& exc) {
        std::cout << "FAIL: " << exc.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "FAIL: unknown exception\n";
        return 1;
    }
}
