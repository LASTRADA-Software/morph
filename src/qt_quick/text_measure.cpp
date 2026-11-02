// SPDX-License-Identifier: Apache-2.0

#include "qt_quick/text_measure.hpp"

#include <QChar>
#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QTextBoundaryFinder>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <vector>

namespace morph::qt_quick::detail {

namespace {

/// A piece of the text: where it ends in the joined text, and the metrics of its face.
struct Part {
    qsizetype end;
    QFontMetricsF metrics;
};

/// Whether @p character ends a line without taking width: a space or a line break. A no-break space is text.
bool isLineSpace(QChar character) noexcept {
    return character.isSpace() && character != QChar{0x00A0} && character != QChar{0x202F} &&
           character != QChar{0x2007};
}

/// The advance of the characters of @p text from @p begin to @p end, each in the face of the part that holds it.
qreal widthOf(std::vector<Part> const& parts, QString const& text, qsizetype begin, qsizetype end) {
    qreal width = 0;
    qsizetype partBegin = 0;
    for (Part const& part : parts) {
        qsizetype const first = std::max(begin, partBegin);
        qsizetype const last = std::min(end, part.end);
        if (first < last) {
            width += part.metrics.horizontalAdvance(text.mid(first, last - first));
        }
        partBegin = part.end;
    }
    return width;
}

}  // namespace

TextMeasure::TextMeasure(QObject* parent) : QObject{parent} {}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): QML calls it on the instance Theme.textMeasure holds
qreal TextMeasure::widestSegment(QVariantList const& pieces) const {
    QString joined;
    std::vector<Part> parts;
    parts.reserve(static_cast<std::size_t>(pieces.size()));
    for (QVariant const& entry : pieces) {
        QVariantMap const piece = entry.toMap();
        joined += piece.value(QStringLiteral("text")).toString();
        parts.push_back(
            Part{.end = joined.size(), .metrics = QFontMetricsF{piece.value(QStringLiteral("font")).value<QFont>()}});
    }
    QTextBoundaryFinder finder{QTextBoundaryFinder::Line, joined};
    qreal widest = 0;
    qsizetype begin = 0;
    while (begin < joined.size()) {
        qsizetype next = finder.toNextBoundary();
        if (next < 0 || next > joined.size()) {
            next = joined.size();
        }
        if (next <= begin) {
            break;
        }
        qsizetype end = next;
        while (end > begin && isLineSpace(joined.at(end - 1))) {
            --end;
        }
        widest = std::max(widest, widthOf(parts, joined, begin, end));
        begin = next;
    }
    return widest;
}

}  // namespace morph::qt_quick::detail
