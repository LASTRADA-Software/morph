// SPDX-License-Identifier: Apache-2.0
//
// Client-side, per-field widget-override / theming registry (the "toolkit"
// escape hatch -- docs/spec/forms/forms.md "Theming / component-override
// registry" / docs/planned/gui_renderer_toolkit.md). Entirely client-side: a
// slot is a QML Component the host app registers; it never appears in the
// schema or on the wire, and two renderers of the same schema may register
// different slots. DynamicForm consults byField/byWidget/byUnit/byKind/byType
// in that priority order and falls back to its own built-in control on a miss.
//
// byChrome registers the form's *chrome* instead -- the field label, help
// text, section/accordion/tab containers, header, status line, submit button,
// preview and result -- one Component per role, each replacing the built-in
// one wholesale (docs/spec/forms/forms.md, "Chrome slots").

import QtQuick

QtObject {
    id: registry

    property var _byField: ({})   // "action field" -> Component
    property var _byWidget: ({})  // x-widget id -> Component
    property var _byUnit: ({})    // unitAscii -> Component
    property var _byKind: ({})    // renderer kind ("quantity", "choice", ...) -> Component
    property var _byType: ({})    // JSON type ("integer", "string", ...) -> Component
    property var _byChrome: ({})  // chrome role ("fieldLabel", "section", ...) -> Component

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

    /// Registers @p component for every field of the given renderer kind --
    /// the control DynamicForm would otherwise draw, as its field
    /// descriptor's `kind` names it: "quantity", "choice", "enum", "datetime",
    /// "date", "boolean", "integer", "number", "string", "array",
    /// "objectArray" or "object". Unlike the JSON type, a kind tells a
    /// Quantity from a nested object, a Choice from an integer, and an enum or
    /// a date-time from free text.
    function byKind(kind, component) {
        _byKind[kind] = component
        revision++
    }

    /// Registers @p component for every field of the given JSON Schema
    /// `type` (e.g. "integer", "string").
    function byType(jsonType, component) {
        _byType[jsonType] = component
        revision++
    }

    /// Registers @p component as the form's chrome for @p role: one of
    /// "fieldLabel", "fieldHelp", "section", "accordion", "tabset", "header",
    /// "status", "submitButton", "preview", "result". An unknown role is
    /// stored and never asked for.
    function byChrome(role, component) {
        _byChrome[role] = component
        revision++
    }

    /// The Component registered for chrome @p role, or null (built-in). An
    /// "accordion" with no registration of its own falls back to "section": a
    /// host with one container card gets it for both kinds.
    function resolveChrome(role) {
        registry.revision
        if (_byChrome[role] !== undefined) return _byChrome[role]
        if (role === "accordion" && _byChrome["section"] !== undefined) return _byChrome["section"]
        return null
    }

    /// Resolution order: field -> x-widget -> unit -> kind -> type -> null
    /// (built-in). Returns the first matching Component, or null on a total
    /// miss (DynamicForm then renders its own built-in control). @p kind is
    /// optional, so a caller passing the five original arguments resolves
    /// exactly as before.
    function resolve(action, field, xWidget, unitAscii, jsonType, kind) {
        registry.revision
        const key = action + " " + field
        if (_byField[key] !== undefined) return _byField[key]
        if (xWidget !== "" && _byWidget[xWidget] !== undefined) return _byWidget[xWidget]
        if (unitAscii !== "" && _byUnit[unitAscii] !== undefined) return _byUnit[unitAscii]
        if (kind !== undefined && kind !== "" && _byKind[kind] !== undefined) return _byKind[kind]
        if (jsonType !== "" && _byType[jsonType] !== undefined) return _byType[jsonType]
        return null
    }
}
