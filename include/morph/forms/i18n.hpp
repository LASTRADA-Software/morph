// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/i18n.hpp
/// @brief Stable, schema-derived message-key vocabulary for renderer i18n.
///
/// A renderer that wants to translate a piece of GUI display text (a field's
/// label/help/placeholder, a layout group's title, a cross-field rule's
/// violation message, a wizard's or app-shell's title) needs a **stable
/// catalog key** to look that text up under. This header is the single
/// source of truth for how such a key is *derived* from identifiers the
/// schema (or the `actionType` label a renderer already has, per
/// `docs/spec/forms/forms.md`'s renderer contract) already carries:
/// `morph::model::ActionTraits<A>::typeId()`, a reflected wire field name, or
/// a 0-based index into a schema array (`x-layout.groups`, `x-rules`, a
/// wizard's steps, an app's menu).
///
/// No key derived here is written into the schema — a renderer computes it
/// itself from data it already has, so the common (no-override) case adds
/// zero bytes to any schema. See `morph::render::resolveText`
/// (`render/i18n.hpp`) for how a derived key is looked up against a
/// host-supplied catalog, with the schema's authored literal text as the
/// fallback on a miss.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace morph::forms::i18n {

/// @brief Which of a field's three translatable texts a message key names.
enum class FieldSlot : std::uint8_t {
    Label,       ///< The field's display label (`<stem>.label`).
    Help,        ///< The field's help/description text (`<stem>.help`).
    Placeholder  ///< The field's empty-state placeholder text (`<stem>.placeholder`).
};

/// @brief The catalog-key suffix for @p slot.
/// @param slot The field text slot.
/// @return `"label"`, `"help"`, or `"placeholder"`.
[[nodiscard]] constexpr std::string_view fieldSlotName(FieldSlot slot) noexcept {
    switch (slot) {
        case FieldSlot::Label:
            return "label";
        case FieldSlot::Help:
            return "help";
        case FieldSlot::Placeholder:
            return "placeholder";
        default:
            // Unreachable through any real code path: FieldSlot is a closed
            // enum and every enumerator is already handled explicitly above.
            // This arm only exists to satisfy the compiler that the function
            // returns on every enum value, including one manufactured by an
            // out-of-range `static_cast` -- mirrors ruleKindName's identical
            // default: arm in forms.hpp.
            return "label";
    }
}

/// @brief Appends `.<slotName>` to a message-key stem.
/// @param stem A message-key stem: a derived `<actionTypeId>.<wireField>` or
///             a host-declared override (a field's `x-i18nKey`).
/// @param slot Which field text this key names.
/// @return `"<stem>.<slotName>"`.
[[nodiscard]] inline std::string withSlot(std::string_view stem, FieldSlot slot) {
    std::string key{stem};
    key += '.';
    key += fieldSlotName(slot);
    return key;
}

/// @brief The derived message-key stem for a field: `<actionTypeId>.<wireField>`.
/// @param actionTypeId `ActionTraits<A>::typeId()` of the action the field belongs to.
/// @param wireField    The field's wire key (its reflected member name).
/// @return The stem, before the `.label` / `.help` / `.placeholder` suffix.
[[nodiscard]] inline std::string fieldKeyStem(std::string_view actionTypeId, std::string_view wireField) {
    std::string stem{actionTypeId};
    stem += '.';
    stem += wireField;
    return stem;
}

/// @brief The fully-derived message key for one field text slot.
/// @param actionTypeId `ActionTraits<A>::typeId()` of the action the field belongs to.
/// @param wireField    The field's wire key.
/// @param slot         Which of the field's texts this key names.
/// @return `"<actionTypeId>.<wireField>.<slot>"`.
[[nodiscard]] inline std::string fieldKey(std::string_view actionTypeId, std::string_view wireField, FieldSlot slot) {
    return withSlot(fieldKeyStem(actionTypeId, wireField), slot);
}

/// @brief The explicit override key for one field text slot, when the field
///        declares an `x-i18nKey` stem override.
///
/// A single `FieldMeta::i18nKey` override (see `gui_field_metadata.md`)
/// replaces the derived `<actionTypeId>.<wireField>` stem for *all three* of
/// a field's text slots; the `.label` / `.help` / `.placeholder` suffix is
/// still appended per slot, exactly as it is for the derived key.
/// @param i18nKeyOverride The field's declared `FieldMeta::i18nKey` (empty = no override).
/// @param slot            Which of the field's texts this key names.
/// @return `std::nullopt` when @p i18nKeyOverride is empty; otherwise `"<i18nKeyOverride>.<slot>"`.
[[nodiscard]] inline std::optional<std::string> explicitFieldKey(std::string_view i18nKeyOverride, FieldSlot slot) {
    if (i18nKeyOverride.empty()) {
        return std::nullopt;
    }
    return withSlot(i18nKeyOverride, slot);
}

/// @brief The derived message key for a layout group's title.
/// @param actionTypeId `ActionTraits<A>::typeId()` of the action the group belongs to.
/// @param groupIndex   The group's 0-based index into `x-layout.groups`.
/// @return `"<actionTypeId>.group.<groupIndex>"`.
[[nodiscard]] inline std::string groupKey(std::string_view actionTypeId, std::size_t groupIndex) {
    std::string key{actionTypeId};
    key += ".group.";
    key += std::to_string(groupIndex);
    return key;
}

/// @brief The derived message key for a cross-field rule's violation message.
/// @param actionTypeId `ActionTraits<A>::typeId()` of the action the rule belongs to.
/// @param ruleIndex    The rule's 0-based index into `x-rules`.
/// @return `"<actionTypeId>.rule.<ruleIndex>"`.
[[nodiscard]] inline std::string ruleKey(std::string_view actionTypeId, std::size_t ruleIndex) {
    std::string key{actionTypeId};
    key += ".rule.";
    key += std::to_string(ruleIndex);
    return key;
}

/// @brief The derived message key for a wizard's own title.
/// @param wizardId The registered wizard id.
/// @return `"<wizardId>.title"`.
[[nodiscard]] inline std::string wizardTitleKey(std::string_view wizardId) {
    std::string key{wizardId};
    key += ".title";
    return key;
}

/// @brief The derived message key for one wizard step's title.
/// @param wizardId  The registered wizard id.
/// @param stepIndex The step's 0-based index.
/// @return `"<wizardId>.step.<stepIndex>.title"`.
[[nodiscard]] inline std::string wizardStepTitleKey(std::string_view wizardId, std::size_t stepIndex) {
    std::string key{wizardId};
    key += ".step.";
    key += std::to_string(stepIndex);
    key += ".title";
    return key;
}

/// @brief The derived message key for an app shell's own title.
/// @param appId The registered app id.
/// @return `"<appId>.title"`.
[[nodiscard]] inline std::string appTitleKey(std::string_view appId) {
    std::string key{appId};
    key += ".title";
    return key;
}

/// @brief The derived message key for one app-menu entry's label.
/// @param appId     The registered app id.
/// @param menuIndex The menu entry's 0-based index.
/// @return `"<appId>.menu.<menuIndex>.label"`.
[[nodiscard]] inline std::string appMenuLabelKey(std::string_view appId, std::size_t menuIndex) {
    std::string key{appId};
    key += ".menu.";
    key += std::to_string(menuIndex);
    key += ".label";
    return key;
}

}  // namespace morph::forms::i18n
