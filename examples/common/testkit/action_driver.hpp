// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

/// @file
/// `SeededScript<Action>` -- the weighted action generator + per-burst
/// invariant hook `examples/TESTING.md`'s "Multi-client stress harness"
/// section names as rung 4's own obligation. Seed comes from
/// `MORPH_STRESS_SEED` if set (always printed on failure via a Catch2
/// `INFO`), otherwise a caller-supplied default -- so a CI failure is
/// reproducible by re-running with the same seed.

namespace morph::ladder::testkit {

template <typename Action>
class SeededScript {
  public:
    using Generator = std::function<Action()>;
    struct WeightedGenerator {
        int weight;
        Generator generate;
    };
    using OnBurst = std::function<void(const std::vector<Action>&)>;

    /// @param defaultSeed Used if `MORPH_STRESS_SEED` is unset.
    /// @param generators  Weighted action generators; a generator with
    ///        weight 2 is twice as likely to be picked as one with weight 1.
    /// @param burstSize   Number of `next()` calls between `onBurst` calls.
    /// @param onBurst     Invariant-check callback, called with every action
    ///        generated since the last call, once `burstSize` actions have
    ///        accumulated (and once more via `flushBurst()` for a partial
    ///        final burst).
    SeededScript(std::uint64_t defaultSeed, std::vector<WeightedGenerator> generators, std::size_t burstSize,
                 OnBurst onBurst)
        : _seed{resolveSeed(defaultSeed)},
          _rng{_seed},
          _generators{std::move(generators)},
          _burstSize{burstSize},
          _onBurst{std::move(onBurst)} {
        INFO("MORPH_STRESS_SEED=" << _seed);
        int totalWeight = 0;
        for (const auto& g : _generators) {
            totalWeight += g.weight;
        }
        _totalWeight = totalWeight;
    }

    /// @brief Generates the next action, picking a generator by weight.
    [[nodiscard]] Action next() {
        std::uniform_int_distribution<int> dist{0, _totalWeight - 1};
        int pick = dist(_rng);
        // Walk the first N-1 weight ranges only; the last generator absorbs
        // whatever weight is left over. `pick` is drawn from
        // [0, _totalWeight) and the weights sum to `_totalWeight`, so
        // running off the end of this loop *is* the "last generator won"
        // case, not an error. Selecting it that way (rather than testing
        // every generator and falling through to a post-loop
        // `_generators.front()`) leaves no unreachable statement behind:
        // the old fallback could not be reached by any input, so llvm-cov
        // scored it as a permanently-missed line, and the loop's own
        // "condition went false" arm was equally unreachable because some
        // generator always matched first.
        std::size_t chosen = _generators.size() - 1;
        for (std::size_t i = 0; i + 1 < _generators.size(); ++i) {
            if (pick < _generators[i].weight) {
                chosen = i;
                break;
            }
            pick -= _generators[i].weight;
        }
        Action action = _generators[chosen].generate();
        _burst.push_back(action);
        if (_burst.size() >= _burstSize) {
            _onBurst(_burst);
            _burst.clear();
        }
        return action;
    }

    /// @brief Calls `onBurst` with whatever partial burst remains, then
    ///        clears it. Call once at the end of a script run so a final
    ///        partial burst still gets its invariant check.
    void flushBurst() {
        if (!_burst.empty()) {
            _onBurst(_burst);
            _burst.clear();
        }
    }

    /// @return The seed this run used (for logging).
    [[nodiscard]] std::uint64_t seed() const noexcept { return _seed; }

  private:
    [[nodiscard]] static std::uint64_t resolveSeed(std::uint64_t defaultSeed) {
        if (const char* env = std::getenv("MORPH_STRESS_SEED"); env != nullptr && *env != '\0') {
            return std::stoull(env);
        }
        return defaultSeed;
    }

    std::uint64_t _seed;
    std::mt19937_64 _rng;
    std::vector<WeightedGenerator> _generators;
    int _totalWeight = 0;
    std::size_t _burstSize;
    OnBurst _onBurst;
    std::vector<Action> _burst;
};

}  // namespace morph::ladder::testkit
