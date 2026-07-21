// SPDX-License-Identifier: Apache-2.0

#include "I18nCatalog.hpp"

I18nCatalog::I18nCatalog(QObject* parent) : QObject{parent} {}

void I18nCatalog::addTranslation(const QString& locale, const QString& key, const QString& text) {
    _table[{locale.toStdString(), key.toStdString()}] = text;
}

QVariant I18nCatalog::lookup(const QString& locale, const QString& key) const {
    auto const it = _table.find({locale.toStdString(), key.toStdString()});
    if (it == _table.end()) {
        return {};
    }
    return it->second;
}
