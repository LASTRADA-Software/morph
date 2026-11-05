// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <QVariantList>
#include <QtTypes>

namespace morph::qt_quick::detail {

/// Measures the pieces of a text that Qt keeps whole when it wraps the text, for QML, which has no way to ask the line
/// breaker where it may break. `Theme.textMeasure` holds the one the backend makes when it starts.
class TextMeasure final : public QObject {
    Q_OBJECT

public:
    /// A measure with no parent.
    /// @param parent The owning object, or null.
    explicit TextMeasure(QObject* parent = nullptr);

    /// The width of the widest unbreakable segment of a text laid out in the given faces. A segment is the text
    /// between two line-break opportunities (the Unicode line-breaking analysis Qt's own wrapping uses, so a no-break
    /// space, a no-break hyphen or a path of slashes is as Qt keeps it), with the whitespace that ends it dropped, as
    /// Qt's line width drops it. Each part of a segment is measured in the face of the piece that holds it.
    /// @param pieces The text in order, each a map of `text` (a string) and `font` (a font): a run and the face it is
    ///        drawn in. The segments run across pieces, so a word split between a regular and a strong run is one.
    /// @return The widest segment's width in pixels; 0 for no text.
    Q_INVOKABLE [[nodiscard]] qreal widestSegment(QVariantList const& pieces) const;
};

}  // namespace morph::qt_quick::detail
