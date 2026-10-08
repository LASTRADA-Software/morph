// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/detail/owner_affinity.hpp"
#include "../../core/detail/owner_probe.hpp"
#include "../../core/executor.hpp"

/// @file
/// @brief The reactive graph's internals: node colours and links, the tracking
///        frame, batching and the flush queue.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive::detail {

/// @brief The site names a misuse is reported under through `exec::detail::noteOwner`.
namespace site {
/// @brief An operation ran off the Runtime's owner; it is dropped.
inline constexpr char const* kOffOwner = "morph::reactive: off the Runtime's owner";
/// @brief An exception escaped an Effect; the flush stops.
inline constexpr char const* kEffectThrew = "morph::reactive: an exception escaped an Effect";
/// @brief An Effect re-ran more than `maxEffectRunsPerFlush` times in one flush; the flush stops.
inline constexpr char const* kWriteCycle = "morph::reactive: an Effect re-ran past maxEffectRunsPerFlush";
/// @brief A signal was written while a Computed was computing; the write is dropped.
inline constexpr char const* kSetInComputed = "morph::reactive: set() inside a Computed";
/// @brief A Computed read itself; the read throws `std::logic_error`.
inline constexpr char const* kComputedReadsItself = "morph::reactive: a Computed read itself";
/// @brief `Store::send` was called from inside an update; the message is dropped.
inline constexpr char const* kSendInUpdate = "morph::reactive: send() inside an update";
/// @brief `Query::refetch` or `Mutation::run` was called while a Computed was computing; nothing is issued.
inline constexpr char const* kIssueInComputed =
    "morph::reactive: Query::refetch() or Mutation::run() inside a Computed";
/// @brief An exception escaped `RuntimeOptions::afterFlush`; it is dropped.
inline constexpr char const* kAfterFlushThrew = "morph::reactive: an exception escaped afterFlush";
/// @brief A Runtime was destroyed while nodes made from it were alive.
inline constexpr char const* kRuntimeOutlived = "morph::reactive: Runtime destroyed while nodes are alive";
/// @brief A control node was built over a handler whose callbacks are not delivered on the Runtime's owner.
inline constexpr char const* kHandlerExecutor = "morph::reactive: a handler delivers off the Runtime's owner";
}  // namespace site

/// @brief How many times one Effect may run in a single flush unless `RuntimeOptions` says otherwise.
inline constexpr std::size_t kDefaultMaxEffectRunsPerFlush = 100;

/// @brief How many flushes in a row are re-posted after an Effect threw, unless `RuntimeOptions` says otherwise.
inline constexpr std::size_t kDefaultMaxThrowReposts = 3;

/// @brief Whether `==` is usable for @p T: declared, and its element types usable too.
///
/// `std::equality_comparable` alone is not enough. The standard containers declare an unconstrained
/// `operator==`, so it holds for a container whose element has no `==`, and comparing two of them then fails
/// to compile. So a range is checked through its element type, and `std::optional`, `std::pair`, `std::tuple`
/// and `std::variant` through theirs.
/// @tparam T The value type, without cv-qualifiers.
template <typename T>
struct EqualityUsable : std::bool_constant<std::equality_comparable<T>> {};

/// @brief A range is usable when it declares `==` and its element type is usable. A range whose element is
///        itself (`std::filesystem::path`) is checked as a plain type.
/// @tparam T The range type.
template <typename T>
    requires std::ranges::range<T const> && (!std::same_as<std::remove_cv_t<std::ranges::range_value_t<T const>>, T>)
struct EqualityUsable<T>
    : std::bool_constant<std::equality_comparable<T> &&
                         EqualityUsable<std::remove_cv_t<std::ranges::range_value_t<T const>>>::value> {};

/// @brief An optional is usable when its value type is.
/// @tparam T The value type.
template <typename T>
struct EqualityUsable<std::optional<T>> : EqualityUsable<std::remove_cv_t<T>> {};

