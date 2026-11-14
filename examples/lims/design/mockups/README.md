# lims mockups

The visual target for lims's GUI: the application shell and one mockup per screen of the design's Screens table
(`docs/superpowers/specs/2026-11-09-lims-erp-design.md`, Part D). They show what a contract laboratory would pay for,
not what ui/1 says today; each screen is later compared with its ui/1 rendering by side-by-side grabs, and each gap
listed below is a candidate vocabulary or frontend change in morph.

Each `.dc.html` file is self-contained HTML in the format of `examples/lims_gallery/design/mockups`: styles inline,
one row per layout, an expanded window (1215 × n px, desktop) beside a compact one (390 px, phone), and further rows
for the states that matter. The `support.js` the format references is not needed to view a file; open it in a
browser. The lab screens (14–18) put the gallery's designs 05, 02/10, 08, 09 with the e-signature, and 04 into the
shell, keyed by sample rather than worksheet, since lims has no worksheets.

## Scenario

A German contract laboratory, **Rheinlab Analytik GmbH** (EUR, fiscal year from January, VAT 19 %, sites Köln and
Leverkusen), and its subsidiary **Rheinlab Japan K.K.** (JPY, zero decimals, fiscal year from April, site Osaka). Today
is Monday 9 November 2026; October is closing, September is closed. People: Dr. Anna Meyer (lab director; verifier,
invoice approver up to €10,000.00, requisition approver up to €5,000.00), Manpreet Kaur (analyst), Lena Fischer and
Markus Weber (billing clerks), Julia Schneider (finance controller, approver up to €50,000.00), Tobias Krüger (stores),
Kenji Sato (Osaka). Figures agree across screens: invoice totals are net plus 19 % VAT rounded half up, the trial
balance is computed from balanced entries, and the goods receipt's CHF lines lose a cent against their total, which
the entry carries as an explicit exchange-rounding line.

## Design language

| Token | Value |
|---|---|
| Brand | `#17784A`; hover `#0F5A36`; tint `#EAF5EE`, tint border `#BCDFC9`, focus ring `0 0 0 3px #CDEBD8` |
| Text | primary `#1C1E21`, secondary `#363C46`, muted `#4D5461`, faint `#7A828F` |
| Surfaces | canvas `#EEF0F3`, page `#F6F7F9`, card and bar `#FFFFFF`, table head and group rows `#FAFBFC`, read-only input `#F3F5F7` |
| Lines | card `#DDE1E6`, control `#C3C9D2`, row rule `#EDF0F3`, bar rule `#E1E5EA` |
| Sidebar | `#1C1E21`, selected `#2B3038` with a `#3FB57A` inset bar, text `#C9CFD8`, headings `#7A828F` |
| Tones (badge fill / text) | ok `#E2F4E8`/`#17602F`, warn `#FFF1D6`/`#7A4A00`, err `#FDE7E7`/`#9B1C1C`, info `#E6ECFF`/`#1F3FA8`, neutral `#EEF0F3`/`#363C46` |
| Banners (fill / border / text) | err `#FDECEC`/`#F2C0C0`/`#8A1C1C`, warn `#FFF6E5`/`#F1D49A`/`#6B4100`, info `#EEF2FF`/`#C9D4F5`/`#1F3A8A`, ok `#EAF5EE`/`#BCDFC9`/`#17602F` |
| Row states | out of specification `#FFF5F5`; selected `#F2F9F5` with a 3 px brand bar at the row's start |
| Type | Inter; JetBrains Mono 12 px for record numbers, codes and lots; tabular figures everywhere |
| Type scale | 11 badges, table heads, overlines · 12 labels, help, captions · **13 body** · 14 compact inputs, section in dialogs · 15 dialog and compact titles · 17 page title · 22 KPI value |
| Spacing | 4 px grid: 4, 6, 8, 10, 12, 14, 16, 18; page gutter 18 expanded, 12 compact |
| Radii | 4 badges and chips · 6 controls and cards · 8 windows and dialogs |
| Control heights | 30 expanded, 44 compact (touch), 24–26 inline ghost buttons; table rows 31 |
| Elevation | menus `0 8px 24px rgba(20,24,31,.16)`; dialogs `0 18px 48px rgba(20,24,31,.28)` over a `rgba(20,24,31,.42)` scrim |
| Money | currency symbol before the amount, two decimals for EUR and none for JPY; credit balances as `13,196.40 Cr`; the print view in the client's locale (`8.365,11 €`) |
| Dates | `09-Nov-2026` on screen, `31.10.2026` in a German document |

