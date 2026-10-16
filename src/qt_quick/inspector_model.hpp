// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>

namespace morph::qt_quick::detail {

/// What the inspector window's QML reads: the sections as text, the queries as a list of `{name, line, query}`, and
/// the refetch its buttons call. Built only into a client with the development tools.
class InspectorModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString counters READ counters NOTIFY changed)
    Q_PROPERTY(QVariantList queries READ queries NOTIFY changed)
    Q_PROPERTY(QString nodes READ nodes NOTIFY changed)
    Q_PROPERTY(QString log READ log NOTIFY changed)

public:
    /// A model whose buttons call @p refetch with the query's name.
    explicit InspectorModel(std::function<void(QString const&)> refetch, QObject* parent = nullptr)
        : QObject{parent}, _refetch{std::move(refetch)} {}

    [[nodiscard]] QString counters() const { return _counters; }
    [[nodiscard]] QVariantList queries() const { return _queries; }
    [[nodiscard]] QString nodes() const { return _nodes; }
    [[nodiscard]] QString log() const { return _log; }

    /// Replaces every section and tells QML.
    void set(QString counters, QVariantList queries, QString nodes, QString log) {
        _counters = std::move(counters);
        _queries = std::move(queries);
        _nodes = std::move(nodes);
        _log = std::move(log);
        Q_EMIT changed();
    }

    /// What a query's refetch button calls.
    Q_INVOKABLE void refetch(QString const& query) {
        if (_refetch) {
            _refetch(query);
        }
    }

Q_SIGNALS:
    /// Every section changed.
    void changed();

private:
    std::function<void(QString const&)> _refetch;
    QString _counters;
    QVariantList _queries;
    QString _nodes;
    QString _log;
};

}  // namespace morph::qt_quick::detail
