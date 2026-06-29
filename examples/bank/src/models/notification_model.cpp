// SPDX-License-Identifier: Apache-2.0

#include "bank/models/notification_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/db/ledger_ops.hpp"  // for nowMillis()
#include "bank/db/notification_entity.hpp"

namespace bank {

namespace {

dto::NotificationInfo toInfo(const db::NotificationRecord& rec) {
    return dto::NotificationInfo{
        .id = static_cast<std::int64_t>(rec.id.Value()),
        .owner = std::string{rec.owner.Value().str()},
        .severity = rec.severity.Value(),
        .message = std::string{rec.message.Value().str()},
        .read = rec.read.Value(),
        .createdAtMs = rec.createdAtMs.Value(),
    };
}

}  // namespace

dto::NotificationInfo NotificationModel::execute(const dto::Notify& action) {
    if (!action.validate()) {
        throw ValidationError{"message required and severity must be 0..2"};
    }
    const std::string owner = sessionPrincipal();
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    db::NotificationRecord rec;
    rec.owner = Light::SqlAnsiString<64>{owner};
    rec.severity = action.severity;
    rec.message = Light::SqlAnsiString<256>{action.message};
    rec.read = false;
    rec.createdAtMs = db::nowMillis();
    mapper().Create(rec);
    return toInfo(rec);
}

dto::NotificationList NotificationModel::execute(const dto::ListNotifications& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto rows = mapper()
                    .Query<db::NotificationRecord>()
                    .Where(Lightweight::FieldNameOf<&db::NotificationRecord::owner>, "=", owner)
                    .All();
    dto::NotificationList out;
    for (const auto& rec : rows) {
        if (!rec.read.Value()) {
            out.unreadCount += 1;
        }
        if (action.unreadOnly && rec.read.Value()) {
            continue;
        }
        out.notifications.push_back(toInfo(rec));
    }
    return out;
}

dto::CommandResult NotificationModel::execute(const dto::MarkRead& action) {
    auto rec = mapper().QuerySingle<db::NotificationRecord>(static_cast<std::uint64_t>(action.id));
    if (!rec.has_value()) {
        throw NotFound{"notification not found"};
    }
    if (std::string{rec->owner.Value().str()} != sessionPrincipal()) {
        throw Unauthorized{"notification belongs to a different owner"};
    }
    rec->read = true;
    mapper().Update(*rec);
    return dto::CommandResult{.ok = true, .message = "marked read"};
}

dto::CommandResult NotificationModel::execute(const dto::MarkAllRead& action) {
    const std::string owner = resolveOwner(action.owner);
    if (owner.empty()) {
        throw Unauthorized{"no session principal"};
    }
    auto rows = mapper()
                    .Query<db::NotificationRecord>()
                    .Where(Lightweight::FieldNameOf<&db::NotificationRecord::owner>, "=", owner)
                    .All();
    int updated = 0;
    for (auto& rec : rows) {
        if (!rec.read.Value()) {
            rec.read = true;
            mapper().Update(rec);
            ++updated;
        }
    }
    return dto::CommandResult{.ok = true, .message = std::to_string(updated) + " marked read"};
}

}  // namespace bank
