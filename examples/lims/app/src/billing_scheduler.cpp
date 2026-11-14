// SPDX-License-Identifier: Apache-2.0
#include "lims/app/billing_scheduler.hpp"

#include "lims/billing/models/billing_run_model.hpp"

namespace lims::app {

BillingScheduleResult BillingScheduler::tick() {
    return _client.call<BillingRunModel>(RunBillingSchedule{.batch = _batch});
}

}  // namespace lims::app
