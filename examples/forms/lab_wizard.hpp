// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The demo's wizard and app-shell descriptors, built entirely from the
/// actions already declared in lab_model.hpp. `IntakeWizard` sequences
/// `RegisterSample` (returns a new sample's id) into `RecordMeasurement`
/// (prefilled with that id), demonstrating the shared flow draft. `LabApp`
/// is the app-shell descriptor the QML reference renderer's AppShell.qml
/// loads as its navigation root.

#include <morph/forms/app.hpp>
#include <morph/forms/flows.hpp>
#include <tuple>

#include "lab_model.hpp"

namespace lab {

/// @brief Register a sample, then record its first measurement — the
///        `RegisterSample` result's `id` prefills `RecordMeasurement.sampleId`.
using IntakeWizard =
    morph::flows::Wizard<"Register & measure a sample", morph::flows::WizardStep<RegisterSample, "New sample">,
                         morph::flows::WizardStep<RecordMeasurement, "First measurement",
                                                  morph::flows::Bind<"sampleId", "RegisterSample.id">>>;

/// @brief The demo's app shell: a density calculator, a standalone measurement
///        form, and the intake wizard.
using LabApp =
    morph::app::App<"Lab console",
                    std::tuple<morph::app::MenuEntry<"Density", "density">,
                               morph::app::MenuEntry<"Measure", "measure">, morph::app::MenuEntry<"Intake", "intake">>,
                    std::tuple<morph::app::FormScreen<"density", ComputeDryDensity>,
                               morph::app::FormScreen<"measure", RecordMeasurement>,
                               morph::app::WizardScreen<"intake", IntakeWizard>>>;

}  // namespace lab

BRIDGE_REGISTER_WIZARD(lab::IntakeWizard, "IntakeWizard")
BRIDGE_REGISTER_APP(lab::LabApp, "LabApp")