/// @brief A pair is usable when both its types are.
/// @tparam First The first type.
/// @tparam Second The second type.
template <typename First, typename Second>
struct EqualityUsable<std::pair<First, Second>> : std::bool_constant<EqualityUsable<std::remove_cv_t<First>>::value &&
                                                                     EqualityUsable<std::remove_cv_t<Second>>::value> {
};

/// @brief A tuple is usable when every element type is.
/// @tparam Ts The element types.
template <typename... Ts>
struct EqualityUsable<std::tuple<Ts...>> : std::bool_constant<(EqualityUsable<std::remove_cv_t<Ts>>::value && ...)> {};

/// @brief A variant is usable when every alternative is.
/// @tparam Ts The alternatives.
template <typename... Ts>
struct EqualityUsable<std::variant<Ts...>> : std::bool_constant<(EqualityUsable<std::remove_cv_t<Ts>>::value && ...)> {
};

/// @brief The bounds a flush keeps, as `RuntimeOptions` sets them.
struct FlushBounds {
    /// @brief How many times one Effect may run in a single flush before the flush is a write cycle.
    std::size_t maxEffectRunsPerFlush = kDefaultMaxEffectRunsPerFlush;
    /// @brief How many flushes in a row are re-posted after an Effect threw.
    std::size_t maxThrowReposts = kDefaultMaxThrowReposts;
};

/// @brief A node's freshness. Ordered: a later enumerator is staler.
enum class Colour : std::uint8_t {
    Clean,  ///< Up to date.
    Check,  ///< A transitive source changed: pull the sources before deciding to recompute.
    Dirty,  ///< A direct source changed: recompute.
};

class Node;
class TrackingFrame;

/// @brief State shared by one `Runtime` and every node made from it.
///
/// Nodes hold it by `shared_ptr`, so a node that outlives its `Runtime` (a
/// reported misuse) still has a valid core. The posted flush holds it weakly
/// and checks `detach()`, so a flush after the `Runtime` is gone does nothing.
class RuntimeCore : public std::enable_shared_from_this<RuntimeCore> {
public:
    /// @param owner The executor every operation must run on, and the one flushes are posted to. It must
    ///        queue what is posted and run it later, in order: an executor that runs a post inline would
    ///        run Effects inside `set()`. Borrowed: it must outlive the core.
    explicit RuntimeCore(exec::IExecutor& owner MORPH_LIFETIMEBOUND) noexcept : _affinity{owner} {}

    ~RuntimeCore() = default;
    RuntimeCore(RuntimeCore const&) = delete;
    RuntimeCore& operator=(RuntimeCore const&) = delete;
    RuntimeCore(RuntimeCore&&) = delete;
    RuntimeCore& operator=(RuntimeCore&&) = delete;

    /// @brief Whether the calling thread is on the owner, without reporting anything.
    /// @return What `checkOwner()` returns, minus the report.
    [[nodiscard]] bool onOwner() const noexcept { return _affinity.here(); }

    /// @brief Whether the calling thread is on the owner, reporting `site::kOffOwner` when it is not.
    /// @return True on the owner: inside one of its tasks, or anywhere on the thread that built the Runtime
    ///         when that thread was running no executor's task at the time (a Qt slot, `main()`, a test
    ///         body). That second half is a per-thread check: on that thread a task of another executor
    ///         passes too, so running a different executor's tasks there is not detected.
    [[nodiscard]] bool checkOwner() const noexcept {
        if (_affinity.here()) {
            return true;
        }
        report(site::kOffOwner);
        return false;
    }

    /// @brief Reports a misuse: asserts in a debug build, or hands it to an installed owner probe.
    /// @param where One of the `site::` constants.
    void report(char const* where) const noexcept {
        exec::detail::noteOwner(where, _affinity.owner().coreExecutor(), false);
    }

    /// @brief The owner executor.
    /// @return The executor passed at construction.
    [[nodiscard]] exec::IExecutor& owner() const noexcept { return _affinity.owner(); }

