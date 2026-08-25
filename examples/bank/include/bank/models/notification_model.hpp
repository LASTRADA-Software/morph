// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bank/db/db_model.hpp"
#include "bank/dto/common.hpp"
#include "bank/dto/notification_dto.hpp"

/// @file
/// The Notification model: post, list, and mark-read alerts for the owner.

namespace bank {

/// @brief Stores and serves per-owner notifications.
class NotificationModel : private db::WithMapper {
public:
    dto::NotificationInfo execute(const dto::Notify& action);
    dto::NotificationList execute(const dto::ListNotifications& action);
    dto::CommandResult execute(const dto::MarkRead& action);
    dto::CommandResult execute(const dto::MarkAllRead& action);
};

}  // namespace bank

using bank::NotificationModel;
using bank::dto::ListNotifications;
using bank::dto::MarkAllRead;
using bank::dto::MarkRead;
using bank::dto::Notify;

BRIDGE_REGISTER_MODEL(NotificationModel, "NotificationModel")
BRIDGE_REGISTER_ACTION(NotificationModel, Notify, "Notify")
BRIDGE_REGISTER_ACTION(NotificationModel, ListNotifications, "ListNotifications", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(NotificationModel, MarkRead, "MarkRead")
BRIDGE_REGISTER_ACTION(NotificationModel, MarkAllRead, "MarkAllRead")
