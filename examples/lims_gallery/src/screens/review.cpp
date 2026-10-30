// SPDX-License-Identifier: Apache-2.0
//
// Design 09, review and approval: the worksheets awaiting review beside the one being reviewed, its flagged results
// compared with the previous lots, its audit trail and comments. The reviewer is A. Meyer, who entered WS-2026-0409
// and so cannot approve it. Returning or rejecting a worksheet needs a reason; approving needs a signature, the
// e-signature of design 01.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "lims_gallery/lims_gallery.hpp"
#include "lims_gallery/screens.hpp"
#include "screen_layout.hpp"

namespace lims_gallery {

namespace {

namespace ui = layout::ui;
using layout::Align;
using layout::ButtonEmphasis;
using Text = ui::Handle<std::string>;
using Flag = ui::Handle<bool>;
using Count = ui::Handle<std::int64_t>;

constexpr char const* kSelfReview = "You entered these results, so another reviewer must approve them.";

struct Tag {
    char const* text;
    char const* tone;
};

struct Sheet {
    char const* id;
    char const* age;
    char const* test;
    char const* analyst;
    char const* product;
    char const* instrument;
    char const* submitted;
    // Whether the reviewer entered its results.
    bool self;
    // The queue entry's tags; the compact head adds suitability to them.
    std::vector<Tag> tags;
    // The worksheet head's tags beside its result count.
    std::vector<Tag> summary;
    std::vector<Tag> compactSummary;
};

std::vector<Sheet> const& sheets() {
    static std::vector<Sheet> const kSheets{
        {.id = "WS-2026-0412",
         .age = "2 h 10 min",
         .test = "Assay and impurities",
         .analyst = "M. Kaur",
         .product = "Paracetamol 500 mg",
         .instrument = "HPLC-02",
         .submitted = "24-Oct-2026 07:30 CEST",
         .self = false,
         .tags = {{.text = "1 out of spec", .tone = "err"}, {.text = "1 changed", .tone = "warn"}},
         .summary = {{.text = "1 out of spec · investigation OOS-2026-031", .tone = "err"},
                     {.text = "1 changed after entry", .tone = "warn"},
                     {.text = "System suitability passed", .tone = "ok"}},
         .compactSummary = {{.text = "1 out of spec", .tone = "err"},
                            {.text = "1 changed", .tone = "warn"},
                            {.text = "Suitability passed", .tone = "ok"}}},
        {.id = "WS-2026-0409",
         .age = "1 h 05 min",
         .test = "Water (KF)",
         .analyst = "A. Meyer",
         .product = "Lactose monohydrate",
         .instrument = "Karl Fischer",
         .submitted = "24-Oct-2026 08:35 CEST",
         .self = true,
         .tags = {},
         .summary = {{.text = "All within spec", .tone = "ok"}},
         .compactSummary = {{.text = "All within spec", .tone = "ok"}}},
        {.id = "WS-2026-0405",
         .age = "48 min",
         .test = "Dissolution",
         .analyst = "J. Ruiz",
         .product = "Ibuprofen 400 mg",
         .instrument = "Dissolution bath 1",
         .submitted = "24-Oct-2026 08:52 CEST",
         .self = false,
         .tags = {{.text = "All within spec", .tone = "ok"}},
         .summary = {{.text = "All within spec", .tone = "ok"}},
         .compactSummary = {{.text = "All within spec", .tone = "ok"}}},
        {.id = "WS-2026-0398",
         .age = "20 min",
         .test = "Identity (IR)",
         .analyst = "M. Kaur",
         .product = "Paracetamol 500 mg",
         .instrument = "FTIR-01",
         .submitted = "24-Oct-2026 09:20 CEST",
         .self = false,
         .tags = {{.text = "All within spec", .tone = "ok"}},
         .summary = {{.text = "All within spec", .tone = "ok"}},
         .compactSummary = {{.text = "All within spec", .tone = "ok"}}},
    };
    return kSheets;
}

struct Result {
    char const* sheet;
    char const* key;
    char const* sample;
    char const* analyte;
    char const* value;
    char const* spec;
    char const* previous;
    char const* evaluation;
    char const* tone;
    char const* change;
    bool flagged;
};

// WS-2026-0412's results are design 08's; the other worksheets hold one result each.
constexpr std::array<Result, 11> kResults{{
    {.sheet = "WS-2026-0412",
     .key = "ib2",
     .sample = "S-26-104170",
     .analyte = "Impurity B",
     .value = "0.21 %",
     .spec = "≤ 0.20 %",
     .previous = "0.09 %",
     .evaluation = "Out of spec: above 0.20",
     .tone = "err",
     .change = "0.12 → 0.21 · integration corrected",
     .flagged = true},
    {.sheet = "WS-2026-0412",
     .key = "mean",
     .sample = "S-26-104176",
     .analyte = "Assay, mean",
     .value = "99.4 % LC",
     .spec = "95.0–105.0",
     .previous = "100.1 % LC",
     .evaluation = "Out of trend: −0.7 % vs. last 5 lots",
     .tone = "warn",
     .change = "—",
     .flagged = true},
    {.sheet = "WS-2026-0412",
     .key = "a1",
     .sample = "S-26-104176",
     .analyte = "Assay, prep 1",
     .value = "99.6 % LC",
     .spec = "95.0–105.0",
     .previous = "—",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0412",
     .key = "a2",
     .sample = "S-26-104176",
     .analyte = "Assay, prep 2",
     .value = "99.2 % LC",
     .spec = "95.0–105.0",
     .previous = "—",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0412",
     .key = "ia1",
     .sample = "S-26-104176",
     .analyte = "Impurity A",
     .value = "0.04 %",
     .spec = "≤ 0.10 %",
     .previous = "—",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0412",
     .key = "ia2",
     .sample = "S-26-104170",
     .analyte = "Impurity A",
     .value = "0.03 %",
     .spec = "≤ 0.10 %",
     .previous = "—",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0412",
     .key = "total",
     .sample = "S-26-104170",
     .analyte = "Total impurities",
     .value = "0.24 %",
     .spec = "≤ 0.50 %",
     .previous = "0.12 %",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0409",
     .key = "kf",
     .sample = "S-26-104198",
     .analyte = "Water (KF)",
     .value = "0.3 %",
     .spec = "≤ 0.5 %",
     .previous = "0.3 %",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0405",
     .key = "diss",
     .sample = "S-26-104176",
     .analyte = "Dissolution, 30 min",
     .value = "92 %",
     .spec = "Q = 80 %",
     .previous = "94 %",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0398",
     .key = "ir",
     .sample = "S-26-104150",
     .analyte = "Identity (IR)",
     .value = "Conforms",
     .spec = "Conforms to reference",
     .previous = "Conforms",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
    {.sheet = "WS-2026-0398",
     .key = "ir-match",
     .sample = "S-26-104150",
     .analyte = "Spectrum match",
     .value = "99.2 %",
     .spec = "≥ 95 %",
     .previous = "99.0 %",
     .evaluation = "Within spec",
     .tone = "ok",
     .change = "—",
     .flagged = false},
}};

struct Lot {
    char const* lot;
    char const* made;
    char const* assay;
    char const* impurityB;
};

// The product's lots, newest first: the two of this worksheet, then the five its trend compares with.
constexpr std::array<Lot, 7> kLots{{
    {.lot = "L-26-0381 · this worksheet", .made = "12-Oct-2026", .assay = "99.4 % LC", .impurityB = "—"},
    {.lot = "L-26-0377 · this worksheet", .made = "28-Sep-2026", .assay = "—", .impurityB = "0.21 %"},
    {.lot = "L-26-0362", .made = "02-Sep-2026", .assay = "100.1 % LC", .impurityB = "0.09 %"},
    {.lot = "L-26-0355", .made = "14-Aug-2026", .assay = "100.4 % LC", .impurityB = "0.08 %"},
    {.lot = "L-26-0349", .made = "29-Jul-2026", .assay = "99.8 % LC", .impurityB = "0.10 %"},
    {.lot = "L-26-0341", .made = "10-Jul-2026", .assay = "100.0 % LC", .impurityB = "0.07 %"},
    {.lot = "L-26-0333", .made = "22-Jun-2026", .assay = "100.2 % LC", .impurityB = "0.09 %"},
}};

struct Entry {
    char const* when;
    char const* who;
    char const* what;
};

constexpr std::array<Entry, 3> kAudit{{
    {.when = "24-Oct-2026 07:12",
     .who = "M. Kaur",
     .what = "Impurity B of S-26-104170 changed 0.12 → 0.21 %. Reason: integration corrected."},
    {.when = "24-Oct-2026 07:30", .who = "M. Kaur", .what = "Submitted for review."},
    {.when = "24-Oct-2026 07:31", .who = "M. Kaur", .what = "Comment added."},
}};

constexpr Entry kComment{
    .when = "24-Oct-2026 07:31",
    .who = "M. Kaur",
    .what = "Impurity B reintegrated, chromatogram attached to OOS-2026-031. Phase 1 points to a lab error."};

ui::AnyHandle tagList(std::vector<Tag> const& tags) {
    std::vector<ui::AnyHandle> items;
    items.reserve(tags.size());
    for (Tag const& tag : tags) {
        items.push_back(ui::obj({{"text", tag.text}, {"tone", tag.tone}}));
    }
    return ui::list(items);
}

ui::AnyHandle queueList() {
    std::vector<ui::AnyHandle> items;
    items.reserve(sheets().size());
    for (Sheet const& sheet : sheets()) {
        items.push_back(ui::obj({{"id", sheet.id},
                                 {"age", sheet.age},
                                 {"test", sheet.test},
                                 {"analyst", sheet.analyst},
                                 {"product", sheet.product},
                                 {"instrument", sheet.instrument},
                                 {"submitted", sheet.submitted},
                                 {"self", sheet.self},
                                 {"tags", tagList(sheet.tags)},
                                 {"summary", tagList(sheet.summary)},
                                 {"compactSummary", tagList(sheet.compactSummary)}}));
    }
    return ui::list(items);
}

ui::AnyHandle resultList() {
    std::vector<ui::AnyHandle> items;
    items.reserve(kResults.size());
    for (Result const& result : kResults) {
        items.push_back(ui::obj({{"sheet", result.sheet},
                                 {"key", result.key},
                                 {"sample", result.sample},
                                 {"analyte", result.analyte},
                                 {"value", result.value},
                                 {"spec", result.spec},
                                 {"previous", result.previous},
                                 {"evaluation", result.evaluation},
                                 {"tone", result.tone},
                                 {"change", result.change},
                                 {"flagged", result.flagged}}));
    }
    return ui::list(items);
}

ui::AnyHandle lotList() {
    std::vector<ui::AnyHandle> items;
    items.reserve(kLots.size());
    for (Lot const& lot : kLots) {
        items.push_back(ui::obj({{"sheet", "WS-2026-0412"},
                                 {"lot", lot.lot},
                                 {"made", lot.made},
                                 {"assay", lot.assay},
                                 {"impurityB", lot.impurityB}}));
    }
    return ui::list(items);
}

ui::AnyHandle entryList(std::vector<Entry> const& entries) {
    std::vector<ui::AnyHandle> items;
    items.reserve(entries.size());
    std::int64_t index = 0;
    for (Entry const& entry : entries) {
        items.push_back(ui::obj({{"sheet", "WS-2026-0412"},
                                 {"key", index++},
                                 {"when", entry.when},
                                 {"who", entry.who},
                                 {"what", entry.what}}));
    }
    return ui::list(items);
}

// The review's state: the queue, the worksheet being reviewed, and the decision being made about it.
struct Review {
    ui::Element decisionDialog = ui::element("decisionDialog");
    ui::AnyHandle width;
    ui::AnyHandle queue;
    Text selected;
    Text search;
    Text show;
    Text comment;
    Text decision;
    Flag decisionOpen;
    Flag showQueue;
    ui::AnyHandle results;
    ui::AnyHandle lots;
    ui::AnyHandle audit;
    ui::AnyHandle comments;
    Count waiting;
    Flag self;
    ui::AnyHandle shownResults;
    Count resultCount;
    Count flaggedCount;
    Count auditCount;
    Count commentCount;
    // The worksheet the e-signature last answered for, and the meaning it was signed with.
    Text signedSheet;
    Text signedAs;
};

// A field of the worksheet being reviewed.
ui::AnyHandle pick(Review const& review, char const* field) {
    auto const selected = ui::filter(review.queue, "sheet", [&review](ui::AnyHandle const& sheet) {
        return ui::eq(sheet.at("id"), review.selected);
    });
    return ui::call("first",
                    {ui::map(selected, "sheet", [field](ui::AnyHandle const& sheet) { return sheet.at(field); })});
}

ui::AnyHandle ofSelected(Review const& review, ui::AnyHandle const& list) {
    return ui::filter(list, "item",
                      [&review](ui::AnyHandle const& item) { return ui::eq(item.at("sheet"), review.selected); });
}

Review declare(ui::Scope& screen) {
    Review review;
    review.width = layout::widthClass(screen);
    review.queue = screen.state<ui::Any>("queue", queueList());
    review.selected = screen.state("selected", std::string{"WS-2026-0412"});
    review.search = screen.state("search", std::string{});
    review.show = screen.state("show", std::string{"flagged"});
    review.comment = screen.state("comment", std::string{});
    review.decision = screen.state("decision", std::string{"return"});
    review.decisionOpen = screen.state("decisionOpen", false);
    review.showQueue = screen.state("showQueue", false);
    review.signedSheet = screen.state("signedSheet", std::string{});
    review.signedAs = screen.state("signedAs", std::string{});
    review.results = screen.let("results", resultList());
    review.lots = screen.let("lots", lotList());
    review.audit = screen.let("audit", entryList({kAudit.begin(), kAudit.end()}));
    review.comments = screen.let("comments", entryList({kComment}));
    review.waiting = screen.let("waiting", ui::call("count", {review.queue}));
    review.self = screen.let("selfEntered", Flag{ui::coalesce({pick(review, "self"), ui::AnyHandle{false}})});
    auto const mine = screen.let("sheetResults", ofSelected(review, review.results));
    review.resultCount = screen.let("resultCount", ui::call("count", {mine}));
    review.flaggedCount =
        screen.let("flaggedCount", ui::call("count", {ui::filter(mine, "result", [](ui::AnyHandle const& result) {
                                                return result.at("flagged");
                                            })}));
    review.shownResults =
        screen.let("shownResults", ui::filter(mine, "result", [&review](ui::AnyHandle const& result) -> ui::AnyHandle {
                       return ui::eq(review.show, "all") || Flag{result.at("flagged")};
                   }));
    review.auditCount = screen.let("auditCount", ui::call("count", {ofSelected(review, review.audit)}));
    review.commentCount = screen.let("commentCount", ui::call("count", {ofSelected(review, review.comments)}));
    return review;
}

ui::Node tagRow(ui::AnyHandle const& tags, std::string const& testId) {
    return ui::forEach(tags, {.key = ui::row("text"), .axis = "horizontal", .gap = 1},
                       [](ui::DeclScope&) { return ui::Nodes{layout::tag(ui::row("text"), ui::row("tone"))}; })
        .testId(testId);
}

// ── The queue ────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Commands choose(Review const& review) {
    return {ui::set(review.selected, ui::row("id")), ui::set(review.show, std::string{"flagged"}),
            ui::set(review.comment, std::string{}), ui::set(review.showQueue, false)};
}

// One entry of the queue: the whole card takes the click and the current mark; its label names it.
ui::Node queueEntry(Review const& review) {
    auto const content = ui::column(
        {ui::text(
             {.text = ui::format("{0} · {1}", {ui::row("id"), ui::row("age")}),
              .spans = ui::spans({ui::styled(ui::row("id"), {.strong = true, .mono = true}), " · ", ui::row("age")})}),
         ui::text({.text = ui::format("{0} · {1}", {ui::row("test"), ui::row("analyst")}), .role = "muted"}),
         tagRow(ui::row("tags"), "queue-tags"),
         layout::muted(kSelfReview).visible(Flag{ui::row("self")}).testId("queue-self")},
        {.gap = 1});
    return ui::button({.label = ui::format("{0} · {1}", {ui::row("id"), ui::row("age")}),
                       .onClick = choose(review),
                       .style = morph::ui::ButtonStyle::Item,
                       .selected = ui::eq(review.selected, ui::row("id")),
                       .children = {content}})
        .testId("queue-entry");
}

ui::AnyHandle filteredQueue(Review const& review) {
    return ui::filter(review.queue, "sheet", [&review](ui::AnyHandle const& sheet) -> ui::AnyHandle {
        return ui::call("contains", {ui::concat({sheet.at("id"), " ", sheet.at("test"), " ", sheet.at("analyst")}),
                                     review.search});
    });
}

ui::Node queueTitle(Review const& review) {
    return ui::text({.text = ui::format("Awaiting review · {0}", {review.waiting}), .role = "heading"})
        .testId("queue-title");
}

ui::Node queueSearch(Review const& review) {
    return ui::textInput({.value = review.search,
                          .label = "Search the queue",
                          .placeholder = "Worksheet, lot or analyst",
                          .onChange = ui::set(review.search, ui::event())})
        .testId("queue-search");
}

// The queue beside the worksheet: its head is padded, and its entries span the pane edge to edge, divided, so an
// entry's tint and selection bar reach the pane's edges.
ui::Node queuePane(Review const& review) {
    return ui::column({ui::column({queueTitle(review), queueSearch(review)}, {.gap = 2, .padding = 3}),
                       ui::forEach(filteredQueue(review), {.key = ui::row("id"), .gap = 0, .divided = true},
                                   [&review](ui::DeclScope&) { return ui::Nodes{queueEntry(review)}; })
                           .testId("queue")},
                      {.gap = 0});
}

// The phone's queue, a page of its own: its entries sit inside the page's inset.
ui::Node compactQueue(Review const& review) {
    return ui::column({queueTitle(review), queueSearch(review),
                       ui::forEach(filteredQueue(review), {.key = ui::row("id"), .gap = 3},
                                   [&review](ui::DeclScope&) { return ui::Nodes{queueEntry(review)}; })
                           .testId("queue")},
                      {.gap = 2});
}

// ── The worksheet ────────────────────────────────────────────────────────────────────────────────────────────────

ui::Node showChoice(Review const& review) {
    auto const options =
        ui::list({ui::obj({{"key", "all"}, {"label", ui::format("All {0}", {review.resultCount})}}),
                  ui::obj({{"key", "flagged"}, {"label", ui::format("Flagged {0}", {review.flaggedCount})}})});
    return ui::row({layout::muted("Show"), ui::select({.options = options,
                                                       .selected = review.show,
                                                       .label = "Show results",
                                                       .style = "segmented",
                                                       .onChoose = ui::set(review.show, ui::event())})
                                               .testId("show")},
                   {.gap = 1, .align = Align::Center});
}

ui::Node resultTable(Review const& review) {
    std::vector<ui::TableColumn> const columns{
        {.name = "sample", .label = "Sample", .width = ui::fixed(11), .role = "mono"},
        {.name = "analyte", .label = "Analyte", .width = ui::content(), .wrap = false},
        {.name = "value", .label = "Result", .width = ui::fixed(10), .align = Align::End, .wrap = false},
        {.name = "spec", .label = "Specification", .width = ui::content(), .wrap = false},
        {.name = "previous", .label = "Previous lot", .width = ui::content(), .align = Align::End, .wrap = false},
        {.name = "evaluation",
         .label = "Evaluation",
         .width = ui::fixed(28),
         .cell = layout::tag(ui::row("evaluation"), ui::row("tone"))},
        {.name = "change",
         .label = "Change",
         .width = ui::stretch(),
         .cell = ui::text({.text = ui::row("change"), .role = "muted"}),
         .wrap = false}};
    return ui::table(review.shownResults, {.key = ui::row("key"),
                                           .columns = columns,
                                           .rowTone = ui::cond(ui::eq(ui::row("tone"), "err"), ui::AnyHandle{"err"},
                                                               ui::AnyHandle{nullptr})})
        .a11y("Results under review")
        .testId("results");
}

ui::Node resultCards(Review const& review) {
    return ui::forEach(
               review.shownResults, {.key = ui::row("key"), .gap = 2},
               [](ui::DeclScope&) {
                   return ui::Nodes{
                       ui::panel(
                           {ui::row({ui::text({.text = ui::row("sample"),
                                               .spans = ui::spans(
                                                   {ui::styled(ui::row("sample"), {.strong = true, .mono = true})})})
                                         .sizing(ui::stretch()),
                                     ui::text({.text = ui::row("value"), .role = "section"})},
                                    {.gap = 1}),
                            ui::text({.text = ui::format("{0} · spec {1} · previous lot {2}",
                                                         {ui::row("analyte"), ui::row("spec"), ui::row("previous")})}),
                            layout::tag(ui::row("evaluation"), ui::row("tone")),
                            layout::muted(ui::format("Changed {0}", {ui::row("change")}))
                                .visible(ui::ne(ui::row("change"), "—"))},
                           {.padding = 2})
                           // An out-of-specification card carries the tint the table's row has.
                           .surface(ui::cond(ui::eq(ui::row("tone"), "err"), Text{"outOfSpec"}, Text{""}))};
               })
        .testId("results");
}

ui::Node commentCards(Review const& review) {
    return ui::forEach(
               ofSelected(review, review.comments), {.key = ui::row("key"), .gap = 2},
               [](ui::DeclScope&) {
                   return ui::Nodes{ui::panel(
                       {ui::text({.text = ui::format("Comment from {0} · {1}", {ui::row("who"), ui::row("when")}),
                                  .role = "section"}),
                        ui::text({.text = ui::row("what")})},
                       {.padding = 2})};
               })
        .testId("comments");
}

ui::Node reviewComment(Review const& review) {
    return layout::captioned(ui::textInput({.value = review.comment,
                                            .label = "Review comment",
                                            .placeholder = "Required to reject or return",
                                            .mode = "multiline",
                                            .onChange = ui::set(review.comment, ui::event())}))
        .testId("review-comment");
}

ui::Node lotTable(Review const& review) {
    std::vector<ui::TableColumn> const columns{
        {.name = "lot", .label = "Lot", .width = ui::stretch()},
        {.name = "made", .label = "Made", .width = ui::fixed(12)},
        {.name = "assay", .label = "Assay, mean", .width = ui::fixed(12), .align = Align::End},
        {.name = "impurityB", .label = "Impurity B", .width = ui::fixed(11), .align = Align::End}};
    return ui::column(
        {ui::table(ofSelected(review, review.lots),
                   {.key = ui::row("lot"),
                    .columns = columns,
                    .empty = {ui::emptyState({.title = "No lots to compare",
                                              .text = "This product has no earlier lots tested by this method."})}})
             .a11y("Previous lots")
             .testId("lots"),
         layout::muted("The last 5 lots average 100.1 % LC for the assay; the latest had 0.09 % Impurity B.")
             .visible(Count{ui::call("count", {ofSelected(review, review.lots)})} > 0)},
        {.gap = 2});
}

ui::Node auditList(Review const& review) {
    return ui::column({ui::forEach(ofSelected(review, review.audit), {.key = ui::row("key"), .gap = 2},
                                   [](ui::DeclScope&) {
                                       return ui::Nodes{ui::column(
                                           {layout::muted(ui::format("{0} · {1}", {ui::row("when"), ui::row("who")})),
                                            ui::text({.text = ui::row("what")})},
                                           {.gap = 0})};
                                   }),
                       layout::muted("No entries since the worksheet was submitted.")
                           .visible(ui::eq(review.auditCount, std::int64_t{0}))},
                      {.gap = 2})
        .testId("audit");
}

// The record's tabs, whose counts follow the selected worksheet; the reviewer's pick is kept across worksheets.
ui::Node recordTabs(Review const& review, bool compact) {
    // A line of room above the comment's caption, which sits close under the cards otherwise.
    auto const room = ui::spacer().sizing(ui::fixed(1), ui::fixed(1));
    ui::Nodes const resultsPage{showChoice(review), compact ? resultCards(review) : resultTable(review),
                                commentCards(review), room, reviewComment(review)};
    return ui::tabs({{.label = "Results", .children = resultsPage},
                     {.label = compact ? "Previous" : "Previous lots", .children = lotTable(review)},
                     {.label = ui::format(compact ? "Audit {0}" : "Audit trail · {0}", {review.auditCount}),
                      .children = auditList(review)},
                     {.label = ui::format(compact ? "Comments {0}" : "Comments · {0}", {review.commentCount}),
                      .children = commentCards(review)}})
        .testId("record-tabs");
}

ui::Node sheetHead(Review const& review, bool compact) {
    auto const line =
        compact
            ? ui::format("{0} · {1} · {2}", {pick(review, "id"), pick(review, "analyst"), pick(review, "submitted")})
            : ui::format("{0} · submitted {1} by {2} · {3}", {pick(review, "id"), pick(review, "submitted"),
                                                              pick(review, "analyst"), pick(review, "instrument")});
    auto const summary = ui::concatLists(
        {ui::list({ui::obj({{"text", ui::format("{0} results", {review.resultCount})}, {"tone", "neutral"}})}),
         pick(review, compact ? "compactSummary" : "summary")});
    return ui::column({layout::muted(line).testId("sheet-line"),
                       ui::text({.text = ui::format("{0} · {1}", {pick(review, "test"), pick(review, "product")}),
                                 .role = "heading"})
                           .testId("sheet-title"),
                       tagRow(summary, "sheet-tags")},
                      {.gap = 1});
}

ui::Node selfBanner(Review const& review) {
    return ui::banner({.tone = "warn", .text = kSelfReview}).visible(review.self).testId("self-review");
}

// The signature the e-signature answered with, for the worksheet it signed.
ui::Node signedBanner(Review const& review) {
    return ui::banner({.tone = "ok", .text = ui::format("{0} signed: {1}.", {review.signedSheet, review.signedAs})})
        .visible(ui::eq(review.signedSheet, review.selected))
        .testId("signed");
}

// ── Decisions ────────────────────────────────────────────────────────────────────────────────────────────────────

ui::Commands decide(Review const& review, char const* decision) {
    return {ui::set(review.decision, std::string{decision}), ui::set(review.decisionOpen, true)};
}

// The decided worksheet leaves the queue, and the next one is reviewed.
ui::Commands confirmDecision(Review const& review) {
    auto const remaining = ui::filter(review.queue, "sheet", [&review](ui::AnyHandle const& sheet) {
        return ui::ne(sheet.at("id"), review.selected);
    });
    auto const message =
        ui::cond(ui::eq(review.decision, "reject"), Text{ui::format("{0} is rejected.", {review.selected})},
                 Text{ui::format("{0} is returned to the analyst.", {review.selected})});
    return {
        ui::notify(message), ui::set(review.queue, remaining),
        ui::set(review.selected,
                ui::coalesce({ui::call("first", {ui::map(review.queue, "sheet",
                                                         [](ui::AnyHandle const& sheet) { return sheet.at("id"); })}),
                              ui::AnyHandle{""}})),
        ui::set(review.comment, std::string{}), ui::set(review.decisionOpen, false)};
}

Flag hasReason(Review const& review) { return !ui::isEmpty(ui::call("trim", {review.comment})); }

ui::Node decisionDialog(Review const& review) {
    Flag const reject = ui::eq(review.decision, "reject");
    ui::Commands const close = ui::set(review.decisionOpen, false);
    return ui::dialog(
               review.decisionDialog,
               {layout::muted(ui::cond(reject,
                                       Text{"The worksheet is closed and its results are not reported. The reason is "
                                            "kept with the record."},
                                       Text{"The analyst gets the worksheet back with your reason, to correct and "
                                            "submit again."})),
                layout::captioned(ui::textInput({.value = review.comment,
                                                 .label = "Reason",
                                                 .mode = "multiline",
                                                 .onChange = ui::set(review.comment, ui::event())}))
                    .required(true)
                    .testId("decision-reason")},
               {.title = ui::cond(reject, Text{ui::format("Reject {0}", {review.selected})},
                                  Text{ui::format("Return {0} to the analyst", {review.selected})}),
                .open = review.decisionOpen,
                .showErrors = ui::ShowErrors::Always,
                .cancel = close,
                .onDismiss = close,
                .actions = {{.id = "cancel", .label = "Cancel", .onClick = close},
                            {.id = "confirm",
                             .label = ui::cond(reject, Text{"Reject"}, Text{"Return to analyst"}),
                             .emphasis = ButtonEmphasis::Primary,
                             .enabled = hasReason(review),
                             .onClick = confirmDecision(review)}}})
        .sizing(ui::fixed(58))
        .testId("decision");
}

// Approving opens design 01's signature for the worksheet as a dialog over this screen; the meaning it answers with
// marks the worksheet signed.
ui::Commands approve(Review const& review) {
    return ui::navigate("lims-gallery.esignature", {.params = ui::obj({{"worksheet", review.selected}}),
                                                    .as = ui::OpenAs::Dialog,
                                                    .onResult = {ui::set(review.signedSheet, review.selected),
                                                                 ui::set(review.signedAs, ui::event())}});
}

ui::Node returnButton(Review const& review, char const* label) {
    return ui::button({.label = label, .onClick = decide(review, "return")}).testId("return");
}

ui::Node rejectButton(Review const& review, char const* label) {
    return ui::button({.label = label, .emphasis = ButtonEmphasis::DangerOutline, .onClick = decide(review, "reject")})
        .testId("reject");
}

ui::Node approveButton(Review const& review, char const* label) {
    return ui::button({.label = label, .emphasis = ButtonEmphasis::Primary, .onClick = approve(review)})
        .enabled(!review.self)
        .tooltip(ui::cond(review.self, Text{kSelfReview}, Text{"Approving needs your signature"}))
        .testId("approve");
}

// ── Layouts ──────────────────────────────────────────────────────────────────────────────────────────────────────

// The worksheet scrolls above the decisions, which stay at the pane's bottom.
ui::Node worksheetPane(Review const& review) {
    return ui::pinned(
        {ui::column({sheetHead(review, false), selfBanner(review), signedBanner(review), recordTabs(review, false)},
                    {.gap = 2})},
        {.bottom = ui::row({returnButton(review, "Return to analyst…"), rejectButton(review, "Reject…"),
                            ui::spacer().sizing(ui::stretch()),
                            layout::muted(ui::cond(review.self, Text{"Another reviewer must approve"},
                                                   Text{"Approving needs your signature"}))
                                .testId("approve-note"),
                            approveButton(review, "Approve and sign…")},
                           {.gap = 1, .align = Align::Center})});
}

ui::Node expandedLayout(Review const& review) {
    // The design's 280 px queue beside the worksheet, in the frontend's character widths. The splitter takes the
    // page's height, so each pane reaches the page's bottom.
    auto const sizes = ui::AnyHandle{std::vector<std::int64_t>{29, 74}};
    return ui::column({ui::splitter({queuePane(review).surface("pane"), worksheetPane(review).surface("pane")},
                                    ui::AnyHandle{"horizontal"}, sizes)
                           .sizing(ui::stretch(), ui::stretch())
                           .a11y("Queue and worksheet")
                           .testId("split"),
                       decisionDialog(review)},
                      {.pageEdge = true})
        .sizing(ui::stretch(), ui::stretch());
}

// The decisions stay at the bottom of the phone screen while the worksheet scrolls above them.
ui::Node compactWorksheet(Review const& review) {
    return ui::pinned({ui::column({ui::button({.label = ui::format("‹ Awaiting review · {0}", {review.waiting}),
                                               .emphasis = ButtonEmphasis::Ghost,
                                               .onClick = ui::set(review.showQueue, true)})
                                       .testId("back"),
                                   sheetHead(review, true), selfBanner(review), signedBanner(review),
                                   recordTabs(review, true), decisionDialog(review)},
                                  {.gap = 2})},
                      {.bottom = ui::row({returnButton(review, "Return…").sizing(ui::stretch()),
                                          rejectButton(review, "Reject…").sizing(ui::stretch()),
                                          approveButton(review, "Approve…").sizing(ui::stretch(2))},
                                         {.gap = 1})});
}

ui::Node compactLayout(Review const& review) {
    return ui::switchOn(review.showQueue, {ui::Case{true, {ui::pinned(compactQueue(review))}}},
                        {compactWorksheet(review)})
        .sizing(ui::stretch(), ui::stretch());
}

}  // namespace

ui::Screen reviewScreen() {
    return ui::screen("lims-gallery.review", [](ui::Scope& screen) {
        Review const review = declare(screen);
        screen.title("Review and approval");
        return layout::byWidth(review.width, compactLayout(review), expandedLayout(review))
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace lims_gallery
