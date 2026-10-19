// SPDX-License-Identifier: Apache-2.0
//
// A record type whose registration sits in a translation unit linked after the
// action that returns it (push_records_late.cpp, after test_push_effects.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace push_records_late {

struct LateRow {
    std::int64_t id{};
    std::string label;
};

struct LateList {
    std::vector<LateRow> rows;
};

}  // namespace push_records_late