    /// @brief Sets the flush bounds and the after-flush hook.
    /// @param bounds How often one Effect may run in a flush, and how many flushes in a row are re-posted after
    ///        an Effect threw.
    /// @param afterFlush Called after every flush that processed a queued Effect; may be empty.
    void configure(FlushBounds bounds, std::function<void()> afterFlush) {
        _bounds = bounds;
        _afterFlush = std::move(afterFlush);
    }

    /// @brief Opens a batch; batches nest.
    void beginBatch() noexcept { ++_batchDepth; }

    /// @brief Closes a batch; closing the outermost one requests a flush when effects are queued.
    ///
    /// The outermost batch closing outside a flush is a write from outside the graph, which starts a new count
    /// of the re-posts after a throwing Effect.
    void endBatch() {
        if (--_batchDepth == 0) {
            if (!_flushing) {
                _throwReposts = 0;
            }
            requestFlush();
        }
    }

    /// @brief Queues an Effect that turned stale. Defined below `Node`.
    /// @param effect The Effect node.
    void enqueue(Node& effect);

    /// @brief Posts one flush to the owner, unless one is already requested or running, nothing is
    ///        queued, or the Runtime is gone.
    void requestFlush() {
        if (_flushing || _isFlushRequested || _queue.empty() || _detached) {
            return;
        }
        // Set before posting, so the flag is already down again if the flush runs inside post().
        _isFlushRequested = true;
        try {
            _affinity.owner().post([weak = weak_from_this()] {
                if (auto const core = weak.lock()) {
                    core->flush();
                }
            });
        } catch (...) {
            // Nothing was posted: the queue is kept, and the next write posts again.
            _isFlushRequested = false;
            throw;
        }
    }

    /// @brief Runs every queued Effect that a source change really affects. Defined below `Node`.
    void flush();

    /// @brief Removes every reference the core holds to a node being destroyed. Defined below `Node`.
    /// @param node The node.
    void forget(Node& node) noexcept;

    /// @brief The innermost tracking frame, or null outside every tracked run.
    /// @return The frame reads register with.
    [[nodiscard]] TrackingFrame* tracking() const noexcept { return _tracking; }

    /// @brief Installs the innermost tracking frame.
    /// @param frame The frame, or null.
    void setTracking(TrackingFrame* frame) noexcept { _tracking = frame; }

    /// @brief Whether a Computed is computing on this runtime.
    /// @return True between `enterComputed()` and the matching `leaveComputed()`.
    [[nodiscard]] bool isComputing() const noexcept { return _computing != 0; }

    /// @brief Marks a Computed as computing.
    void enterComputed() noexcept { ++_computing; }

    /// @brief Marks a Computed as done computing.
    void leaveComputed() noexcept { --_computing; }

    /// @brief Marks the start of a widget callback.
    void enterWidgetEvent() noexcept { ++_widgetEventDepth; }

    /// @brief Marks the end of a widget callback.
    void leaveWidgetEvent() noexcept { --_widgetEventDepth; }

    /// @brief How deeply batches are nested on the owner.
    /// @return Zero outside every batch.
    [[nodiscard]] std::size_t batchDepth() const noexcept { return _batchDepth; }

    /// @brief Whether a widget callback is running on the owner.
    /// @return True between `enterWidgetEvent()` and the matching `leaveWidgetEvent()`.
    [[nodiscard]] bool isInWidgetEvent() const noexcept { return _widgetEventDepth != 0; }

    /// @brief Whether a flush is posted and has not run yet.
    /// @return The coalescing flag.
    [[nodiscard]] bool isFlushRequested() const noexcept { return _isFlushRequested; }

    /// @brief Marks the Runtime as gone: flushes do nothing from now on.
    void detach() noexcept { _detached = true; }

    /// @brief Counts a node made from this runtime.
    /// @return The node's creation order: greater than that of every node made from this runtime before it.
    [[nodiscard]] std::uint64_t nodeCreated() noexcept {
        ++_liveNodes;
        return ++_createdNodes;
    }

