// SPDX-License-Identifier: Apache-2.0

// Companion header for test_registration_same_line.cpp -- see that file for the scenario this
// reproduces. The BRIDGE_REGISTER_MODEL invocation below must stay on the exact same physical
// line number as the one in issue21_same_line_a.hpp for the two headers to exercise the bug.

#pragma once

#include <morph/core/registry.hpp>

namespace issue21::sameline {

struct WidgetB {
    int execute(int x) { return x + 2; }
};

}  // namespace issue21::sameline

BRIDGE_REGISTER_MODEL(issue21::sameline::WidgetB, "Issue21_SameLine_WidgetB")
