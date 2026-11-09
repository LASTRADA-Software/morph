// SPDX-License-Identifier: Apache-2.0
#include "lims/lab/authorization/lab_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <vector>

#include "lims/lab/db/lims_entity.hpp"

namespace lims {

namespace {

/// @brief The result row @p result names.
/// @param mapper The open mapper.
/// @param result The result.
/// @return Its row, at most one.
[[nodiscard]] std::vector<db::ResultRecord> resultRows(Lightweight::DataMapper& mapper, ResultId result) {
    if (!result.hasValue()) {
        return {};
    }
    return mapper.Query<db::ResultRecord>()
        .Where(::Lightweight::FieldNameOf<&db::ResultRecord::id>, "=", *result)
        .All();
}

}  // namespace

Scope Authorization<VerifyResult>::scopeOf(VerifyResult const& action) {
    Lightweight::DataMapper mapper;
    auto const results = resultRows(mapper, action.resultId);
    if (results.empty()) {
        return Scope{SiteId{}};
    }
    auto const samples = mapper.Query<db::SampleRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::SampleRecord::id>, "=",
                                    static_cast<std::int64_t>(results.front().sample.Value()))
                             .All();
    return samples.empty() ? Scope{SiteId{}} : Scope{SiteId::fromOptional(samples.front().siteId.Value())};
}

std::optional<std::string> Authorization<VerifyResult>::firstPerformerOf(VerifyResult const& action) {
    Lightweight::DataMapper mapper;
    auto const results = resultRows(mapper, action.resultId);
    if (results.empty()) {
        return std::nullopt;
    }
    return std::string{results.front().capturedBy.Value().ToStringView()};
}

}  // namespace lims