    /// @brief Counts a node destroyed.
    void nodeDestroyed() noexcept { --_liveNodes; }

    /// @brief How many nodes made from this runtime are alive.
    /// @return The live-node count.
    [[nodiscard]] std::size_t liveNodes() const noexcept { return _liveNodes; }

private:
    void dropQueue() noexcept;

    exec::detail::OwnerAffinity _affinity;
    // A binary heap ordered by depth, then creation, the shallowest and oldest on top: see `flush()`.
    std::vector<Node*> _queue;
    std::function<void()> _afterFlush;
    TrackingFrame* _tracking = nullptr;
    FlushBounds _bounds;
    // Flushes re-posted in a row after an Effect threw.
    std::size_t _throwReposts = 0;
    std::size_t _batchDepth = 0;
    std::size_t _computing = 0;
    std::size_t _widgetEventDepth = 0;
    std::size_t _liveNodes = 0;
    std::uint64_t _createdNodes = 0;
    std::uint64_t _flushEpoch = 0;
    bool _isFlushRequested = false;
    bool _flushing = false;
    bool _detached = false;
};

/// @brief Collects the sources one tracked run reads, and restores the previous frame when it ends.
///
/// A frame whose observer is null is an untracked region: reads inside it subscribe nothing.
class TrackingFrame {
public:
    /// @param core The runtime core.
    /// @param observer The node whose run this is, or null for an untracked region.
    TrackingFrame(RuntimeCore& core, Node* observer) noexcept
        : _core{&core}, _previous{core.tracking()}, _observer{observer} {
        core.setTracking(this);
    }

    /// @brief Reinstalls the enclosing frame.
    ~TrackingFrame() { _core->setTracking(_previous); }
    TrackingFrame(TrackingFrame const&) = delete;
    TrackingFrame& operator=(TrackingFrame const&) = delete;
    TrackingFrame(TrackingFrame&&) = delete;
    TrackingFrame& operator=(TrackingFrame&&) = delete;

    /// @brief Records that the run read @p source; a no-op in an untracked region.
    /// @param source The node read.
    void add(Node& source) {
        if (_observer != nullptr && std::ranges::find(_sources, &source) == _sources.end()) {
            _sources.push_back(&source);
        }
    }

    /// @brief Drops @p node, which is being destroyed, from the recorded sources. When @p node is this
    ///        frame's observer, the frame stops recording and remembers that its observer is gone.
    /// @param node The node.
    void forget(Node& node) noexcept {
        std::erase(_sources, &node);
        if (&node == _observer) {
            _observer = nullptr;
            _observerDestroyed = true;
        }
    }

    /// @brief Whether the observer was destroyed during the run: a body may destroy its own node (a
    ///        remount inside an Effect), and the node's run must not touch it afterwards.
    /// @return True once the observer's destructor has run.
    [[nodiscard]] bool observerDestroyed() const noexcept { return _observerDestroyed; }

    /// @brief Hands the recorded sources to the observer.
    /// @return The sources, deduplicated, in first-read order.
    [[nodiscard]] std::vector<Node*> takeSources() noexcept { return std::move(_sources); }

    /// @brief The enclosing frame.
    /// @return The frame that was innermost when this one was opened, or null.
    [[nodiscard]] TrackingFrame* previous() const noexcept { return _previous; }

private:
    RuntimeCore* _core;
    TrackingFrame* _previous;
    Node* _observer;
    std::vector<Node*> _sources;
    bool _observerDestroyed = false;
};

