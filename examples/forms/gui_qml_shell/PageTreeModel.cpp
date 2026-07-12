// SPDX-License-Identifier: Apache-2.0

#include "PageTreeModel.hpp"

#include <QJsonDocument>
#include <QJsonObject>

PageTreeModel::PageTreeModel(QObject* parent)
    : QAbstractItemModel{parent} {
    _root.isFolder = true;
}

PageTreeModel::~PageTreeModel() = default;

QModelIndex PageTreeModel::index(int row, int column, const QModelIndex& parentIdx) const {
    if (!hasIndex(row, column, parentIdx))
        return {};
    auto* parent = parentIdx.isValid() ? nodeFromIndex(parentIdx) : const_cast<PageNode*>(&_root);
    if (row < parent->children.size())
        return createIndex(row, column, parent->children[row]);
    return {};
}

QModelIndex PageTreeModel::parent(const QModelIndex& index) const {
    if (!index.isValid())
        return {};
    auto* node = nodeFromIndex(index);
    if (!node || node->parent == &_root || !node->parent)
        return {};
    return indexFromNode(node->parent);
}

int PageTreeModel::rowCount(const QModelIndex& parentIdx) const {
    if (!parentIdx.isValid())
        return _root.children.size();
    auto* node = nodeFromIndex(parentIdx);
    return node ? node->children.size() : 0;
}

int PageTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant PageTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    auto* node = nodeFromIndex(index);
    if (!node)
        return {};
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return node->name;
    case SourceRole:
        return node->source;
    case NodeTypeRole:
        return node->isFolder ? QStringLiteral("folder") : QStringLiteral("page");
    case VisibleRole:
        return node->visible;
    }
    return {};
}

QHash<int, QByteArray> PageTreeModel::roleNames() const {
    return {
        {NameRole, "name"},
        {SourceRole, "source"},
        {NodeTypeRole, "nodeType"},
        {VisibleRole, "visible"},
    };
}

Qt::ItemFlags PageTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QModelIndex PageTreeModel::addFolder(const QModelIndex& parentIdx, const QString& name) {
    auto* parent = parentIdx.isValid() ? nodeFromIndex(parentIdx) : &_root;
    if (!parent)
        return {};
    auto* child = appendChild(parent, name, {}, true);
    return indexFromNode(child);
}

QModelIndex PageTreeModel::addPage(const QModelIndex& parentIdx, const QString& name, const QString& source) {
    auto* parent = parentIdx.isValid() ? nodeFromIndex(parentIdx) : &_root;
    if (!parent)
        return {};
    auto* child = appendChild(parent, name, source, false);
    return indexFromNode(child);
}

bool PageTreeModel::removeNode(const QModelIndex& index) {
    if (!index.isValid())
        return false;
    auto* node = nodeFromIndex(index);
    if (!node || !node->parent)
        return false;
    auto* parent = node->parent;
    int row = parent->children.indexOf(node);
    if (row < 0)
        return false;
    beginRemoveRows(indexFromNode(parent), row, row);
    parent->children.removeAt(row);
    delete node;
    endRemoveRows();
    emit treeChanged();
    return true;
}

bool PageTreeModel::renameNode(const QModelIndex& index, const QString& name) {
    if (!index.isValid())
        return false;
    auto* node = nodeFromIndex(index);
    if (!node)
        return false;
    node->name = name;
    emit dataChanged(index, index, {NameRole, Qt::DisplayRole});
    emit treeChanged();
    return true;
}

bool PageTreeModel::setNodeVisible(const QModelIndex& index, bool visible) {
    if (!index.isValid())
        return false;
    auto* node = nodeFromIndex(index);
    if (!node)
        return false;
    node->visible = visible;
    emit dataChanged(index, index, {VisibleRole});
    emit treeChanged();
    return true;
}

