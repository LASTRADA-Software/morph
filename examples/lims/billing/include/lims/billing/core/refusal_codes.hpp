// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <string_view>

namespace lims {

/// @brief The code of the refusal of a draft invoice another person is editing; its argument `holder` is that
///        person's display name.
inline constexpr std::string_view kInvoiceLeasedCode = "lims.billing.invoiceLeased";

/// @brief Every specific code billing throws; the kit's test holds both catalogues to this list.
inline constexpr std::array<std::string_view, 1> kBillingRefusalCodes{kInvoiceLeasedCode};

}  // namespace lims