/// @brief A graph node: a source (Signal), an observer (Effect), or both (Computed).
///
/// Non-copyable and non-movable: other nodes hold raw pointers to it, and its
/// destructor unlinks it in both directions. A derived node supplies `recompute()`
/// and `isEffect()` as private overrides; only `Node` calls them.
///
/// A node may be destroyed while it is being brought up to date (an Effect body
/// that remounts the view owning it). The destructor tells the running pull and
/// the running tracking frame, and neither touches the node again.
class Node {
public:
    /// @brief Constructs a node. Off the owner this is reported (`site::kOffOwner`) and the node is
    ///        not counted towards the core's live nodes, whose counter belongs to the owner.
    /// @param core The runtime core this node belongs to.
    /// @param depth How deeply the scope owning this node is nested; queued Effects run shallowest first.
    explicit Node(std::shared_ptr<RuntimeCore> core, std::size_t depth = 0)
        : _core{std::move(core)}, _depth{depth}, _bornOnOwner{_core->checkOwner()} {
        if (_bornOnOwner) {
            _creationOrder = _core->nodeCreated();
        }
    }

    /// @brief Unlinks this node from its sources and observers, drops every reference the core holds
    ///        to it, and tells a pull running on it that it is gone.
    ///
    /// Destroying a node off the owner is a misuse. It is reported (`site::kOffOwner`) but cannot be
    /// refused: the destructor still edits the owner's graph, queue and counters, and what that does
    /// beyond the report is undefined.
    virtual ~Node() {
        static_cast<void>(_core->checkOwner());
        if (_destroyedWatch != nullptr) {
            *_destroyedWatch = true;
        }
        for (Node* const source : _sources) {
            std::erase(source->_observers, this);
        }
        for (Node* const observer : _observers) {
            std::erase(observer->_sources, this);
        }
        _core->forget(*this);
        if (_bornOnOwner) {
            _core->nodeDestroyed();
        }
    }

