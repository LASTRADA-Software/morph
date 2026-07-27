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
///
/// `revision` is not part of `morph::render::TranslationProvider`'s contract
/// — it exists solely so QML's declarative bindings notice a catalog
/// mutation. `DynamicForm.qml`'s `fields` is an ordinary QML property
/// binding: it re-evaluates only when a property *it read* changes, and
/// `lookup()`'s return value is not such a property, so seeding
/// translations after a form has already computed `fields` once (e.g. from
/// `Component.onCompleted`, which fires after the initial binding pass)
/// would otherwise go unnoticed. `resolveText` reads `catalog.revision`
/// for exactly this reason — the same cache-invalidation idiom this file
/// already uses for `fieldOptions` via `optionsRevision`.
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

    /// @brief Bumped on every `addTranslation` call, so a QML binding that
    ///        reads it is invalidated whenever the catalog's contents change.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    explicit I18nCatalog(QObject* parent = nullptr);

    /// @brief Seeds (or replaces) the translation for @p key under @p locale.
    Q_INVOKABLE void addTranslation(const QString& locale, const QString& key, const QString& text);

    /// @brief The translated text for @p key under @p locale, or an invalid
    ///        `QVariant` (`undefined` in QML) on a miss.
    Q_INVOKABLE QVariant lookup(const QString& locale, const QString& key) const;

    /// @brief The current revision counter (see `Q_PROPERTY` above).
    /// @return The number of `addTranslation` calls made so far.
    [[nodiscard]] int revision() const noexcept;

signals:
    /// @brief Emitted once per `addTranslation` call, after `_revision` is bumped.
    void revisionChanged();

private:
    std::map<std::pair<std::string, std::string>, QString> _table;
    int _revision{0};
};
