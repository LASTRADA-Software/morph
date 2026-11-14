// SPDX-License-Identifier: Apache-2.0

#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <span>
#include <vector>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

namespace {

// 96 device-independent pixels to 25.4 mm, on both axes, whatever the screen's DPI.
constexpr double kPixelsPerInch = 96.0;
constexpr double kMillimetresPerInch = 25.4;

[[nodiscard]] double pixelsOf(ui::Mm length) noexcept {
    return length.millimetres() * kPixelsPerInch / kMillimetresPerInch;
}

[[nodiscard]] ui::Mm lengthOf(double pixels) { return ui::Mm{pixels * kMillimetresPerInch / kPixelsPerInch}; }

}  // namespace

PageImpl::PageImpl(Context& context, ui::ContainerWidget* parent, ui::PageGeometry const& geometry)
    : ContainerItemWidget{context, QStringLiteral("MorphPage"), parent,
                          QVariantMap{{QStringLiteral("sheetWidth"), pixelsOf(geometry.width)},
                                      {QStringLiteral("sheetHeight"), pixelsOf(geometry.height)}}},
      _geometry{geometry} {
    relay().on(QStringLiteral("available"), [this](QVariant const& payload) {
        // The item reports the width in millimetres.
        _available = ui::Mm{payload.toDouble()};
        relayout();
    });
}

PageImpl::~PageImpl() {
    for (PageRegionImpl* const region : _regions) {
        region->orphan();
    }
}

void PageImpl::join(PageRegionImpl& region) {
    _regions.push_back(&region);
    relayout();
}

void PageImpl::leave(PageRegionImpl& region) noexcept { std::erase(_regions, &region); }

void PageImpl::relayout() {
    std::vector<ui::RegionSpec> specs;
    specs.reserve(_regions.size());
    for (PageRegionImpl const* const region : _regions) {
        specs.push_back({region->geometry(), region->contentHeight()});
    }
    ui::PageLayout const layout = ui::layoutPage(_geometry, specs, _available);
    write("scaleFactor", layout.scale);
    for (std::size_t i = 0; i < _regions.size(); ++i) {
        _regions[i]->show(layout.regions[i]);
    }
}

PageRegionImpl::PageRegionImpl(Context& context, ui::ContainerWidget* parent, ui::RegionGeometry const& geometry)
    : ContainerItemWidget{context, QStringLiteral("MorphPageRegion"), parent},
      _page{dynamic_cast<PageImpl*>(parent)},
      _geometry{geometry} {
    relay().on(QStringLiteral("content"), [this](QVariant const& payload) {
        _contentHeight = lengthOf(payload.toDouble());
        if (_page != nullptr) {
            _page->relayout();
        }
    });
    if (_page != nullptr) {
        _page->join(*this);
    }
}

PageRegionImpl::~PageRegionImpl() {
    if (_page != nullptr) {
        _page->leave(*this);
    }
}

void PageRegionImpl::show(ui::RegionLayout const& layout) const {
    write("x", pixelsOf(layout.rect.x));
    write("y", pixelsOf(layout.rect.y));
    write("rectWidth", pixelsOf(layout.rect.width));
    write("rectHeight", pixelsOf(layout.rect.height));
    write("overflow", layout.overflow);
}

}  // namespace morph::qt_quick::detail
