// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// The models the shell's parts read. The shell reads each through one shared instance, which a screen document may
/// name only for a model that keeps no state; these keep none, so a shared instance is equivalent to a private one.
/// Every translation unit that aliases one of them in a screen includes this header.

/// @brief The organisation keeps no state between actions.
template <>
struct morph::ui::author::ModelFacts<lims::OrgModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};

/// @brief The lab's shell reads keep no state.
template <>
struct morph::ui::author::ModelFacts<lims::SampleDirectoryModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};

/// @brief Invoices keep no state between actions.
template <>
struct morph::ui::author::ModelFacts<lims::InvoiceModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};

/// @brief The ledger keeps no state between actions.
template <>
struct morph::ui::author::ModelFacts<lims::LedgerModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};

/// @brief Procurement keeps no state between actions.
template <>
struct morph::ui::author::ModelFacts<lims::ProcurementModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};

/// @brief Stock keeps no state between actions.
template <>
struct morph::ui::author::ModelFacts<lims::StockModel> {
    /// @brief A shared instance is equivalent to a private one.
    static constexpr bool stateless = true;
};
