// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/i18n.hpp>
#include <optional>
#include <string>

using morph::forms::i18n::FieldSlot;

TEST_CASE("forms::i18n::fieldKey matches the spec table for every slot", "[forms][i18n]") {
    CHECK(morph::forms::i18n::fieldKey("RecordMeasurement", "sampleId", FieldSlot::Label) ==
          "RecordMeasurement.sampleId.label");
    CHECK(morph::forms::i18n::fieldKey("RecordMeasurement", "sampleId", FieldSlot::Help) ==
          "RecordMeasurement.sampleId.help");
    CHECK(morph::forms::i18n::fieldKey("RecordMeasurement", "sampleId", FieldSlot::Placeholder) ==
          "RecordMeasurement.sampleId.placeholder");
}

TEST_CASE("forms::i18n::explicitFieldKey overrides the stem but keeps the slot suffix", "[forms][i18n]") {
    CHECK(morph::forms::i18n::explicitFieldKey("", FieldSlot::Label) == std::nullopt);
    CHECK(morph::forms::i18n::explicitFieldKey("catalog.sample", FieldSlot::Label) == "catalog.sample.label");
    CHECK(morph::forms::i18n::explicitFieldKey("catalog.sample", FieldSlot::Help) == "catalog.sample.help");
    CHECK(morph::forms::i18n::explicitFieldKey("catalog.sample", FieldSlot::Placeholder) ==
          "catalog.sample.placeholder");
}

TEST_CASE("forms::i18n::groupKey and ruleKey are indexed off the action type id", "[forms][i18n]") {
    CHECK(morph::forms::i18n::groupKey("RecordMeasurement", 0) == "RecordMeasurement.group.0");
    CHECK(morph::forms::i18n::groupKey("RecordMeasurement", 2) == "RecordMeasurement.group.2");
    CHECK(morph::forms::i18n::ruleKey("RecordMeasurement", 1) == "RecordMeasurement.rule.1");
}

TEST_CASE("forms::i18n wizard and app-shell keys", "[forms][i18n]") {
    CHECK(morph::forms::i18n::wizardTitleKey("OnboardWizard") == "OnboardWizard.title");
    CHECK(morph::forms::i18n::wizardStepTitleKey("OnboardWizard", 0) == "OnboardWizard.step.0.title");
    CHECK(morph::forms::i18n::wizardStepTitleKey("OnboardWizard", 3) == "OnboardWizard.step.3.title");
    CHECK(morph::forms::i18n::appTitleKey("LabApp") == "LabApp.title");
    CHECK(morph::forms::i18n::appMenuLabelKey("LabApp", 2) == "LabApp.menu.2.label");
}
