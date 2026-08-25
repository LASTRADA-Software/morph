// SPDX-License-Identifier: Apache-2.0

#include "qml_surface.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <utility>

namespace morph::ladder::testkit {

namespace {

/// @brief One-based line number of @p offset within @p text.
/// @param text   The text @p offset indexes into.
/// @param offset Character offset.
/// @return The line number.
[[nodiscard]] int lineOf(const QString& text, qsizetype offset) {
    int line = 1;
    for (qsizetype i = 0; i < offset && i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\n')) {
            ++line;
        }
    }
    return line;
}

/// @brief Index of the `}` matching the `{` at @p open.
/// @param text The text to walk; must already be comment/string-blanked.
/// @param open Offset of the opening brace.
/// @return Offset of the matching brace, or `text.size()` if unbalanced.
[[nodiscard]] qsizetype matchBrace(const QString& text, qsizetype open) {
    int depth = 0;
    for (qsizetype i = open; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (text.at(i) == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return text.size();
}

/// @brief Counts the arguments of the parenthesised list starting at @p open.
/// @param text  The text to walk; must already be comment/string-blanked.
/// @param open  Offset of the opening parenthesis.
/// @param close Set to the offset just past the closing parenthesis.
/// @return The number of top-level, comma-separated arguments.
[[nodiscard]] int countArguments(const QString& text, qsizetype open, qsizetype& close) {
    int depth = 0;
    int commas = 0;
    bool sawContent = false;
    for (qsizetype i = open; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                close = i + 1;
                return sawContent ? commas + 1 : 0;
            }
        } else if (c == QLatin1Char(',') && depth == 1) {
            ++commas;
        } else if (!c.isSpace()) {
            sawContent = true;
        }
    }
    close = text.size();
    return sawContent ? commas + 1 : 0;
}

/// @brief `onListed` -> `listed`; anything not in handler shape -> empty.
/// @param handler The handler name as written in QML.
/// @return The signal name the handler binds.
[[nodiscard]] QString signalNameOf(const QString& handler) {
    if (handler.size() < 3 || !handler.startsWith(QLatin1String("on"))) {
        return {};
    }
    QString name = handler.mid(2);
    if (!name.at(0).isUpper()) {
        return {};
    }
    name[0] = name.at(0).toLower();
    return name;
}

/// @brief Human-readable class name of @p object's most-derived metaobject.
/// @param object The bridge.
/// @return Its class name.
[[nodiscard]] QString classNameOf(const QObject& object) {
    return QString::fromLatin1(object.metaObject()->className());
}

/// @brief The own (non-inherited) members of @p meta, by kind.
struct Surface {
    QStringList properties;
    QStringList signalNames;
    QMap<QString, QList<int>> methodArities;  ///< invokable/slot name -> parameter counts
    QMap<QString, QString> notifyByProperty;  ///< property -> its NOTIFY signal, when it has one
    QMap<QString, int> signalArities;
};

/// @brief Reads @p meta's own members — everything at or past its own offsets,
///        i.e. excluding `QObject`'s and any intermediate base's.
/// @param meta The bridge class's metaobject.
/// @return The surface inventory.
[[nodiscard]] Surface surfaceOf(const QMetaObject* meta) {
    Surface surface;
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        surface.properties.append(QString::fromLatin1(property.name()));
        if (property.hasNotifySignal()) {
            surface.notifyByProperty.insert(QString::fromLatin1(property.name()),
                                            QString::fromLatin1(property.notifySignal().name()));
        }
    }
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        const QString name = QString::fromLatin1(method.name());
        if (method.methodType() == QMetaMethod::Signal) {
            surface.signalNames.append(name);
            surface.signalArities.insert(name, method.parameterCount());
        } else {
            surface.methodArities[name].append(method.parameterCount());
        }
    }
    return surface;
}

}  // namespace

