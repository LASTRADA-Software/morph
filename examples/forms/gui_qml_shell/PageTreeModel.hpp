// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// QAbstractItemModel that represents the navigation tree for the form shell.
/// Nodes are folders (internal) or pages (leaves). The tree is backed by a
/// JSON file on disk for persistence.

#include <QAbstractItemModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct PageNode {
    QString name;
    QString source;   // "builtin://ActionType" or file:// URL
    bool isFolder = false;
    bool visible = true;
    PageNode* parent = nullptr;
    QVector<PageNode*> children;

    ~PageNode() { qDeleteAll(children); }
};

class PageTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        SourceRole,
        NodeTypeRole,
        VisibleRole,
    };

    explicit PageTreeModel(QObject* parent = nullptr);
    ~PageTreeModel() override;

    // QAbstractItemModel interface
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // Tree mutation
    Q_INVOKABLE QModelIndex addFolder(const QModelIndex& parent, const QString& name);
    Q_INVOKABLE QModelIndex addPage(const QModelIndex& parent, const QString& name, const QString& source);
    Q_INVOKABLE bool removeNode(const QModelIndex& index);
    Q_INVOKABLE bool renameNode(const QModelIndex& index, const QString& name);
    Q_INVOKABLE bool setNodeVisible(const QModelIndex& index, bool visible);
    Q_INVOKABLE bool moveNode(const QModelIndex& from, const QModelIndex& toParent, int toRow);

    // Serialization
    Q_INVOKABLE QJsonArray toJson() const;
    Q_INVOKABLE void fromJson(const QJsonArray& json);

    // Query
    Q_INVOKABLE bool isFolder(const QModelIndex& index) const;
    Q_INVOKABLE QString nodeSource(const QModelIndex& index) const;

    /// @brief Returns the index of the first page node matching @p source,
    ///        or an invalid index if not found.
    QModelIndex findPageBySource(const QString& source, const QModelIndex& parent = {}) const;

signals:
    void treeChanged();

private:
    PageNode* nodeFromIndex(const QModelIndex& index) const;
    QModelIndex indexFromNode(PageNode* node) const;
    PageNode* appendChild(PageNode* parent, const QString& name, const QString& source, bool isFolder);
    void buildFromJson(const QJsonArray& json, PageNode* parent);

    PageNode _root;
};