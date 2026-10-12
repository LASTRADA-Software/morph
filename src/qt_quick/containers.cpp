// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QUrl>
#include <QVariantList>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

// ── Stack, Grid ───────────────────────────────────────────────────────────

StackImpl::StackImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
    : ContainerItemWidget{context, QStringLiteral("MorphStack"), parent,
                          QVariantMap{{QStringLiteral("axis"), axis == ui::Axis::Horizontal ? 1 : 0}}} {}

void StackImpl::setGap(int gap) { write("gap", gap); }

GridImpl::GridImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphGrid"), parent} {}

void GridImpl::setColumns(int columns) { write("columns", std::max(1, columns)); }

void GridImpl::setGap(int gap) { write("gap", gap); }

void GridImpl::setSpan(ui::Widget& child, int span) {
    if (ItemBase const* const base = baseOf(child)) {
        base->write("columnSpan", std::max(1, span));
    }
}

// ── Panel, Scroll, Slot ───────────────────────────────────────────────────

PanelImpl::PanelImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphPanel"), parent} {}

void PanelImpl::setTitle(std::string_view title) { write("title", toQString(title)); }

void PanelImpl::setPadding(int padding) { write("padding", padding); }

void PanelImpl::setCollapsible(bool collapsible) { write("collapsible", collapsible); }

void PanelImpl::setCollapsed(bool collapsed) { write("collapsed", collapsed); }

void PanelImpl::setOnToggle(std::function<void(bool)> onToggle) {
    relay().on(QStringLiteral("toggle"), [onToggle = std::move(onToggle)](QVariant const& payload) {
        if (onToggle) {
            onToggle(payload.toBool());
        }
    });
}

ScrollImpl::ScrollImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
    : ContainerItemWidget{context, QStringLiteral("MorphScroll"), parent,
                          QVariantMap{{QStringLiteral("axis"), axis == ui::Axis::Horizontal ? 1 : 0}}} {}

SlotImpl::SlotImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphSlot"), parent} {}

// ── Tabs ──────────────────────────────────────────────────────────────────

TabsImpl::TabsImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphTabs"), parent} {}

void TabsImpl::setTabs(std::vector<std::string> const& labels) { write("labels", toStringList(labels)); }

void TabsImpl::setSelected(std::size_t index) {
    int const count = static_cast<int>(read("labels").toStringList().size());
    int const wanted = std::cmp_less(index, count) ? static_cast<int>(index) : -1;
    if (highlighted() != wanted) {
        call("select", wanted);
    }
}

void TabsImpl::setOnSelect(std::function<void(std::size_t)> onSelect) {
    relay().on(QStringLiteral("select"), [onSelect = std::move(onSelect)](QVariant const& payload) {
        int const index = payload.toInt();
        if (index >= 0 && onSelect) {
            onSelect(static_cast<std::size_t>(index));
        }
    });
}

int TabsImpl::highlighted() const { return read("highlighted").toInt(); }

// ── Splitter ──────────────────────────────────────────────────────────────

SplitterImpl::SplitterImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
    : ContainerItemWidget{context, QStringLiteral("MorphSplitter"), parent,
                          QVariantMap{{QStringLiteral("axis"), axis == ui::Axis::Horizontal ? 1 : 0}}} {}

void SplitterImpl::setSizes(std::vector<int> const& sizes) {
    QVariantList list;
    for (int const size : sizes) {
        list.append(size);
    }
    write("sizes", list);
    call("apply");
}

void SplitterImpl::setOnResize(std::function<void(std::vector<int>)> onResize) {
    relay().on(QStringLiteral("resize"), [onResize = std::move(onResize)](QVariant const& payload) {
        QVariantList const given = payload.toList();
        std::vector<int> sizes;
        sizes.reserve(static_cast<std::size_t>(given.size()));
        for (QVariant const& size : given) {
            sizes.push_back(size.toInt());
        }
        if (onResize) {
            onResize(std::move(sizes));
        }
    });
}

QQuickItem* SplitterImpl::contentFor(ItemBase& child) {
    static_cast<void>(child);
    // A split view takes its panes through its container API, in `adopted`.
    return nullptr;
}