bool PageTreeModel::moveNode(const QModelIndex& from, const QModelIndex& toParent, int toRow) {
    if (!from.isValid())
        return false;
    auto* node = nodeFromIndex(from);
    if (!node || !node->parent)
        return false;
    auto* srcParent = node->parent;
    auto* dstParent = toParent.isValid() ? nodeFromIndex(toParent) : &_root;
    if (!dstParent)
        return false;
    int srcRow = srcParent->children.indexOf(node);
    if (srcRow < 0)
        return false;
    if (srcParent == dstParent && toRow == srcRow)
        return false;
    if (toRow > srcRow)
        --toRow; // adjust after removal
    if (!beginMoveRows(indexFromNode(srcParent), srcRow, srcRow, indexFromNode(dstParent), toRow))
        return false;
    srcParent->children.removeAt(srcRow);
    if (toRow < 0 || toRow > dstParent->children.size())
        dstParent->children.append(node);
    else
        dstParent->children.insert(toRow, node);
    node->parent = dstParent;
    endMoveRows();
    emit treeChanged();
    return true;
}

QJsonArray PageTreeModel::toJson() const {
    QJsonArray arr;
    for (auto* child : _root.children) {
        QJsonObject obj;
        obj["name"] = child->name;
        obj["type"] = child->isFolder ? QStringLiteral("folder") : QStringLiteral("page");
        obj["visible"] = child->visible;
        if (!child->isFolder)
            obj["source"] = child->source;
        if (child->isFolder && !child->children.isEmpty()) {
            QJsonArray childArr;
            for (auto* gc : child->children) {
                QJsonObject go;
                go["name"] = gc->name;
                go["type"] = gc->isFolder ? QStringLiteral("folder") : QStringLiteral("page");
                go["visible"] = gc->visible;
                if (!gc->isFolder)
                    go["source"] = gc->source;
                childArr.append(go);
            }
            obj["children"] = childArr;
        }
        arr.append(obj);
    }
    return arr;
}

void PageTreeModel::fromJson(const QJsonArray& json) {
    beginResetModel();
    qDeleteAll(_root.children);
    _root.children.clear();
    buildFromJson(json, &_root);
    endResetModel();
    emit treeChanged();
}

bool PageTreeModel::isFolder(const QModelIndex& index) const {
    if (!index.isValid())
        return false;
    auto* node = nodeFromIndex(index);
    return node && node->isFolder;
}

QString PageTreeModel::nodeSource(const QModelIndex& index) const {
    if (!index.isValid())
        return {};
    auto* node = nodeFromIndex(index);
    return node ? node->source : QString{};
}

QModelIndex PageTreeModel::findPageBySource(const QString& source, const QModelIndex& parentIdx) const {
    auto* parent = parentIdx.isValid() ? nodeFromIndex(parentIdx) : const_cast<PageNode*>(&_root);
    if (!parent)
        return {};
    for (int i = 0; i < parent->children.size(); ++i) {
        auto* child = parent->children[i];
        if (!child->isFolder && child->source == source)
            return createIndex(i, 0, child);
        if (child->isFolder) {
            auto found = findPageBySource(source, indexFromNode(child));
            if (found.isValid())
                return found;
        }
    }
    return {};
}

PageNode* PageTreeModel::nodeFromIndex(const QModelIndex& index) const {
    if (!index.isValid())
        return nullptr;
    return static_cast<PageNode*>(index.internalPointer());
}

QModelIndex PageTreeModel::indexFromNode(PageNode* node) const {
    if (!node || !node->parent)
        return {};
    int row = node->parent->children.indexOf(node);
    if (row < 0)
        return {};
    return createIndex(row, 0, node);
}

PageNode* PageTreeModel::appendChild(PageNode* parent, const QString& name, const QString& source, bool isFolder) {
    auto* child = new PageNode;
    child->name = name;
    child->source = source;
    child->isFolder = isFolder;
    child->parent = parent;
    int row = parent->children.size();
    beginInsertRows(indexFromNode(parent), row, row);
    parent->children.append(child);
    endInsertRows();
    emit treeChanged();
    return child;
}

void PageTreeModel::buildFromJson(const QJsonArray& json, PageNode* parent) {
    for (const auto& val : json) {
        auto obj = val.toObject();
        auto* child = new PageNode;
        child->name = obj["name"].toString();
        child->source = obj["source"].toString();
        child->isFolder = obj["type"].toString() == QStringLiteral("folder");
        child->visible = obj.value("visible").toBool(true);
        child->parent = parent;
        parent->children.append(child);
        if (child->isFolder && obj.contains("children"))
            buildFromJson(obj["children"].toArray(), child);
    }
}