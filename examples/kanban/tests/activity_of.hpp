// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/async/DetachedTask.hpp>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "kanban/models/board_model.hpp"
#include "testkit/storage_owner.hpp"

/// @file
/// Runs `BoardModel::execute(GetActivity)` — a Task handler — from a test
/// thread that calls the model directly. The handler awaits the board's log
/// on the log's owner; for a log built on the test's thread that owner is
/// `testkit::storageOwner()`, which this pumps until the handler finishes.

namespace kanban::testing {

namespace detail {

inline ::core::async::DetachedTask runActivity(BoardModel* model, std::optional<GetActivityResult>* result,
                                               std::exception_ptr* error, bool* done) {
    try {
        *result = co_await model->execute(GetActivity{});
    } catch (...) {
        *error = std::current_exception();
    }
    *done = true;
}

}  // namespace detail

/// @brief `model.execute(GetActivity{})`, driven to its end on this thread.
/// @param model The board to ask.
/// @return The activity stream.
/// @throws Whatever the handler throws.
inline GetActivityResult activityOf(BoardModel& model) {
    std::optional<GetActivityResult> result;
    std::exception_ptr error;
    bool done = false;
    detail::runActivity(&model, &result, &error, &done);
    while (!done && ::morph::ladder::testkit::storageOwner().runOnce()) {
    }
    if (error) {
        std::rethrow_exception(error);
    }
    if (!result) {
        throw std::logic_error{"activityOf: GetActivity did not finish"};
    }
    return std::move(*result);
}

}  // namespace kanban::testing
