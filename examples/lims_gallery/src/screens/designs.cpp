// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <span>

#include "lims_gallery/screens.hpp"

namespace lims_gallery {

// The one list of designs. A design is built by giving it its screen's builder; until then the index lists it as
// unavailable and the bundle leaves it out.
std::span<Design const> designs() {
    static std::array<Design, 10> const kDesigns{{
        {.number = "01",
         .id = "lims-gallery.esignature",
         .title = "E-signature",
         .summary = "A dialog over the screen it signs",
         .make = &eSignatureScreen},
        {.number = "02",
         .id = "lims-gallery.receipt",
         .title = "Sample receipt",
         .summary = "Scan, check, receive, next",
         .make = &receiptScreen},
        {.number = "03",
         .id = "lims-gallery.instrument",
         .title = "Instrument record",
         .summary = "Record with status, tabs and actions",
         .make = &instrumentScreen},
        {.number = "04",
         .id = "lims-gallery.audit",
         .title = "Audit trail",
         .summary = "Facets beside a read-only table",
         .make = &auditScreen},
        {.number = "05",
         .id = "lims-gallery.samples",
         .title = "Sample list",
         .summary = "A dense table with bulk actions",
         .make = &samplesScreen},
        {.number = "06",
         .id = "lims-gallery.stability",
         .title = "Stability study setup",
         .summary = "Conditions × timepoints grid and a live summary",
         .make = &stabilityScreen},
        {.number = "07",
         .id = "lims-gallery.plate",
         .title = "Plate layout",
         .summary = "Drag samples into wells, or select wells and assign",
         .make = &plateScreen},
        {.number = "08",
         .id = "lims-gallery.results",
         .title = "Result entry",
         .summary = "A worksheet grid, change reason on edit",
         .make = &resultsScreen},
        {.number = "09",
         .id = "lims-gallery.review",
         .title = "Review and approval",
         .summary = "Queue and worksheet side by side",
         .make = nullptr},
        {.number = "10",
         .id = "lims-gallery.console",
         .title = "Lab operations console",
         .summary = "Shell, dashboard, worklist beside a non-modal sample pane",
         .make = nullptr},
    }};
    return kDesigns;
}

}  // namespace lims_gallery
