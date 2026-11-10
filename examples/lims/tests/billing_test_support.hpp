// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"

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
    auto const list = prices.execute(CreatePriceList{
        .company = seededCompany(),
        .name = std::string{currencyName(currency)} + " list",
        .currency = currency,
        .validFrom = ::morph::time::Timestamp{at(2026, 1, 1, 0)},
    });
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

}  // namespace lims::test
