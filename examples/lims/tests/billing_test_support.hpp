// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/ledger_wiring.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/core/ledger_port.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

/// @file
/// A sample with verified results, ready to publish or published, and a client set up to be billed: what every
/// billing suite starts from.

namespace lims::test {

/// @brief What `readyToPublish` made.
struct LabSample {
    /// @brief The lab's client.
    ClientId client;
    /// @brief The nitrate analysis definition.
    AnalysisId nitrate;
    /// @brief The sample, at the seeded site.
    SampleId sample;
    /// @brief Its results, all verified.
    std::vector<ResultId> results;
};

/// @brief A sample at the seeded site with one nitrate reading captured by `alice` and verified by `carol`, in
///        `ToBeVerified`. Grants carol the verifier role at the seeded company.
/// @param reference The sample's reference.
/// @return The ids.
[[nodiscard]] inline LabSample readyToPublish(std::string const& reference = "WW-1") {
    grant("carol", LimsRole::Verifier);
    LabSample lab;
    ResultId result;
    {
        ScopedPrincipal const alice{"alice"};
        AnalysisCatalogModel catalog;
        auto const nitrate = catalog.execute(
            DefineAnalysis{.name = "Nitrate " + reference, .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
        lab.nitrate = nitrate.analysisId;
        SampleModel bench;
        lab.client = bench.execute(RegisterClient{.name = "Waterworks Ltd"}).clientId;
        lab.sample =
            bench.execute(RegisterSample{.siteId = seededSite(), .clientId = lab.client, .reference = reference}).id;
        bench.execute(ReceiveSample{});
        bench.execute(StartWork{});
        result =
            bench
                .execute(CaptureConcentration{
                    .analysisVersionId = nitrate.versionId,
                    .value = Concentration{::morph::math::Rational{
                        ::morph::math::Numerator{12}, ::morph::math::Denominator{5}, ::morph::math::DecimalPlaces{3}}},
                })
                .id;
        bench.execute(SubmitForVerification{});
    }
    {
        ScopedPrincipal const carol{"carol"};
        SampleModel verifier;
        verifier.execute(OpenSample{.sampleId = lab.sample});
        verifier.execute(VerifyResult{.resultId = result});
    }
    lab.results.push_back(result);
    return lab;
}

/// @brief `readyToPublish`, then published by `alice`.
/// @param reference The sample's reference.
/// @return The ids.
[[nodiscard]] inline LabSample publishedSample(std::string const& reference = "WW-1") {
    auto lab = readyToPublish(reference);
    ScopedPrincipal const alice{"alice"};
    SampleModel bench;
    bench.execute(OpenSample{.sampleId = lab.sample});
    bench.execute(PublishSample{});
    return lab;
}

/// @brief Registers @p client for billing at the seeded company in @p currency, with a price list in force since
///        1 January 2026 pricing every analysis id from 1 to 20 at 40 units (4000 yen in JPY). Done by `bea`, a
///        billing clerk there.
/// @param client The lab's client.
/// @param currency The client's currency.
inline void billingFor(ClientId client, CurrencyCode currency = CurrencyCode::EUR) {
    grant("bea", LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    PriceListModel prices;
    prices.execute(RegisterBillingClient{
        .client = client,
        .company = seededCompany(),
        .name = "Waterworks Ltd",
        .currency = currency,
    });
    // A second client of the same currency shares the list the first one made.
    PriceListView list;
    try {
        list = prices.execute(CreatePriceList{
            .company = seededCompany(),
            .name = std::string{currencyName(currency)} + " list",
            .currency = currency,
            .validFrom = ::morph::time::Timestamp{at(2026, 1, 1, 0)},
        });
    } catch (Conflict const&) {
        return;
    }
    std::int64_t const price = currency == CurrencyCode::JPY ? 4000 : 40;
    for (std::int64_t analysis = 1; analysis <= 20; ++analysis) {
        prices.execute(SetPrice{
            .priceList = list.id,
            .analysis = AnalysisId{analysis},
            .client = {},
            .amount = Amount{::morph::math::Rational{price, ::morph::math::DecimalPlaces{decimalsOf(currency)}}},
        });
    }
}

/// @brief The secret the test servers' authorizer and the service tokens share.
inline constexpr std::string_view kServiceSecret = "lims-service-test-secret-at-least-32-bytes";

/// @brief Gives @p principal the service role in @p scope, unless it holds it there already.
/// @param principal A service principal.
/// @param scope The scope; the seeded company by default.
inline void grantService(std::string const& principal, Scope scope = Scope{seededCompany()}) {
    auto const held = OrgModel::rolesIn(principal, scope);
    if (std::ranges::find(held, LimsRole::Service) == held.end()) {
        grant(principal, LimsRole::Service, scope);
    }
}

/// @brief Gives @p principal @p role at the seeded company, unless it holds it there already.
/// @param principal The principal.
/// @param role The role.
inline void grantIfAbsent(std::string const& principal, LimsRole role) {
    auto const held = OrgModel::rolesIn(principal, Scope{seededCompany()});
    if (std::ranges::find(held, role) == held.end()) {
        static_cast<void>(grant(principal, role));
    }
}

/// @brief One relay pass over lab's outbox into billing, as the server runs it: through an internal client of a
///        server of its own, as the events principal, which this gives the service role at the seeded company.
/// @return How many events were relayed.
/// @throws std::exception from billing's consumer, as `lims::app::relayLabEvents` does.
inline std::size_t relayLabEvents() {
    grantService(std::string{app::kEventsPrincipal});
    ::morph::ladder::testkit::BackendRig rig{::morph::ladder::testkit::Mode::Socket, 0,
                                             limsAuthorizer(kServiceSecret)};
    app::InternalClient events{rig.server(), kServiceSecret, std::string{app::kEventsPrincipal}};
    return app::relayLabEvents(events);
}

/// @brief Runs @p action on a fresh @p Model as @p principal through an in-process bridge, as a client's call runs.
///
/// A handler that is a coroutine (a Task handler) is only built by a direct `execute`; a dispatch drives it, so a
/// suite sends such an action this way.
/// @tparam Model The model.
/// @tparam Action The action.
/// @param principal Who sends it.
/// @param action The action.
/// @return Its result.
/// @throws std::exception the action's failure.
template <typename Model, typename Action>
auto dispatchAs(std::string const& principal, Action action) {
    ::morph::ladder::testkit::BackendRig rig{::morph::ladder::testkit::Mode::Local, 1};
    ::morph::session::TokenIssuer const issuer{std::string{kServiceSecret}, ::morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(signedAs(issuer, principal));
    auto handler = rig.client<Model>(0);
    return ::morph::ladder::testkit::awaitQt(handler.execute(std::move(action)));
}

/// @brief Billing settings for the seeded company (19 % tax, 48 approval hours, billing day 25), value limits as
///        invoice approver: paula 100 and tom 10 at the seeded site, carl 1000 and bea 1000 at the company. Also
///        one published, relayed sample of a client billed in EUR. Set at 09:00 on 10 November 2026.
/// @return The sample.
inline LabSample approvalSetUp() {
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    grant("sam", LimsRole::Supervisor);
    {
        ScopedPrincipal const sam{"sam"};
        PriceListModel{}.execute(SetBillingSettings{
            .company = seededCompany(),
            .receivableAccount = "1200",
            .revenueAccount = "4000",
            .taxAccount = "2200",
            .roundingAccount = "6900",
            .taxRate = Rate{::morph::math::Rational{::morph::math::Numerator{19}, ::morph::math::Denominator{100},
                                                    ::morph::math::DecimalPlaces{6}}},
            .approvalHours = Count{::morph::math::Rational{48, ::morph::math::DecimalPlaces{0}}},
            .billingDay = Count{::morph::math::Rational{25, ::morph::math::DecimalPlaces{0}}}});
        OrgModel org;
        auto const limit = [&org](std::string principal, std::int64_t units) {
            org.execute(
                SetValueLimit{.principal = std::move(principal),
                              .company = seededCompany(),
                              .limit = Amount{::morph::math::Rational{units, ::morph::math::DecimalPlaces{2}}}});
        };
        limit("paula", 100);
        limit("carl", 1000);
        limit("tom", 10);
        limit("bea", 1000);
    }
    grant("paula", LimsRole::InvoiceApprover, Scope{seededSite()});
    grant("tom", LimsRole::InvoiceApprover, Scope{seededSite()});
    grant("carl", LimsRole::InvoiceApprover);
    grant("bea", LimsRole::InvoiceApprover);
    auto lab = publishedSample();
    billingFor(lab.client);
    relayLabEvents();
    return lab;
}

/// @brief `approvalSetUp`, and the client's invoice drafted by bea at the seeded site (gross 47.60).
/// @return The invoice.
inline InvoiceView draftedInvoice() {
    auto const lab = approvalSetUp();
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    ScopedPrincipal const bea{"bea"};
    return InvoiceModel{}.execute(
        CreateInvoice{.company = seededCompany(), .site = seededSite(), .client = lab.client});
}

/// @brief Submits @p invoice as bea at 09:00 on 10 November, dispatched as a client sends it.
/// @param invoice The invoice.
inline void submit(InvoiceView const& invoice) {
    ::morph::ladder::ScopedClockOverride const clock{at(2026, 11, 10)};
    static_cast<void>(dispatchAs<InvoiceModel>("bea", SubmitInvoice{.invoiceId = invoice.id}));
}

/// @brief The seeded company's books: accounts 1200, 2200, 4000, 6900 and November 2026 open, made by `acc`, an
///        accountant there.
inline void books() {
    grant("acc", LimsRole::Accountant);
    ScopedPrincipal const acc{"acc"};
    LedgerModel ledger;
    for (auto const& [code, type] : std::vector<std::pair<std::string, AccountType>>{
             {"1200", AccountType::Asset},
             {"2200", AccountType::Liability},
             {"4000", AccountType::Revenue},
             {"6900", AccountType::Expense},
         }) {
        ledger.execute(
            CreateAccount{.company = seededCompany(), .code = code, .name = "Account " + code, .type = type});
    }
    ledger.execute(OpenPeriod{.company = seededCompany(), .month = ::morph::time::Timestamp{at(2026, 11, 1)}});
}

/// @brief The seeded company's billing settings, 19 % tax, 48 approval hours, billing day 25, set by `sam`.
inline void billingSettings() {
    grant("sam", LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    PriceListModel{}.execute(SetBillingSettings{
        .company = seededCompany(),
        .receivableAccount = "1200",
        .revenueAccount = "4000",
        .taxAccount = "2200",
        .roundingAccount = "6900",
        .taxRate = Rate{::morph::math::Rational{::morph::math::Numerator{19}, ::morph::math::Denominator{100},
                                                ::morph::math::DecimalPlaces{6}}},
        .approvalHours = Count{::morph::math::Rational{48, ::morph::math::DecimalPlaces{0}}},
        .billingDay = Count{::morph::math::Rational{25, ::morph::math::DecimalPlaces{0}}},
    });
}

/// @brief Whether the posting principal holds the service role a `PostingRig` installs it with.
enum class PostingRole : std::uint8_t {
    Granted,   ///< `system:posting` holds `LimsRole::Service` at the seeded company.
    Withheld,  ///< It holds no role: the ledger's rule has nothing to allow.
};

/// @brief A server, the posting principal's client on it and the ledger port installed over them, as the server
///        process wires them; two socket clients for the clerks. The port is uninstalled when it goes.
class PostingRig {
public:
    /// @brief Installs the port in @p mode.
    /// @param mode Which way `PostInvoice` posts.
    /// @param role Whether the posting principal holds the service role.
    explicit PostingRig(PostingMode mode, PostingRole role = PostingRole::Granted) {
        if (role == PostingRole::Granted) {
            grantService(std::string{app::kPostingPrincipal});
        }
        app::installLedgerPort(_client, mode);
    }
    ~PostingRig() { setLedgerPort({}); }
    PostingRig(PostingRig const&) = delete;
    PostingRig& operator=(PostingRig const&) = delete;
    PostingRig(PostingRig&&) = delete;
    PostingRig& operator=(PostingRig&&) = delete;

    /// @brief The server and its clients.
    /// @return The rig.
    [[nodiscard]] ::morph::ladder::testkit::BackendRig& rig() noexcept { return _rig; }

    /// @brief Posts @p invoice as @p principal over socket client @p index, as a clerk's screen sends it.
    /// @param invoice The invoice.
    /// @param principal Who posts it.
    /// @param index The socket client.
    /// @return The posted invoice.
    /// @throws std::exception the posting's refusal or failure.
    InvoiceView post(InvoiceView const& invoice, std::string const& principal = "bea", std::size_t index = 0) {
        ::morph::session::TokenIssuer const issuer{std::string{kServiceSecret}, ::morph::session::hmacSha256};
        _rig.bridge(index).setDefaultSession(signedAs(issuer, principal));
        auto clerk = _rig.client<InvoiceModel>(index);
        return ::morph::ladder::testkit::awaitQt(clerk.execute(PostInvoice{.invoiceId = invoice.id}));
    }

private:
    ::morph::ladder::testkit::BackendRig _rig{::morph::ladder::testkit::Mode::Socket, 2,
                                              limsAuthorizer(kServiceSecret)};
    app::InternalClient _client{_rig.server(), kServiceSecret, std::string{app::kPostingPrincipal}};
};

/// @brief An approved invoice for one published sample in @p currency: drafted and submitted by `bea`, approved by
///        `carl`, an approver at the company with a limit of 1000. Needs `billingSettings()` first, and a
///        `PostingRig` when @p currency is not the company's.
/// @param reference The sample's reference, distinct per invoice.
/// @param currency The client's currency.
/// @return The invoice.
inline InvoiceView approvedInvoice(std::string const& reference = "WW-1", CurrencyCode currency = CurrencyCode::EUR) {
    {
        ScopedPrincipal const sam{"sam"};
        OrgModel{}.execute(SetValueLimit{
            .principal = "carl",
            .company = seededCompany(),
            .limit = Amount{::morph::math::Rational{1000, ::morph::math::DecimalPlaces{2}}},
        });
    }
    grantIfAbsent("carl", LimsRole::InvoiceApprover);
    auto const lab = publishedSample(reference);
    billingFor(lab.client, currency);
    relayLabEvents();
    InvoiceId invoiceId;
    {
        ScopedPrincipal const bea{"bea"};
        invoiceId = InvoiceModel{}
                        .execute(CreateInvoice{.company = seededCompany(), .site = seededSite(), .client = lab.client})
                        .id;
    }
    static_cast<void>(dispatchAs<InvoiceModel>("bea", SubmitInvoice{.invoiceId = invoiceId}));
    ScopedPrincipal const carl{"carl"};
    return InvoiceModel{}.execute(ApproveInvoice{.invoiceId = invoiceId});
}

}  // namespace lims::test