    Node(Node const&) = delete;
    Node& operator=(Node const&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;

    /// @brief Push phase: raises this node to @p colour and marks its observers Check.
    ///
    /// An Effect is queued when it leaves Clean. A node already at least as stale stops the walk, because
    /// its observers were marked when it turned stale, unless an observer has since ended Clean above it
    /// (`exposeStaleSources()`); then the walk passes through it once.
    /// @param colour Dirty for a direct observer of a change, Check for a transitive one.
    // NOLINTNEXTLINE(misc-no-recursion) -- the recursion depth is the graph's depth
    void markStale(Colour colour) {
        if (_colour >= colour && !_staleAboveClean) {
            return;
        }
        if (_colour < colour) {
            if (_colour == Colour::Clean && isEffect()) {
                // Queued before the colour changes: an enqueue that fails to allocate leaves the node
                // Clean, so a later walk that reaches it queues it again.
                _core->enqueue(*this);
            }
            _colour = colour;
        }
        // Cleared before the walk: two flagged nodes can observe each other (a cycle a refused self-read
        // leaves linked), and a revisit must stop here. Restored if the walk throws, so the next write
        // passes through again; a node this walk raised from Clean is not flagged, and an Effect behind
        // it whose enqueue failed is not reached again.
        bool const flagged = std::exchange(_staleAboveClean, false);
        try {
            for (Node* const observer : _observers) {
                observer->markStale(Colour::Check);
            }
        } catch (...) {
            _staleAboveClean = flagged;
            throw;
        }
    }

    /// @brief Pull phase: brings this node up to date, recomputing only if a source really changed.
    ///
    /// A pull that throws leaves the node Clean (so later changes still reach it) and rethrows. A pull
    /// during which the node is destroyed returns without touching it.
    /// @return False when the node was destroyed during the pull: the caller must not touch it either.
    // NOLINTNEXTLINE(misc-no-recursion) -- the recursion depth is the graph's depth
    bool updateIfNecessary() {
        bool destroyed = false;
        bool* const enclosing = std::exchange(_destroyedWatch, &destroyed);
        try {
            pull(destroyed);
        } catch (...) {
            endPull(destroyed, enclosing);
            throw;
        }
        endPull(destroyed, enclosing);
        return !destroyed;
    }

    /// @brief Counts one run of this Effect in the flush numbered @p epoch.
    /// @param epoch The current flush's number.
    /// @return How many times it has run in that flush, this one included.
    [[nodiscard]] std::size_t countRun(std::uint64_t epoch) noexcept {
        if (_epoch != epoch) {
            _epoch = epoch;
            _runs = 0;
        }
        return ++_runs;
    }

    /// @brief How deeply the scope owning this node is nested; the flush runs shallower Effects first.
    /// @return Zero for a node no scope owns.
    [[nodiscard]] std::size_t depth() const noexcept { return _depth; }

    /// @brief Whether this node runs after @p other when both are queued: it is deeper, or as deep and newer.
    /// @param other The other node.
    /// @return The flush queue's ordering.
    [[nodiscard]] bool runsAfter(Node const& other) const noexcept {
        return std::pair{_depth, _creationOrder} > std::pair{other._depth, other._creationOrder};
    }

    /// @brief Where this node stands in its runtime's creation order; among Effects of one depth, the flush
    ///        runs older ones first.
    /// @return Greater than that of every node made from the same runtime before it; zero for a node
    ///         constructed off the owner, which is never queued.
    [[nodiscard]] std::uint64_t creationOrder() const noexcept { return _creationOrder; }

    /// @brief Marks this node Clean without running it (a dropped queue entry).
    void settle() noexcept {
        _colour = Colour::Clean;
        exposeStaleSources();
    }

protected:
    /// @brief The runtime core.
    /// @return The core this node belongs to.
    [[nodiscard]] RuntimeCore& core() const noexcept { return *_core; }

    /// @brief Whether this node was constructed on the owner; one that was not is refused its work.
    /// @return The owner check the constructor made.
    [[nodiscard]] bool bornOnOwner() const noexcept { return _bornOnOwner; }

    /// @brief Registers this node as a source of the innermost tracked run, if any.
    void trackRead() const {
        if (TrackingFrame* const frame = _core->tracking(); frame != nullptr) {
            // Reading is logically const; the dependency link is bookkeeping.
            frame->add(const_cast<Node&>(*this));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        }
    }

    /// @brief Marks this node Dirty, so the next pull recomputes it.
    void forceDirty() noexcept { _colour = Colour::Dirty; }

    /// @brief Whether a pull is bringing this node up to date right now. A read of the node from inside
    ///        that pull can only come from its own run or from a source's run that reads it: a cycle.
    /// @return True between the start and the end of `updateIfNecessary()` on this node.
    [[nodiscard]] bool isPulling() const noexcept { return _destroyedWatch != nullptr; }

    /// @brief Marks every observer of this node @p colour.
    /// @param colour The colour to raise them to.
    void markObserversStale(Colour colour) {
        for (Node* const observer : _observers) {
            observer->markStale(colour);
        }
    }

    /// @brief Replaces this node's sources with @p fresh, unlinking the ones no longer read.
    ///
    /// Strong guarantee: linking the new sources is the only step that allocates, and it runs first;
    /// if it throws, the links it made are undone, so no source keeps a link this node does not list.
    /// @param fresh The sources the latest run read.
    void adoptSources(std::vector<Node*> fresh) {
        std::size_t linked = 0;
        try {
            for (Node* const source : fresh) {
                if (std::ranges::find(_sources, source) == _sources.end()) {
                    source->_observers.push_back(this);
                }
                ++linked;
            }
        } catch (...) {
            for (Node* const source : std::span{fresh}.first(linked)) {
                if (std::ranges::find(_sources, source) == _sources.end()) {
                    std::erase(source->_observers, this);
                }
            }
            throw;
        }
        for (Node* const old : _sources) {
            if (std::ranges::find(fresh, old) == fresh.end()) {
                std::erase(old->_observers, this);
            }
        }
        _sources = std::move(fresh);
    }

    /// @brief Adds @p fresh to this node's sources without dropping any: what a failed run keeps.
    /// @param fresh The sources the failed run read before it threw.
    void mergeSources(std::vector<Node*> fresh) {
        std::vector<Node*> kept;
        std::ranges::copy_if(_sources, std::back_inserter(kept),
                             [&fresh](Node const* old) { return std::ranges::find(fresh, old) == fresh.end(); });
        fresh.insert(fresh.end(), kept.begin(), kept.end());
        adoptSources(std::move(fresh));
    }

private:
    /// @brief Brings the node's value up to date.
    /// @return True when observers must treat the value as changed.
    virtual bool recompute() = 0;

    /// @brief Whether this node is an Effect, which the flush queue runs.
    /// @return False unless overridden.
    [[nodiscard]] virtual bool isEffect() const noexcept { return false; }

    // Returns as soon as `destroyed` is set: from then on `this` is gone.
    // NOLINTNEXTLINE(misc-no-recursion) -- the recursion depth is the graph's depth
    void pull(bool const& destroyed) {
        if (_colour == Colour::Check) {
            for (std::size_t i = 0; i < _sources.size();) {
                Node* const source = _sources.at(i);
                source->updateIfNecessary();
                if (destroyed) {
                    return;
                }
                if (_colour == Colour::Dirty) {
                    break;
                }
                // A source destroyed by its own pull has unlinked itself, and the next one now sits at i.
                if (i < _sources.size() && _sources.at(i) == source) {
                    ++i;
                }
            }
        }
        if (_colour == Colour::Dirty) {
            bool const changed = recompute();
            if (destroyed) {
                return;
            }
            if (changed) {
                markObserversStale(Colour::Dirty);
            }
        }
    }

    void endPull(bool destroyed, bool* enclosing) noexcept {
        if (destroyed) {
            if (enclosing != nullptr) {
                *enclosing = true;
            }
            return;
        }
        _destroyedWatch = enclosing;
        _colour = Colour::Clean;
        _staleAboveClean = false;
        exposeStaleSources();
    }

    // This node is Clean while a source may still be stale: an Effect settled by a write cycle without
    // running, an Effect whose run wrote a source of a Computed it had read (the write cannot mark the
    // running Effect), or a run that threw before reading every source. A stale source stops the next
    // write's walk, so this node would never hear of it. Each stale source is flagged, so that walk
    // passes through it once, and its own sources are visited the same way; the visit stops at a Clean
    // source, whose walk passes anyway, and at one already flagged, whose sources were visited when it
    // was flagged. Colours are kept, so the next pull still recomputes exactly what changed.
    // NOLINTNEXTLINE(misc-no-recursion) -- the recursion depth is the graph's depth
    void exposeStaleSources() noexcept {
        for (Node* const source : _sources) {
            if (source->_colour != Colour::Clean && !source->_staleAboveClean) {
                source->_staleAboveClean = true;
                source->exposeStaleSources();
            }
        }
    }

    std::shared_ptr<RuntimeCore> _core;
    std::vector<Node*> _sources;
    std::vector<Node*> _observers;
    // Points at the running pull's flag while one runs on this node.
    bool* _destroyedWatch = nullptr;
    std::uint64_t _epoch = 0;
    std::uint64_t _creationOrder = 0;
    std::size_t _depth;
    std::size_t _runs = 0;
    Colour _colour = Colour::Clean;
    // Stale, with an observer that may be Clean: the next markStale walks through instead of stopping.
    bool _staleAboveClean = false;
    bool _bornOnOwner;
};

/// @brief The flush queue's heap order: the node that runs first is on top.
/// @param lhs A queued node.
/// @param rhs Another queued node.
/// @return Whether @p lhs runs after @p rhs.
inline bool runsLater(Node const* lhs, Node const* rhs) noexcept { return lhs->runsAfter(*rhs); }

inline void RuntimeCore::enqueue(Node& effect) {
    _queue.push_back(&effect);
    std::ranges::push_heap(_queue, runsLater);
}

// Queued Effects run shallowest scope first, then oldest first. An owner's Effects therefore run before those
// of the scopes it owns, and an Effect a run creates is newer than the run's own Effect: either way a parent
// runs before every child it mounted, and unmounts a child before that child can see the state the parent
// is leaving, whatever order the writes came in. Each run is pulled, so the order changes nothing a run
// reads: only which Effects still exist to run.
inline void RuntimeCore::flush() {
    _isFlushRequested = false;
    if (_detached || _queue.empty()) {
        return;
    }
    if (_widgetEventDepth != 0) {
        // A nested event loop inside a widget handler (a modal dialog) ran this flush. A remount must not
        // destroy the widget whose handler is on the stack, so the queue waits: the widget event's batch is
        // still open, and closing it, at the end of the outermost widget event, posts the flush again. Not
        // re-posting here keeps the nested loop from spinning on it.
        return;
    }
    _flushing = true;
    ++_flushEpoch;
    bool stopped = false;
    while (!_queue.empty()) {
        std::ranges::pop_heap(_queue, runsLater);
        Node* const effect = _queue.back();
        _queue.pop_back();
        if (effect->countRun(_flushEpoch) > _bounds.maxEffectRunsPerFlush) {
            report(site::kWriteCycle);
            effect->settle();
            dropQueue();
            break;
        }
        try {
            effect->updateIfNecessary();
        } catch (...) {
            report(site::kEffectThrew);
            stopped = true;
            break;
        }
    }
    _flushing = false;
    if (!stopped) {
        _throwReposts = 0;
    } else if (_throwReposts < _bounds.maxThrowReposts) {
        // What is still queued runs in a flush of its own, so a defect does not stall it until the next write;
        // a bounded number of times in a row, so an Effect that throws on every run cannot loop for ever. Past
        // the bound the queue waits for the next write.
        ++_throwReposts;
        requestFlush();
    }
    if (_afterFlush) {
        // The hook runs as a task of the owner executor, which has no use for the exception; the flush is
        // already complete, so reporting it loses nothing.
        try {
            _afterFlush();
        } catch (...) {
            report(site::kAfterFlushThrew);
        }
    }
}

inline void RuntimeCore::forget(Node& node) noexcept {
    if (std::erase(_queue, &node) != 0) {
        std::ranges::make_heap(_queue, runsLater);
    }
    for (TrackingFrame* frame = _tracking; frame != nullptr; frame = frame->previous()) {
        frame->forget(node);
    }
}

inline void RuntimeCore::dropQueue() noexcept {
    for (Node* const node : _queue) {
        node->settle();
    }
    _queue.clear();
}

/// @brief Opens a batch for its lifetime.
class BatchScope {
public:
    /// @param core The runtime core.
    explicit BatchScope(RuntimeCore& core) noexcept : _core{&core} { core.beginBatch(); }
    /// @brief Closes the batch; closing the outermost one requests a flush when effects are queued.
    ~BatchScope() {
        try {
            _core->endBatch();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Posting the flush failed to allocate. The queue is kept and the next write posts again.
        }
    }
    BatchScope(BatchScope const&) = delete;
    BatchScope& operator=(BatchScope const&) = delete;
    BatchScope(BatchScope&&) = delete;
    BatchScope& operator=(BatchScope&&) = delete;

private:
    RuntimeCore* _core;
};

/// @brief Marks a widget callback for its lifetime.
class WidgetEventScope {
public:
    /// @param core The runtime core.
    explicit WidgetEventScope(RuntimeCore& core) noexcept : _core{&core} { core.enterWidgetEvent(); }
    /// @brief Marks the end of the widget callback.
    ~WidgetEventScope() { _core->leaveWidgetEvent(); }
    WidgetEventScope(WidgetEventScope const&) = delete;
    WidgetEventScope& operator=(WidgetEventScope const&) = delete;
    WidgetEventScope(WidgetEventScope&&) = delete;
    WidgetEventScope& operator=(WidgetEventScope&&) = delete;

private:
    RuntimeCore* _core;
};

}  // namespace morph::reactive::detail