**Shell.** A 220 px dark sidebar holds the product mark, the company and site switcher (it scopes every list and
every new record), and the six modules; only the open module lists its screens, a closed one shows a count of work
waiting in it. A 50 px top bar holds the global search (Ctrl K), the approvals inbox with its count, help, and the
signed-in person with their role. Each screen owns a page head: an eyebrow naming module and company, a title, status
badges, and its actions at the trailing edge; a pinned footer carries the primary action. Compact, the sidebar is a
drawer behind the menu button, search and approvals stay in the bar, and a detail screen shows Back instead of the
menu.

**Refusals** are banners in words: what was refused, why, what was not changed, and what to do next. They never show
an exception text, an enum name or a raw id.

## Screens

| File | Screen |
|---|---|
| `Shell` | The frame: switcher open, user menu with roles and locale, compact bar, navigation drawer |
| `11-Home` | Greeting, a period-close warning, one KPI tile per module with its open work, a monthly chart, the inbox summary; tiles load and fail independently |
| `12-Approvals` | Invoices, requisitions and samples waiting for the signed-in person, in one list; the selected invoice is over her limit and must be forwarded; empty state |
| `13-Search` | Results while typing, grouped by module, with keyboard hints; compact full-screen search; no results with a way forward |
| `14-Samples` | Gallery 05 in the shell: filters, bulk bar, 1,284 rows paged |
| `15-SampleDetail` | Lifecycle steps, analyses, reagents reserved for the sample with a lost reservation race, the billing it will cause, chain of custody |
| `16-ResultEntry` | Gallery 08 per sample: a queue beside the grid, a below-LOD qualifier, the reason-for-change dialog |
| `17-Verification` | Gallery 09 per sample: queue with a separation-of-duties item, flagged results, the signature dialog with a wrong password |
| `18-Audit` | Gallery 04: facets beside a read-only table, chips, export |
| `19-PriceLists` | Versions with effective dates, next year's draft edited in place, a yen price list |
| `20-Invoices` | Drafts without numbers, a leased draft, posted invoices; loading, empty (new company) and out-of-scope refusal |
| `21-InvoiceEditor` | Header, lines grouped by analysis and expandable to samples, totals, approval route; leased by another clerk; posting refused into a closed period; loading; draft deleted meanwhile |
| `22-InvoicePrint` | An A4 German invoice laid out as a document, with print and save; compact scales the page |
| `23-BillingRuns` | A run in progress that resumed after a restart, the schedule, past runs, per-client outcomes with a failure |
| `24-Accounts` | Accounts grouped by type with the modules that post to them, one account's ledger with running balance |
| `25-Journal` | Entries by source, an entry's lines in CHF and EUR with its rounding line; a manual entry refused into a closed period |
| `26-TrialBalance` | One period grouped by type, balanced totals; the yen company's year to date |
| `27-PeriodClose` | Periods of the year, exchange rates, the checklist that blocks the close; typed confirmation |
| `28-Stock` | Items with lots, locations, expiry, reservations and value; one lot's movements |
| `29-Requisitions` | A requisition escalated past its deadline, seen by its requester; purchase orders in supplier currencies |
| `30-GoodsReceipt` | The receipt dialog: order lines received in another unit, new lots (one split in two), the posting it makes; refused for a missing rate and for a duplicate lot; loading; empty |
| `31-Companies` | Company settings fixed after the first posting, number series, posting accounts, sites |
| `32-Users` | Users, one user's scoped roles with value limits and duty rules; refusal to remove a company's last administrator |

