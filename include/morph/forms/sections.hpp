// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/sections.hpp
/// @brief Unordered sections: N independently editable action drafts on one
///        screen, each with its own readiness gate and no sequencing.
///
/// The sibling to `morph::flows::FlowSession`. A wizard has one active step and
/// throws on a field belonging to any other; a section set has no active
/// section at all -- every declared section is editable at every moment, and
/// each dispatches on its own as soon as its draft validates.
///
/// Choose this when order carries no meaning (tabs, cards, a settings page) and
/// `FlowSession` when it does. Like flows, this is additive metadata and
/// dispatch bookkeeping over the ordinary `BridgeHandler<Model>::execute<A>()`
/// path: no new wire format, no new execution mode.
/// See docs/spec/forms/sections.md.

#include <cstddef>
#include <exception>
#include <functional>
#include <glaze/glaze.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../attributes.hpp"
#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/logger.hpp"
#include "detail/session_common.hpp"

namespace morph::forms {

/// @brief One section of a `SectionGroup`: a registered action, a display
///        title, and zero or more `Bind` prefill declarations.
/// @tparam Action Registered action type (`BRIDGE_REGISTER_ACTION`) this section fires.
/// @tparam Title  Human title for the section (tab label / card header).
/// @tparam Binds  Zero or more `Bind<Field, Path>` prefill declarations.
template <typename Action, morph::forms::FixedString Title, typename... Binds>
struct Section {
    /// @brief The section's action type.
    using action = Action;

    /// @brief Tuple of this section's `Bind<...>` prefill declarations (possibly empty).
    using binds = std::tuple<Binds...>;

    /// @brief The section's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief An unordered set of `Section`s sharing one screen.
/// @tparam Title    Human title for the whole group.
/// @tparam Sections One or more `Section<Action, Title, Binds...>` types.
template <morph::forms::FixedString Title, typename... Sections>
struct SectionGroup {
    /// @brief Tuple of this group's `Section<...>` types.
    using sections = std::tuple<Sections...>;

