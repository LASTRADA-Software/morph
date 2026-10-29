// SPDX-License-Identifier: Apache-2.0

#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <utility>

#include "qt_quick/keys.hpp"
#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

// ── Stack, Grid ───────────────────────────────────────────────────────────

StackImpl::StackImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
    : ContainerItemWidget{context, QStringLiteral("MorphStack"), parent,
                          QVariantMap{{QStringLiteral("axis"), axis == ui::Axis::Horizontal ? 1 : 0}}},
      _axis{axis} {}

void StackImpl::setGap(int gap) { write("gap", gap); }

void StackImpl::setAlign(ui::Align align) {
    write("align", static_cast<int>(align));
    alignChildren(align);
}

void StackImpl::setJustify(ui::Justify justify) { write("justify", static_cast<int>(justify)); }

void StackImpl::setWrap(bool wrap) {
    _wrap = wrap;
    write("wrap", wrap);
}

namespace {

// Has the container's `place` run again whenever a cell's implicit size or visibility changes. The container's item is
// the connection's context, and the call goes through it, not through the widget, which may be gone while Qt still
// holds the item.
void placeOnChange(QQuickItem* container, QQuickItem const* cell) {
    auto const place = [target = QPointer<QQuickItem>{container}] {
        if (target != nullptr) {
            QMetaObject::invokeMethod(target, "place", Qt::DirectConnection);
        }
    };
    QObject::connect(cell, &QQuickItem::implicitWidthChanged, container, place);
    QObject::connect(cell, &QQuickItem::implicitHeightChanged, container, place);
    QObject::connect(cell, &QQuickItem::visibleChanged, container, place);
}

}  // namespace

void StackImpl::adopted(ItemBase& child) {
    QQuickItem const* const cell = child.item();
    if (_wrap && cell != nullptr && item() != nullptr) {
        placeOnChange(item(), cell);
    }
}

GridImpl::GridImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphGrid"), parent} {}

void GridImpl::setColumns(int columns) { write("columns", std::max(1, columns)); }

void GridImpl::setTracks(std::vector<ui::GridTrack> const& tracks) {
    QVariantList list;
    for (ui::GridTrack const& track : tracks) {
        list.append(QVariantMap{{QStringLiteral("kind"), static_cast<int>(track.width.kind)},
                                {QStringLiteral("amount"), track.width.amount},
                                {QStringLiteral("align"), static_cast<int>(track.align)}});
    }
    write("tracks", list);
}

void GridImpl::adopted(ItemBase& child) {
    QQuickItem const* const cell = child.item();
    if (cell == nullptr || item() == nullptr) {
        return;
    }
    placeOnChange(item(), cell);
}

void GridImpl::place() const {
    if (item() != nullptr) {
        QMetaObject::invokeMethod(item(), "place", Qt::DirectConnection);
    }
}

void GridImpl::setGap(int gap) { write("gap", gap); }

void GridImpl::setAlign(ui::Align align) {
    write("align", static_cast<int>(align));
    alignChildren(align);
}

void GridImpl::setSpan(ui::Widget& child, int span) {
    if (ItemBase const* const base = baseOf(child)) {
        base->write("columnSpan", std::max(1, span));
        place();
    }
}

// ── Panel, Scroll, Slot ───────────────────────────────────────────────────

PanelImpl::PanelImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphPanel"), parent} {}

QObject* PanelImpl::scheme() const {
    return read("surface").toString().isEmpty() ? context().themeObject("cardScheme") : part("scheme");
}

void PanelImpl::setTitle(std::string_view title) { write("title", toQString(title)); }

void PanelImpl::setHeader(ui::Widget& header) {
    ItemBase const* const base = baseOf(header);
    auto* const area = qobject_cast<QQuickItem*>(part("headerArea"));
    if (base != nullptr && base->item() != nullptr && area != nullptr) {
        base->item()->setParentItem(area);
    }
}

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
                          QVariantMap{{QStringLiteral("axis"), axis == ui::Axis::Horizontal ? 1 : 0},
                                      {QStringLiteral("bar"),
                                       QVariant::fromValue(&context.component(QStringLiteral("MorphScrollBar")))}}} {}

SlotImpl::SlotImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphSlot"), parent,
                          QVariantMap{{QStringLiteral("bar"),
                                       QVariant::fromValue(&context.component(QStringLiteral("MorphScrollBar")))}}} {}

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

void TabsImpl::setBarShown(bool shown) { write("barShown", shown); }

void SlotImpl::setScrollable(bool scrollable) { write("scrollable", scrollable); }

int TabsImpl::highlighted() const { return read("highlighted").toInt(); }

std::string TabsImpl::shownText() const {
    QStringList const labels = read("labels").toStringList();
    int const index = highlighted();
    return index >= 0 && index < labels.size() ? toUtf8(labels.at(index)) : std::string{};
}

// ── Splitter ──────────────────────────────────────────────────────────────

SplitterImpl::SplitterImpl(Context& context, ui::ContainerWidget* parent, ui::Axis axis)
    : ContainerItemWidget{
          context, QStringLiteral("MorphSplitter"), parent,
          QVariantMap{{QStringLiteral("axis"), axis == ui::Axis::Horizontal ? 1 : 0},
                      {QStringLiteral("handle"),
                       QVariant::fromValue(&context.component(QStringLiteral("MorphSplitterHandle")))}}} {}

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
