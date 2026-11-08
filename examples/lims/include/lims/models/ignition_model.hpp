// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>

#include "lims/dto/ignition_dto.hpp"

namespace lims {

/// @brief Ignition tests, each stored as soon as it is added or edited.
///
/// Unlike `SampleModel`'s entry screen there is no working copy and no separate save, so a change made through a chat
/// applies at once. It keeps no journal of its own: entries come from the log its registration attaches
/// (`attachIgnitionLog`), so each action writes exactly one.
class IgnitionModel {
public:
    /// @brief Every ignition test.
    /// @param action The request.
    /// @return The tests, oldest first.
    [[nodiscard]] ListIgnitionTestsResult execute(const ListIgnitionTests& action);

    /// @brief Starts a test with no masses yet.
    /// @param action The request.
    /// @return The new test.
    [[nodiscard]] IgnitionTestView execute(const AddIgnitionTest& action);

    /// @brief Records the masses given, and keeps the others.
    /// @param action The test and its masses.
    /// @return The test as it is now.
    /// @throws NotFound if there is no such test.
    [[nodiscard]] IgnitionTestView execute(const RecordIgnitionValues& action);

    /// @brief Deletes a test.
    /// @param action The test.
    /// @return Which one went.
    /// @throws NotFound if there is no such test.
    [[nodiscard]] DeleteIgnitionTestResult execute(const DeleteIgnitionTest& action);
};

/// @brief Makes every ignition instance created from now on write to @p log, whichever connection registers it.
///
/// A registration and not `journal::setActionLog`: that one reaches every model, and `SampleModel` and
/// `AnalysisCatalogModel` already journal for themselves, so they would write each entry twice.
/// @param log Where the entries go.
void attachIgnitionLog(std::shared_ptr<::morph::journal::IActionLog> log);

}  // namespace lims

BRIDGE_REGISTER_MODEL(lims::IgnitionModel, "IgnitionModel")
BRIDGE_REGISTER_ACTION(lims::IgnitionModel, lims::ListIgnitionTests, "ListIgnitionTests", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::IgnitionModel, lims::AddIgnitionTest, "AddIgnitionTest")
BRIDGE_REGISTER_EFFECTS(lims::AddIgnitionTest, ::morph::effects::changes<lims::IgnitionTestView>)
BRIDGE_REGISTER_ACTION(lims::IgnitionModel, lims::RecordIgnitionValues, "RecordIgnitionValues")
BRIDGE_REGISTER_EFFECTS(lims::RecordIgnitionValues, ::morph::effects::updates<lims::IgnitionTestView>)
BRIDGE_REGISTER_ACTION(lims::IgnitionModel, lims::DeleteIgnitionTest, "DeleteIgnitionTest")
BRIDGE_REGISTER_EFFECTS(lims::DeleteIgnitionTest, ::morph::effects::changes<lims::IgnitionTestView>)
