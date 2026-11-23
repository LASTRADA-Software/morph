// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/row_commits.hpp
/// @brief A model table's row commits held by a test: every request `CellEdits` sends, and the completion each one
///        settles with, called when the test says.

#include <morph/table/edits.hpp>
#include <utility>
#include <vector>

namespace morph::test {

/// @brief Records each row commit and keeps its completion for the test to call.
struct RowCommits {
    /// @brief Every request sent, oldest first.
    std::vector<table::RowEditRequest> sent;
    /// @brief The completion of each request, in the same order.
    std::vector<table::RowEditDone> done;

    /// @brief The commit function a `CellEdits` is built with.
    /// @return It; it reads this object, which must outlive it.
    table::RowEditCommit commit() {
        return [this](table::RowEditRequest const& request, table::RowEditDone settle) {
            sent.push_back(request);
            done.push_back(std::move(settle));
        };
    }
};

}  // namespace morph::test
