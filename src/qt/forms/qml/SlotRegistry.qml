// SPDX-License-Identifier: Apache-2.0
//
// Client-side, per-field widget-override / theming registry (the "toolkit"
// escape hatch -- docs/spec/forms/forms.md "Theming / component-override
// registry" / docs/planned/gui_renderer_toolkit.md). Entirely client-side: a
// slot is a QML Component the host app registers; it never appears in the
// schema or on the wire, and two renderers of the same schema may register
// different slots. DynamicForm consults byField/byWidget/byUnit/byType in
// that priority order and falls back to its own built-in control on a miss.

import QtQuick

QtObject {
    id: registry

    property var _byField: ({})   // "action field" -> Component
    property var _byWidget: ({})  // x-widget id -> Component
    property var _byUnit: ({})    // unitAscii -> Component
    property var _byType: ({})    // JSON type ("integer", "string", ...) -> Component

    // Bumped on every by*() registration. `_byField`/`_byWidget`/`_byUnit`/
    // `_byType` are plain JS objects mutated in place (obj[key] = value);
    // that mutation does not, by itself, fire a QML property-change
    // notification, so a binding that already read one of them (e.g.
    // DynamicForm's `overrideComponent`, evaluated once at field-delegate
    // construction, typically before a host's Component.onCompleted has
    // finished registering slots) would otherwise never re-evaluate once a
    // slot is registered afterwards. `resolve()` reads `revision` for
    // exactly this reason -- the same cache-invalidation idiom I18nCatalog's
    // `revision` / DynamicForm's `optionsRevision` already use.
    property int revision: 0

    /// Registers @p component as the override for exactly one field of one
    /// action (the most specific, highest-priority match).
    function byField(action, field, component) {
        _byField[action + " " + field] = component
        revision++
    }

    /// Registers @p component for every field whose schema carries
    /// `x-widget: xWidget` (e.g. "slider", "radio").
    function byWidget(xWidget, component) {
        _byWidget[xWidget] = component
        revision++
    }

    /// Registers @p component for every Quantity field of the given
    /// canonical unit (ExtUnits.unitAscii).
    function byUnit(unitAscii, component) {
        _byUnit[unitAscii] = component
        revision++
    }

    /// Registers @p component for every field of the given JSON Schema
    /// `type` (e.g. "integer", "string").
    function byType(jsonType, component) {
        _byType[jsonType] = component
        revision++
    }

    /// Resolution order: field -> x-widget -> unit -> type -> null
    /// (built-in). Returns the first matching Component, or null on a total
    /// miss (DynamicForm then renders its own built-in control).
    function resolve(action, field, xWidget, unitAscii, jsonType) {
        registry.revision
        const key = action + " " + field
        if (_byField[key] !== undefined) return _byField[key]
        if (xWidget !== "" && _byWidget[xWidget] !== undefined) return _byWidget[xWidget]
        if (unitAscii !== "" && _byUnit[unitAscii] !== undefined) return _byUnit[unitAscii]
        if (jsonType !== "" && _byType[jsonType] !== undefined) return _byType[jsonType]
        return null
    }
}
