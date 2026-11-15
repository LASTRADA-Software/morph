// SPDX-License-Identifier: Apache-2.0
#include "lims/app/declared_requirements.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_edit_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/reservation_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/authorization/lab_authorization.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/ignition_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/ledger/models/ledger_report_model.hpp"
#include "lims/org/authorization/org_authorization.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims::app {

std::span<std::string_view const> openMutatingActions() {
    static constexpr std::array<std::string_view, 19> open{
        "DefineAnalysis",
        "ReviseAnalysis",
        "RegisterClient",
        "NewSample",
        "LoadSample",
        "HeaderSection",
        "SamplingSection",
        "TurbiditySection",
        "NitrateSection",
        "AddIgnitionTest",
        "RecordIgnitionValues",
        "DeleteIgnitionTest",
        "ClearOptionalMeasurement",
        "SelectProject",
        "RemoveInvoiceLine",
        "SetInvoiceReference",
        "ReleaseInvoice",
        "ReleaseReservation",
        "SetDisplayName",
    };
    return open;
}

std::span<RoleRequirement const> declaredRequirements() {
    static constexpr std::array<RoleRequirement, 54> requirements{
        requirementOf<VerifyResult>(),
        requirementOf<CreateCompany>(),
        requirementOf<CreateSite>(),
        requirementOf<AssignRole>(),
        requirementOf<RevokeRole>(),
        requirementOf<RegisterSample>(),
        requirementOf<ReceiveSample>(),
        requirementOf<StartWork>(),
        requirementOf<SubmitForVerification>(),
        requirementOf<ReturnForRework>(),
        requirementOf<PublishSample>(),
        requirementOf<RejectSample>(),
        requirementOf<CaptureConcentration>(),
        requirementOf<QueuedCapture>(),
        requirementOf<ResolveConflict>(),
        requirementOf<SaveSample>(),
        requirementOf<DeleteSample>(),
        requirementOf<SetValueLimit>(),
        requirementOf<CreateAccount>(),
        requirementOf<OpenPeriod>(),
        requirementOf<BeginClosePeriod>(),
        requirementOf<CreateDraftEntry>(),
        requirementOf<PostEntry>(),
        requirementOf<ReverseEntry>(),
        requirementOf<ClosePeriod>(),
        requirementOf<SetExchangeRate>(),
        requirementOf<RecordPosting>(),
        requirementOf<SetBillingSettings>(),
        requirementOf<RegisterBillingClient>(),
        requirementOf<CreatePriceList>(),
        requirementOf<SetPrice>(),
        requirementOf<RecordBillableLine>(),
        requirementOf<CreateInvoice>(),
        requirementOf<LeaseInvoice>(),
        requirementOf<SubmitInvoice>(),
        requirementOf<ApproveInvoice>(),
        requirementOf<PostInvoice>(),
        requirementOf<StartBillingRun>(),
        requirementOf<AdvanceBillingRun>(),
        requirementOf<RunBillingSchedule>(),
        requirementOf<SetInventorySettings>(),
        requirementOf<CreateItem>(),
        requirementOf<CreateLocation>(),
        requirementOf<RecordOpeningLot>(),
        requirementOf<CreateRequisition>(),
        requirementOf<AddRequisitionLine>(),
        requirementOf<SubmitRequisition>(),
        requirementOf<ApproveRequisition>(),
        requirementOf<CreateSupplier>(),
        requirementOf<CreatePurchaseOrder>(),
        requirementOf<ReceiveGoods>(),
        requirementOf<SetRecipe>(),
        requirementOf<RecordConsumption>(),
        requirementOf<ReserveForSample>(),
    };
    return requirements;
}

std::optional<RoleRequirement> requirementFor(std::string_view actionType) {
    for (RoleRequirement const& requirement : declaredRequirements()) {
        if (requirement.actionType == actionType) {
            return requirement;
        }
    }
    return std::nullopt;
}

std::optional<LimsRole> requiredRole(std::string_view actionType) {
    auto const requirement = requirementFor(actionType);
    return requirement.has_value() ? requirement->role : std::nullopt;
}

}  // namespace lims::app
