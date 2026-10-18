// SPDX-License-Identifier: Apache-2.0
//
// Registers a record after the action that returns it and the effects that name
// it: listed after test_push_effects.cpp in morph_tests, so its static
// initialiser runs later. Matching is lazy, so the order must not matter.

#include "push_records_late.hpp"

#include <morph/core/records.hpp>

BRIDGE_REGISTER_RECORD(push_records_late::LateRow, "T_LateRow")