void SplitterImpl::adopted(ItemBase& child) {
    QObject* const split = part("content");
    if (split != nullptr && child.item() != nullptr) {
        QMetaObject::invokeMethod(split, "addItem", Q_ARG(QQuickItem*, child.item()));
        call("apply");
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a move's two places, in the order moveChild gives them.
void SplitterImpl::reorder(ItemBase& child, std::size_t origin, std::size_t destination) {
    static_cast<void>(child);
    if (QObject* const split = part("content")) {
        QMetaObject::invokeMethod(split, "moveItem", Q_ARG(int, static_cast<int>(origin)),
                                  Q_ARG(int, static_cast<int>(destination)));
    }
}

void SplitterImpl::releasing(ItemBase& child) noexcept {
    QObject* const split = part("content");
    auto const found = std::ranges::find(children(), &child);
    if (split == nullptr || found == children().end()) {
        return;
    }
    QQuickItem* taken = nullptr;
    QMetaObject::invokeMethod(split, "takeItem", Q_RETURN_ARG(QQuickItem*, taken),
                              Q_ARG(int, static_cast<int>(std::distance(children().begin(), found))));
}

// ── Collapsible ───────────────────────────────────────────────────────────

CollapsibleImpl::CollapsibleImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphCollapsible"), parent} {}

void CollapsibleImpl::setTitle(std::string_view title) { write("title", toQString(title)); }

void CollapsibleImpl::setHeader(ui::Widget& header) {
    ItemBase const* const base = baseOf(header);
    auto* const area = qobject_cast<QQuickItem*>(part("headerArea"));
    if (base != nullptr && base->item() != nullptr && area != nullptr) {
        base->item()->setParentItem(area);
    }
}

void CollapsibleImpl::setOpen(bool open) { write("open", open); }

void CollapsibleImpl::setOnToggle(std::function<void(bool)> onToggle) {
    relay().on(QStringLiteral("toggle"), [onToggle = std::move(onToggle)](QVariant const& payload) {
        if (onToggle) {
            onToggle(payload.toBool());
        }
    });
}

void CollapsibleImpl::toggleByUser() const {
    if (!reachable()) {
        return;
    }
    if (QObject* const toggle = item() != nullptr ? item()->findChild<QObject*>(QStringLiteral("toggle")) : nullptr) {
        QMetaObject::invokeMethod(toggle, "clicked");
    }
}

// ── DropZone ──────────────────────────────────────────────────────────────

DropZoneImpl::DropZoneImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphDropZone"), parent} {
    relay().answer(QStringLiteral("takes"), [this](QVariant const& payload) {
        QVariantList const urls = payload.toList();
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(urls.size()));
        for (QVariant const& url : urls) {
            paths.push_back(localPath(url.toString()));
        }
        return takes(paths);
    });
}

void DropZoneImpl::setAccept(std::vector<std::string> const& extensions) { _accept = extensions; }

void DropZoneImpl::setMultiple(bool multiple) { _multiple = multiple; }

void DropZoneImpl::setOnDrop(std::function<void(std::vector<std::string>)> onDrop) {
    relay().on(QStringLiteral("files"), [onDrop = std::move(onDrop)](QVariant const& payload) {
        QVariantList const urls = payload.toList();
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(urls.size()));
        for (QVariant const& url : urls) {
            paths.push_back(localPath(url.toString()));
        }
        if (onDrop) {
            onDrop(std::move(paths));
        }
    });
}

namespace {

std::string lowered(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](unsigned char chr) { return static_cast<char>(std::tolower(chr)); });
    return out;
}

}  // namespace

bool DropZoneImpl::takes(std::vector<std::string> const& paths) const {
    if (paths.empty() || (!_multiple && paths.size() > 1)) {
        return false;
    }
    if (_accept.empty()) {
        return true;
    }
    return std::ranges::all_of(paths, [this](std::string const& path) {
        std::string const name = lowered(path);
        return std::ranges::any_of(_accept, [&name](std::string const& extension) {
            std::string const wanted = lowered(extension);
            return name.size() >= wanted.size() && name.ends_with(wanted);
        });
    });
}

bool DropZoneImpl::offer(std::vector<std::string> const& paths) const {
    if (!reachable()) {
        return false;
    }
    QVariantList urls;
    for (std::string const& path : paths) {
        urls.append(QUrl::fromLocalFile(toQString(path)));
    }
    return askBool("offer", QVariant{urls});
}

}  // namespace morph::qt_quick::detail
