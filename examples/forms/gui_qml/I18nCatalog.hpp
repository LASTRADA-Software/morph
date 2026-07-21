// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A minimal, in-memory realization of `morph::render::TranslationProvider`
/// (see `morph::render::resolveText`, `<morph/render/i18n.hpp>`) exposed to
/// QML. QML/JS cannot hold a `std::function`, so this `QObject` is the QML
/// host's concrete catalog: `addTranslation` seeds it directly (a
/// production host could instead adapt `QTranslator`/`.qm` lookups behind
/// the same `lookup()` method — morph ships the seam, not a storage
/// format), and `lookup` is the `key`/`locale` -> translated-text-or-miss
/// query `DynamicForm.qml`'s `resolveText` JS mirror calls.

#include <QtQml/qqmlregistration.h>

#include <QObject>
#include <QString>
#include <QVariant>
#include <map>
#include <string>
#include <utility>

class I18nCatalog : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit I18nCatalog(QObject* parent = nullptr);

    /// @brief Seeds (or replaces) the translation for @p key under @p locale.
    Q_INVOKABLE void addTranslation(const QString& locale, const QString& key, const QString& text);

    /// @brief The translated text for @p key under @p locale, or an invalid
    ///        `QVariant` (`undefined` in QML) on a miss.
    Q_INVOKABLE QVariant lookup(const QString& locale, const QString& key) const;

private:
    std::map<std::pair<std::string, std::string>, QString> _table;
};