QString blankCommentsAndStrings(const QString& source) {
    enum class State : std::uint8_t { Code, LineComment, BlockComment, Double, Single, Backtick };

    QString out = source;
    State state = State::Code;
    for (qsizetype i = 0; i < source.size(); ++i) {
        const QChar c = source.at(i);
        const QChar next = (i + 1 < source.size()) ? source.at(i + 1) : QChar();
        switch (state) {
            case State::Code:
                if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
                    state = State::LineComment;
                    out[i] = QLatin1Char(' ');
                } else if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
                    state = State::BlockComment;
                    out[i] = QLatin1Char(' ');
                } else if (c == QLatin1Char('"')) {
                    state = State::Double;
                } else if (c == QLatin1Char('\'')) {
                    state = State::Single;
                } else if (c == QLatin1Char('`')) {
                    state = State::Backtick;
                }
                break;
            case State::LineComment:
                if (c == QLatin1Char('\n')) {
                    state = State::Code;
                } else {
                    out[i] = QLatin1Char(' ');
                }
                break;
            case State::BlockComment:
                if (c == QLatin1Char('*') && next == QLatin1Char('/')) {
                    out[i] = QLatin1Char(' ');
                    out[i + 1] = QLatin1Char(' ');
                    ++i;
                    state = State::Code;
                } else if (c != QLatin1Char('\n')) {
                    out[i] = QLatin1Char(' ');
                }
                break;
            case State::Double:
            case State::Single:
            case State::Backtick: {
                const QChar closer = (state == State::Double)   ? QLatin1Char('"')
                                     : (state == State::Single) ? QLatin1Char('\'')
                                                                : QLatin1Char('`');
                if (c == QLatin1Char('\\')) {
                    if (i + 1 < source.size() && next != QLatin1Char('\n')) {
                        out[i] = QLatin1Char(' ');
                        out[i + 1] = QLatin1Char(' ');
                        ++i;
                    }
                } else if (c == closer) {
                    state = State::Code;
                } else if (c != QLatin1Char('\n')) {
                    out[i] = QLatin1Char(' ');
                }
                break;
            }
        }
    }
    return out;
}

QmlScanResult scanQml(const QString& source, const QString& fileName, const QStringList& aliases) {
    const QString text = blankCommentsAndStrings(source);
    QmlScanResult result;

    // ── `alias.member` and `alias.member(...)`, anywhere in the file ──────
    for (const QString& alias : aliases) {
        const QRegularExpression pattern(QStringLiteral(R"(\b%1\s*\.\s*([A-Za-z_]\w*))").arg(alias));
        QRegularExpressionMatchIterator it = pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            // `page.tagController` where `page` itself is not the alias: the
            // `\b` above already refuses a partial identifier, but a *longer*
            // alias ending in this one would still match, so require the
            // preceding character not to be part of an identifier either.
            const qsizetype start = match.capturedStart(0);
            if (start > 0 && (text.at(start - 1).isLetterOrNumber() || text.at(start - 1) == QLatin1Char('_'))) {
                continue;
            }
            QmlReference reference;
            reference.member = match.captured(1);
            reference.alias = alias;
            reference.file = fileName;
            reference.line = lineOf(text, match.capturedStart(1));

            qsizetype after = match.capturedEnd(1);
            while (after < text.size() && text.at(after).isSpace()) {
                ++after;
            }

            // Is this read a *probe* -- `x.y !== undefined`, `x.y === undefined`,
            // or `typeof x.y` -- rather than a use? A probe is how QML asks
            // whether a conditionally-compiled member exists in this build, so
            // the answer "it does not" is the expected one, not a defect.
            {
                const QString tail = text.mid(after, 20);
                const bool comparedToUndefined =
                    (tail.startsWith(QStringLiteral("!==")) || tail.startsWith(QStringLiteral("==="))
                     || tail.startsWith(QStringLiteral("!=")) || tail.startsWith(QStringLiteral("==")))
                    && tail.contains(QStringLiteral("undefined"));
                const qsizetype before = match.capturedStart(0);
                const bool typeofApplied =
                    before >= 7 && text.mid(before - 7, 7) == QStringLiteral("typeof ");
                if (comparedToUndefined || typeofApplied) {
                    result.optionalProbes.insert(alias + QLatin1Char('.') + reference.member);
                }
            }
            if (after < text.size() && text.at(after) == QLatin1Char('(')) {
                qsizetype close = 0;
                reference.kind = QmlReferenceKind::Call;
                reference.argumentCount = countArguments(text, after, close);
            } else {
                reference.kind = QmlReferenceKind::Read;
            }
            result.referencesByAlias[alias].append(reference);
        }
    }

    // ── `Connections { target: <id>.<alias>; function onX(...) {} }` ──────
    static const QRegularExpression connectionsPattern(QStringLiteral(R"(\bConnections\b\s*\{)"));
    static const QRegularExpression targetPattern(
        QStringLiteral(R"(\btarget\s*:\s*([A-Za-z_]\w*)(?:\s*\.\s*([A-Za-z_]\w*))?)"));
    static const QRegularExpression functionHandlerPattern(QStringLiteral(R"(\bfunction\s+(on[A-Z]\w*)\s*\()"));
    static const QRegularExpression propertyHandlerPattern(QStringLiteral(R"(\b(on[A-Z]\w*)\s*:)"));

    QRegularExpressionMatchIterator blocks = connectionsPattern.globalMatch(text);
    while (blocks.hasNext()) {
        const QRegularExpressionMatch block = blocks.next();
        const qsizetype open = block.capturedEnd(0) - 1;
        const qsizetype close = matchBrace(text, open);
        const QString body = text.mid(open, close - open);

        const QRegularExpressionMatch target = targetPattern.match(body);
        if (!target.hasMatch() || target.captured(2).isEmpty()) {
            continue;  // a bare-identifier target is not a bridge alias here
        }
        const QString alias = target.captured(2);
        result.connectionsTargets.append(alias);
        if (!aliases.contains(alias)) {
            continue;
        }

        const auto record = [&](const QString& handler, int argumentCount, qsizetype offsetInBody) {
            const QString name = signalNameOf(handler);
            if (name.isEmpty()) {
                return;
            }
            QmlReference reference;
            reference.member = name;
            reference.alias = alias;
            reference.kind = QmlReferenceKind::SignalHandler;
            reference.argumentCount = argumentCount;
            reference.file = fileName;
            reference.line = lineOf(text, open + offsetInBody);
            result.referencesByAlias[alias].append(reference);
        };

        QRegularExpressionMatchIterator functions = functionHandlerPattern.globalMatch(body);
        while (functions.hasNext()) {
            const QRegularExpressionMatch handler = functions.next();
            qsizetype unusedClose = 0;
            record(handler.captured(1), countArguments(body, handler.capturedEnd(0) - 1, unusedClose),
                   handler.capturedStart(1));
        }
        QRegularExpressionMatchIterator properties = propertyHandlerPattern.globalMatch(body);
        while (properties.hasNext()) {
            const QRegularExpressionMatch handler = properties.next();
            // `function onX(` also matches `onX` followed by `(`, never `:`,
            // so the two patterns cannot double-count the same site.
            record(handler.captured(1), 0, handler.capturedStart(1));
        }
    }

    result.connectionsTargets.removeDuplicates();
    return result;
}