## What ui/1 may not express

Named against the closest existing concept (`docs/spec/ui/`). Unverified against an implementation: these are the
parts to try first.

- **Shell.** The Navigator lists every group open; the design wants module groups that collapse to one row with a
  count of the module's work, and a company and site switcher above them whose choice scopes every query. The global
  search is one field over all modules with grouped, typed results in a dropdown; `headTool` search is per screen and
  has no results surface. The approvals entry is a button with a count in the top bar, not a navigator entry. The
  principal in the head (`principalPlacement: head`) is text, not a menu holding roles and locale. A page head in the
  page with a separate global top bar is the reverse of the ShellFrame, which draws the screen's title in its top bar.
- **Home.** KPI tiles (label, large value, tone, sub-line) have no node; each tile has its own loading and error state
  (`boundary` per tile may serve). The chart is a bar chart; `chart` draws line series.
- **Approvals.** One list over records of three modules, each row from a different model with its own actions, and a
  detail pane whose actions differ by record kind; disabled Approve with the limit as the reason; forwarding to a
  principal chosen by limit.
- **Search.** Results grouped by module with counts, a highlighted row moved by arrow keys while focus stays in the
  field, and Shift Enter for all results.
- **Sample list, audit, stock.** A table cell holding two lines (id and subtitle, or several badges); tinted rows by
  a condition; a selection bar showing a count with "select all 1,284" beyond the page; filter chips that remove a
  filter; a facet column with counts.
- **Sample detail.** `steps` with a second muted line per step (time and person); a page that composes records from
  lab, inventory and billing; a refusal banner naming the principal who holds a reservation.
- **Result entry.** Editable numeric cells inside a grouped table; Enter moving to the next cell; a value-or-qualifier
  cell (`< LOD`); a dialog opened by a cell's change, which can undo it.
- **Verification.** The signature dialog's password field with an attempt count, and a queue item disabled with its
  reason as text rather than a tooltip.
- **Price lists.** A table whose column is an input per row, saved as one draft, with a computed change column.
- **Invoice list.** A row badge naming the lease holder; status counts in the segmented filter; totals under a paged
  list; a refusal that offers to change the scope.
- **Invoice editor.** A multi-model screen: header form, lines grouped by analysis and expandable to their samples,
  totals and approval route from billing, the lease from the server; the whole page turning read-only while another
  principal holds it; a field error and a banner from one refused action.
- **Print view.** A fixed-size A4 page laid out as a document (letterhead, address window, footer pinned to the page
  bottom), in a locale other than the user's, scaled to the width when compact. Nothing in ui/1 lays out a page.
- **Billing runs.** Live progress of a server job with a resume note; a nested per-client table under the selected
  run.
- **Ledger.** Tables with two currency column pairs and a totals row; credit balances with a suffix; tree-grouped
  accounts; a dialog whose table rows are editable account and amount inputs, kept balanced.
- **Period close.** A grid of twelve period tiles with state; a checklist whose items carry their own actions; a
  typed-confirmation dialog.
- **Goods receipt.** A multi-model dialog: order lines from procurement, new lots and locations from stock, the
  posting preview from the ledger, all committed by one button; a quantity with a unit picker and its converted
  value; a line split into two lots (a row that spans rows).
- **Requisitions.** An approval route rendered as a timeline with a passed deadline and an escalation.
- **Org.** Inputs fixed after a domain event ("fixed after the first posting"); an inline add-assignment row whose
  limit's unit follows the chosen scope's currency; an alert dialog refusing a removal.
- **Compact.** Collapsible sections whose header carries a summary, a pinned footer with a total, and a document
  scaled to the width; cards that replace table rows with a different arrangement of the same fields.
