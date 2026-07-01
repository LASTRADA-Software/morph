// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Notification model.

namespace bank::dto {

/// @brief A user-facing alert.
struct NotificationInfo {
    std::int64_t id = 0;
    std::string owner;
    int severity = 0;
    std::string message;
    bool read = false;
    std::int64_t createdAtMs = 0;
};

/// @brief Post a notification for the current owner.
struct Notify {
    std::string message;
    int severity = 0;

    [[nodiscard]] bool validate() const { return !message.empty() && severity >= 0 && severity <= 2; }
};

/// @brief List the current owner's notifications.
struct ListNotifications {
    std::string owner;        ///< empty => session principal
    bool unreadOnly = false;
};

/// @brief Result of `ListNotifications`.
struct NotificationList {
    std::vector<NotificationInfo> notifications;
    int unreadCount = 0;
};

/// @brief Mark a single notification read.
struct MarkRead {
    std::int64_t id = 0;
};

/// @brief Mark all of the current owner's notifications read.
struct MarkAllRead {
    std::string owner;  ///< empty => session principal
};

}  // namespace bank::dto