QmlSurfaceAudit::QmlSurfaceAudit(QString qmlDirectory) : _directories{std::move(qmlDirectory)} {}

void QmlSurfaceAudit::addDirectory(const QString& qmlDirectory) { _directories.append(qmlDirectory); }

void QmlSurfaceAudit::bind(const QString& alias, const QObject& bridge) {
    _bindings.append(Binding{.alias = alias, .file = {}, .bridge = &bridge});
}

void QmlSurfaceAudit::bindIn(const QString& fileName, const QString& alias, const QObject& bridge) {
    _bindings.append(Binding{.alias = alias, .file = fileName, .bridge = &bridge});
}

void QmlSurfaceAudit::allowUnbound(const QString& alias, const QString& member, const QString& reason) {
    _exemptions.append(Exemption{.alias = alias, .member = member, .reason = reason});
}

QStringList QmlSurfaceAudit::scannedFiles() const {
    QStringList files;
    for (const QString& directory : _directories) {
        QDirIterator it(directory, QStringList{QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(QFileInfo(it.next()).fileName());
        }
    }
    files.sort();
    return files;
}

QStringList QmlSurfaceAudit::run() const {
    QStringList findings;

    QStringList aliases;
    for (const Binding& binding : _bindings) {
        if (!aliases.contains(binding.alias)) {
            aliases.append(binding.alias);
        }
    }
    if (aliases.isEmpty()) {
        findings.append(QStringLiteral("audit is vacuous: no bridge was bound"));
        return findings;
    }

    // ── read and scan every .qml under every directory ────────────────────
    QMap<QString, QmlScanResult> byFile;
    QStringList allConnectionsTargets;
    int fileCount = 0;
    for (const QString& directory : _directories) {
        if (!QDir(directory).exists()) {
            findings.append(QStringLiteral("audit is vacuous: no such QML directory '%1'").arg(directory));
            continue;
        }
        QDirIterator it(directory, QStringList{QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                findings.append(QStringLiteral("cannot read '%1'").arg(path));
                continue;
            }
            QTextStream stream(&file);
            const QString name = QFileInfo(path).fileName();
            const QmlScanResult scan = scanQml(stream.readAll(), name, aliases);
            byFile.insert(name, scan);
            allConnectionsTargets.append(scan.connectionsTargets);
            ++fileCount;
        }
    }
    if (fileCount == 0) {
        findings.append(QStringLiteral("audit is vacuous: no .qml files found under %1")
                            .arg(_directories.join(QStringLiteral(", "))));
        return findings;
    }

    // A `Connections` block consuming signals from an alias nobody bound is a
    // bridge with no guard at all — the exact gap this class exists to close,
    // so it may not be reached by simply forgetting a bind() call.
    allConnectionsTargets.removeDuplicates();
    for (const QString& target : allConnectionsTargets) {
        if (!aliases.contains(target)) {
            findings.append(
                QStringLiteral("a Connections block targets '%1' but no bridge was bound to that alias").arg(target));
        }
    }

    // ── per *bridge object*: resolve QML -> metaobject, then sweep back ──
    //
    // Grouped by object rather than by binding: one instance is often reached
    // through more than one alias (`ledger`'s Main.qml calls it `ledgerBridge`
    // and hands it to a sub-view that calls it `bridge`), and a member bound
    // through either alias is bound. Sweeping per binding would report every
    // member each alias happens not to use.
    QVector<const QObject*> objects;
    for (const Binding& binding : _bindings) {
        if (!objects.contains(binding.bridge)) {
            objects.append(binding.bridge);
        }
    }

    for (const QObject* object : objects) {
        const QObject& bridge = *object;
        const QString cls = classNameOf(bridge);
        const Surface surface = surfaceOf(bridge.metaObject());

        QStringList objectAliases;
        QVector<QmlReference> references;
        for (const Binding& binding : _bindings) {
            if (binding.bridge != object) {
                continue;
            }
            if (!objectAliases.contains(binding.alias)) {
                objectAliases.append(binding.alias);
            }
            QVector<QmlReference> own;
            for (auto it = byFile.constBegin(); it != byFile.constEnd(); ++it) {
                if (!binding.file.isEmpty() && it.key() != binding.file) {
                    continue;
                }
                own.append(it.value().referencesByAlias.value(binding.alias));
            }
            if (own.isEmpty()) {
                findings.append(
                    QStringLiteral("audit is vacuous for %1: alias '%2' is referenced by no scanned .qml%3")
                        .arg(cls, binding.alias,
                             binding.file.isEmpty() ? QString() : QStringLiteral(" in %1").arg(binding.file)));
            }
            references.append(own);
        }
        const QString where = QStringLiteral("bound as '%1'").arg(objectAliases.join(QStringLiteral("'/'")));

        if (surface.properties.isEmpty() && surface.signalNames.isEmpty() && surface.methodArities.isEmpty()) {
            findings.append(QStringLiteral("%1 (%2) declares no QML-visible members at all").arg(cls, where));
            continue;
        }
        if (references.isEmpty()) {
            continue;  // already reported as vacuous, above
        }

        QSet<QString> readProperties;
        QSet<QString> handledSignals;
        QSet<QString> calledMethods;

        for (const QmlReference& reference : references) {
            const QString site = QStringLiteral("%1:%2").arg(reference.file).arg(reference.line);
            switch (reference.kind) {
                case QmlReferenceKind::Read:
                    if (surface.properties.contains(reference.member)) {
                        readProperties.insert(reference.member);
                    } else if (surface.methodArities.contains(reference.member)) {
                        calledMethods.insert(reference.member);  // a method reference, not a call
                    } else if (byFile.value(reference.file)
                                   .optionalProbes.contains(reference.alias + QLatin1Char('.') + reference.member)) {
                        // This file probes the member for existence, so it is
                        // written to cope with the member being absent -- see
                        // QmlScanResult::optionalProbes. Nothing to report: the
                        // QML is correct precisely *because* the member is
                        // missing in this configure.
                        //
                        // Scoped to the probing file, not the whole audit: a
                        // guard in one view says nothing about an unguarded read
                        // in another. It does excuse every read of that member
                        // within the file, which is deliberate -- the guard is
                        // normally written once, on the `visible:` binding that
                        // gates the rest.
                    } else {
                        findings.append(QStringLiteral("%1 reads '%2.%3' but %4 has no such property")
                                            .arg(site, reference.alias, reference.member, cls));
                    }
                    break;
                case QmlReferenceKind::Call:
                    if (!surface.methodArities.contains(reference.member)) {
                        if (surface.signalNames.contains(reference.member)) {
                            findings.append(QStringLiteral("%1 calls '%2.%3()' but %4 declares it as a signal, "
                                                           "not an invokable")
                                                .arg(site, reference.alias, reference.member, cls));
                        } else {
                            findings.append(QStringLiteral("%1 calls '%2.%3()' but %4 has no such invokable")
                                                .arg(site, reference.alias, reference.member, cls));
                        }
                    } else if (!surface.methodArities.value(reference.member).contains(reference.argumentCount)) {
                        QStringList arities;
                        for (const int arity : surface.methodArities.value(reference.member)) {
                            arities.append(QString::number(arity));
                        }
                        findings.append(QStringLiteral("%1 calls '%2.%3()' with %4 argument(s) but %5 takes %6")
                                            .arg(site, reference.alias, reference.member)
                                            .arg(reference.argumentCount)
                                            .arg(QStringLiteral("%1::%2").arg(cls, reference.member),
                                                 arities.join(QStringLiteral(" or "))));
                        calledMethods.insert(reference.member);
                    } else {
                        calledMethods.insert(reference.member);
                    }
                    break;
                case QmlReferenceKind::SignalHandler: {
                    const QString handler = QString(reference.member.at(0).toUpper()) + reference.member.mid(1);
                    if (!surface.signalNames.contains(reference.member)) {
                        findings.append(QStringLiteral("%1 handles 'on%2' but %3 emits no signal '%4'")
                                            .arg(site, handler, cls, reference.member));
                    } else if (reference.argumentCount > surface.signalArities.value(reference.member)) {
                        findings.append(QStringLiteral("%1 handles 'on%2' with %3 parameter(s) but %4::%5 carries %6")
                                            .arg(site, handler)
                                            .arg(reference.argumentCount)
                                            .arg(cls, reference.member)
                                            .arg(surface.signalArities.value(reference.member)));
                        handledSignals.insert(reference.member);
                    } else {
                        handledSignals.insert(reference.member);
                    }
                    break;
                }
            }
        }

        const auto exempt = [&](const QString& member) {
            for (const Exemption& exemption : _exemptions) {
                if (objectAliases.contains(exemption.alias) && exemption.member == member) {
                    return true;
                }
            }
            return false;
        };

        for (const QString& property : surface.properties) {
            const QString notify = surface.notifyByProperty.value(property);
            if (readProperties.contains(property) || (!notify.isEmpty() && handledSignals.contains(notify)) ||
                exempt(property)) {
                continue;
            }
            findings.append(
                QStringLiteral("%1::%2 is a Q_PROPERTY no scanned .qml reads (%3)").arg(cls, property, where));
        }
        for (const QString& name : surface.signalNames) {
            if (handledSignals.contains(name) || exempt(name)) {
                continue;
            }
            // A property's NOTIFY signal is covered by reading the property.
            bool isCoveredNotify = false;
            for (auto it = surface.notifyByProperty.constBegin(); it != surface.notifyByProperty.constEnd(); ++it) {
                if (it.value() == name && (readProperties.contains(it.key()) || exempt(it.key()))) {
                    isCoveredNotify = true;
                    break;
                }
            }
            if (isCoveredNotify) {
                continue;
            }
            findings.append(QStringLiteral("%1::%2 is a signal no scanned .qml handles (%3)").arg(cls, name, where));
        }
        for (auto it = surface.methodArities.constBegin(); it != surface.methodArities.constEnd(); ++it) {
            if (calledMethods.contains(it.key()) || exempt(it.key())) {
                continue;
            }
            findings.append(QStringLiteral("%1::%2 is invokable from QML but no scanned .qml calls it (%3)")
                                .arg(cls, it.key(), where));
        }

        // An exemption list is a second hand-written list, and therefore a
        // second thing that can go stale: delete the member and the exemption
        // outlives it silently, or bind it from QML and the exemption keeps
        // suppressing a check that would now pass. Both are findings, so the
        // list can only ever shrink deliberately.
        for (const Exemption& exemption : _exemptions) {
            if (!objectAliases.contains(exemption.alias)) {
                continue;
            }
            const bool exists = surface.properties.contains(exemption.member) ||
                                surface.signalNames.contains(exemption.member) ||
                                surface.methodArities.contains(exemption.member);
            if (!exists) {
                findings.append(QStringLiteral("stale allowUnbound('%1', '%2', \"%3\"): %4 has no such member")
                                    .arg(exemption.alias, exemption.member, exemption.reason, cls));
                continue;
            }
            if (readProperties.contains(exemption.member) || handledSignals.contains(exemption.member) ||
                calledMethods.contains(exemption.member)) {
                findings.append(
                    QStringLiteral("unnecessary allowUnbound('%1', '%2', \"%3\"): a scanned .qml does bind it")
                        .arg(exemption.alias, exemption.member, exemption.reason));
            }
        }
    }

    // An exemption naming an alias nobody bound suppresses nothing and hides
    // the fact that it suppresses nothing.
    for (const Exemption& exemption : _exemptions) {
        if (!aliases.contains(exemption.alias)) {
            findings.append(QStringLiteral("stale allowUnbound('%1', '%2', \"%3\"): no bridge is bound to that alias")
                                .arg(exemption.alias, exemption.member, exemption.reason));
        }
    }

    return findings;
}

}  // namespace morph::ladder::testkit