    /// @brief The group's display title.
    /// @return The declared title.
    [[nodiscard]] static constexpr std::string_view title() noexcept { return Title.view(); }
};

/// @brief Traits specialisation mapping a `SectionGroup` type to its string type-id.
///
/// Specialise via `BRIDGE_REGISTER_SECTION_GROUP` rather than by hand. The
/// default is a forward declaration — using it without a specialisation is an
/// incomplete-type error. The schema needs a stable name for the group;
/// deriving one from the C++ type would tie the wire format to a mangled name.
/// @tparam G Concrete `SectionGroup<...>` type.
template <typename G>
struct SectionGroupTraits;  // forward — specialise or use BRIDGE_REGISTER_SECTION_GROUP

/// @brief Generates the `s-*` JSON document for section group @p G.
///
/// Emits `s-id`, `s-title`, and an `s-sections` array; each entry carries
/// `action` (the section's registered action type-id), `title`, and — only when
/// the section declares at least one `Bind` — a `prefill` object mapping field
/// name to `"<ActionTypeId>.<field>"` path.
///
/// Deliberately emits no index or order field. A renderer arranges the sections
/// itself, and a position in the document would suggest a sequence this type
/// does not have.
/// @tparam G Concrete `SectionGroup<Title, Sections...>` type.
/// @return The group's JSON document. Empty string only if glaze's own JSON
///         writer fails on the assembled DOM (schema generation never throws).
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
template <typename G>
[[nodiscard]] std::string sectionGroupSchemaJson() {
    glz::generic_u64 dom{};
    dom["s-id"] = std::string{SectionGroupTraits<G>::typeId()};
    dom["s-title"] = std::string{G::title()};

    glz::generic_u64::array_t sections{};
    detail::forEachTupleElement<typename G::sections>([&]<typename SectionT, std::size_t I>() {
        static_cast<void>(I);
        glz::generic_u64 entry{};
        entry["action"] = std::string{::morph::model::ActionTraits<typename SectionT::action>::typeId()};
        entry["title"] = std::string{SectionT::title()};
        if constexpr (std::tuple_size_v<typename SectionT::binds> != 0) {
            detail::emitBindsInto<typename SectionT::binds>(entry["prefill"]);
        }
        sections.emplace_back(std::move(entry));
    });
    dom["s-sections"] = sections;

    return glz::write_json(dom).value_or(std::string{});
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

/// @brief Drives N independently editable action drafts on one screen.
///
/// Each section accumulates its own draft through `set<>` and dispatches
/// through the handler as soon as `ActionValidator<A>::ready` accepts it.
/// There is no active section and no sequence: a field belonging to any
/// declared section may be set at any time, in any order. This is the whole
/// difference from `morph::flows::FlowSession`, which throws on a field
/// outside its current step.
///
/// A ready section re-fires on every subsequent `set<>`. There is no latch and
/// no coalescing, matching `FlowSession`; a caller that wants one request per
/// pause debounces on its own side, where it knows what a pause means.
/// @tparam Model    The model the handler is bound to.
/// @tparam Sections One or more `Section<Action, Title, Binds...>` types.
template <typename Model, typename... Sections>
class SectionSet {
    static_assert(sizeof...(Sections) > 0, "SectionSet: a group needs at least one section");
    static_assert(::morph::forms::detail::AllDistinct<typename Sections::action...>::value,
                  "SectionSet: section action types must be pairwise distinct");

public:
    /// @brief Constructs a section set dispatching through @p handler.
    /// @param handler Handler every section dispatches through. Must outlive
    ///                this `SectionSet`.
    /// @param onError Optional callback invoked when a section's dispatch
    ///                fails. When absent, the error is logged via
    ///                `morph::log::logError`. Stored and invoked for this
    ///                object's whole lifetime, so anything the callable refers
    ///                to must outlive it.
    explicit SectionSet(::morph::bridge::BridgeHandler<Model>& handler MORPH_LIFETIMEBOUND,
                        std::function<void(std::exception_ptr)> onError MORPH_LIFETIMEBOUND = nullptr)
        : _handler{handler}, _onError{std::move(onError)} {}

    SectionSet(const SectionSet&) = delete;
    SectionSet& operator=(const SectionSet&) = delete;
    SectionSet(SectionSet&&) = delete;
    SectionSet& operator=(SectionSet&&) = delete;

    /// @brief Stops every callback this set installed from being delivered.
    ///
    /// There is nothing to detach: a section's continuations are owned by the
    /// in-flight dispatch, not held in a map this object could remove itself
    /// from. `_callbacks` gates each one on a token it checks before touching
    /// `this`, so a completion resolving after this object is gone finds the
    /// token stopped and returns without dereferencing anything.
    ///
    /// `requestStop()` is called explicitly rather than left to the member's
    /// own destruction, even though `_callbacks` is declared last: members are
    /// destroyed only *after* the destructor body runs, so a body that later
    /// grew a call pumping an event loop would otherwise deliver into a
    /// half-dead object. The body does not do that today; stopping first keeps
    /// it correct if one is ever added.
    ///
    /// How strong the gate is depends on which thread destroys this object,
    /// exactly as `CallbackScope`'s "Boundary of the guarantee" describes.
    /// Destroying it off the delivery thread is advisory only, and that caller
    /// owns its own synchronisation.
    ~SectionSet() { _callbacks.requestStop(); }

    /// @brief Sets one field of its section's draft, dispatching that section
    ///        if the draft is now ready.
    ///
    /// Unlike `FlowSession::set<>` this imposes no ordering: the field's action
    /// need only be one of the declared sections. Readiness is evaluated on a
    /// copy taken under the lock, so the dispatch happens with the lock
    /// released and a concurrent edit to another section cannot block on it.
    /// @tparam FieldPtr Pointer-to-data-member of a declared section's action struct.
    /// @param value New value for the field.
    template <auto FieldPtr>
    void set(::morph::bridge::detail::MemberPointerTraits<decltype(FieldPtr)>::ValueType value) {
        using A = ::morph::bridge::detail::MemberPointerTraits<decltype(FieldPtr)>::ClassType;
        static_assert((std::is_same_v<A, typename Sections::action> || ...),
                      "SectionSet::set<>: the field's action is not a section of this group");
        A draft{};
        {
            std::scoped_lock const lock{_mtx};
            std::get<A>(_drafts).*FieldPtr = std::move(value);
            draft = std::get<A>(_drafts);
        }
        if (::morph::model::ActionValidator<A>::ready(draft)) {
            fire<A>(std::move(draft));
        }
    }

    /// @brief Clears one section's draft back to a default-constructed action.
    ///
    /// Touches no other section and dispatches nothing. Values already captured
    /// from a previous successful dispatch stay in `resolved()`: they describe
    /// what the model was told, which resetting an editor does not undo.
    /// @tparam A The section's action type.
    template <typename A>
    void reset() {
        static_assert((std::is_same_v<A, typename Sections::action> || ...),
                      "SectionSet::reset<>: not a section of this group");
        std::scoped_lock const lock{_mtx};
        std::get<A>(_drafts) = A{};
    }

    /// @brief Snapshots one section's current draft.
    ///
    /// A copy taken under the lock, not a reference into live state, so a
    /// renderer can read one section while another thread edits a different one.
    /// @tparam A The section's action type.
    /// @return The draft as it stands.
    template <typename A>
    [[nodiscard]] A draft() const {
        static_assert((std::is_same_v<A, typename Sections::action> || ...),
                      "SectionSet::draft<>: not a section of this group");
        std::scoped_lock const lock{_mtx};
        return std::get<A>(_drafts);
    }

    /// @brief Looks up a value captured from a section's submitted draft or result.
    /// @param path `"<ActionTypeId>.<field>"`, matching a section's declared `Bind::path()`.
    /// @return The field's JSON-encoded value, or `std::nullopt` if @p path was
    ///         never captured — the section never fired successfully, or has no
    ///         such field.
    [[nodiscard]] std::optional<std::string> resolved(std::string_view path) const {
        std::scoped_lock const lock{_mtx};
        auto iter = _resolvedValues.find(std::string{path});
        if (iter == _resolvedValues.end()) {
            return std::nullopt;
        }
        return iter->second;
    }

private:
    /// @brief Records section @p A's submitted draft and its result under
    ///        `"<ActionTypeId>.<field>"` keys.
    ///
    /// Draft fields go in first and result fields overwrite them on a name
    /// collision: the result is what the model actually settled on, so it is
    /// the value a later section should prefill from.
    /// @tparam A Section action type.
    /// @param result The dispatch's successful result.
    template <typename A>
    void captureResult(const ::morph::model::ActionTraits<A>::Result& result) {
        std::scoped_lock const lock{_mtx};
        auto const typeId = ::morph::model::ActionTraits<A>::typeId();
        auto record = [&](const auto& value) {
            ::morph::forms::detail::forEachNamedMember(
                value, [&]<std::size_t I>(std::string_view name, const auto& member) {
                    static_cast<void>(I);
                    std::string json;
                    // Mirrors flows.hpp: a field glaze cannot write is skipped
                    // rather than stored as a broken string. The false arm is
                    // unreachable for any type that got this far, since the
                    // same writer already served schema generation.
                    if (!glz::write_json(member, json)) {
                        _resolvedValues[std::string{typeId} + "." + std::string{name}] = std::move(json);
                    }
                });
        };
        record(std::get<A>(_drafts));
        record(result);
    }

    /// @brief Reports a dispatch failure no `onError` callback was given for.
    /// @param typeId The failing section's action type-id.
    /// @param err    The captured exception.
    static void logUnhandledError(std::string_view typeId, const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            ::morph::log::logError(std::string{"[section:"} + std::string{typeId} +
                                   "] unhandled exception: " + exc.what());
        } catch (...) {
            ::morph::log::logError(std::string{"[section:"} + std::string{typeId} + "] unhandled unknown exception");
        }
    }

    /// @brief Dispatches section @p A's ready draft and routes its outcome.
    ///
    /// Both closures are gated on `_callbacks`, so neither touches anything on
    /// `this` once the set has been destroyed — a completion can still resolve
    /// after the set is gone.
    ///
    /// Nothing here is keyed to a "current" section, which is what makes
    /// sections cheaper than flow steps: a late reply cannot be stale, because
    /// there is no position for it to be stale relative to.
    /// @tparam A Section action type.
    /// @param draft The ready action to execute.
    template <typename A>
    void fire(A draft) {
        _handler.execute(std::move(draft))
            .then(_callbacks,
                  [this](const ::morph::model::ActionTraits<A>::Result& result) {
                      this->template captureResult<A>(result);
                  })
            .onError(_callbacks, [this](const std::exception_ptr& err) {
                if (_onError) {
                    _onError(err);
                } else {
                    logUnhandledError(::morph::model::ActionTraits<A>::typeId(), err);
                }
            });
    }

    ::morph::bridge::BridgeHandler<Model>& _handler;
    std::function<void(std::exception_ptr)> _onError;
    // _handler/_onError are set once at construction and never reassigned.
    // Everything below is touched both by the owning thread (set/reset/draft)
    // and by a dispatch's continuation, which runs on whatever thread resolves
    // the BridgeHandler completion -- see docs/spec/core/bridge.md's
    // executor/callback model.
    mutable std::mutex _mtx;
    std::tuple<typename Sections::action...> _drafts{};
    std::unordered_map<std::string, std::string> _resolvedValues;
    // Declared last, so it is the first member destroyed: every gated callback
    // is stopped before the state those callbacks touch goes away.
    ::morph::async::CallbackScope _callbacks;
};

}  // namespace morph::forms

// clang-format off -- public macro surface; see CONTRIBUTING.md, "Formatting/linting".
// NOLINTBEGIN(cppcoreguidelines-macro-usage) — registration macro is the intended public API
/// @brief Specialises `morph::forms::SectionGroupTraits<G>` with the string type-id @p NAME.
#define BRIDGE_REGISTER_SECTION_GROUP(G, NAME)                               \
    template <>                                                              \
    struct morph::forms::SectionGroupTraits<G> {                             \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };
// clang-format on
// NOLINTEND(cppcoreguidelines-macro-usage)
