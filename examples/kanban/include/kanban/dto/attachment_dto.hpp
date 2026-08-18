// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "kanban/core/types.hpp"
#include "kanban/dto/project_dto.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @file
/// `AddAttachment`/`GetAttachments`/`RemoveAttachment` -- README build-order
/// step 8's task attachments, metadata half only ("bytes over a side
/// channel, metadata through actions"). The actual byte upload/download is a
/// separate HTTP side channel (a later task, not this one): a client first
/// uploads the file's bytes there and gets back a `storageKey` -- an opaque
/// string naming where those bytes now live -- and only then calls
/// `AddAttachment` with that `storageKey` to commit the metadata row. This
/// file never validates that `storageKey` actually resolves to stored bytes;
/// that is the side channel's job (and, later, the GUI wiring that surfaces
/// a broken reference).
namespace kanban {

inline constexpr std::size_t kMaxAttachmentFilenameBytes = 255;
inline constexpr std::size_t kMaxAttachmentContentTypeBytes = 127;
inline constexpr std::size_t kMaxAttachmentStorageKeyBytes = 255;

/// @brief Records that a file has been uploaded (via the separate HTTP side
///        channel) and attaches its metadata to a task.
struct AddAttachment {
    TaskId taskId;
    std::string filename;
    std::string contentType;
    std::int64_t sizeBytes = 0;
    /// @brief Opaque reference to wherever the side channel put the bytes --
    ///        not a path/URL this type interprets or validates in any way.
    std::string storageKey;

    [[nodiscard]] bool validate() const noexcept {
        return taskId.hasValue() && !filename.empty() && filename.size() <= kMaxAttachmentFilenameBytes &&
               !contentType.empty() && contentType.size() <= kMaxAttachmentContentTypeBytes && sizeBytes >= 0 &&
               !storageKey.empty() && storageKey.size() <= kMaxAttachmentStorageKeyBytes;
    }
};

/// @brief One attachment, as returned by `GetAttachments`.
struct AttachmentView {
    AttachmentId id;
    TaskId taskId;
    std::string filename;
    std::string contentType;
    std::int64_t sizeBytes = 0;
    std::string storageKey;
    std::string uploadedBy;
    std::int64_t uploadedAtMs = 0;
};

/// @brief Lists every attachment recorded against one task.
struct GetAttachments {
    TaskId taskId;

    [[nodiscard]] bool validate() const noexcept { return taskId.hasValue(); }
};

/// @brief `GetAttachments`' result: every attachment on the task, in
///        upload order.
struct GetAttachmentsResult {
    std::vector<AttachmentView> attachments;
};

/// @brief Deletes one attachment's metadata row. Does not (and, being
///        metadata-only, cannot) delete the underlying bytes the side
///        channel stored under `storageKey` -- that is out of scope for this
///        task, same as this file's `@file` comment states for `storageKey`
///        itself.
struct RemoveAttachment {
    AttachmentId attachmentId;

    [[nodiscard]] bool validate() const noexcept { return attachmentId.hasValue(); }
};

// `AddAttachment`/`RemoveAttachment` both return `kanban::Ack` (from
// `project_dto.hpp`) -- an acknowledgement carrying no data, the same result
// shape `DeleteRule` uses (design spec §7's "every mutating action returns
// the full rebuilt state" convention predates the Rule/Tag family; Task 13/14
// already established that a later-added feature may return its own,
// narrower result type instead of `GetBoardResult`, and attachments follow
// that same, more recent precedent rather than growing `GetBoardResult`
// itself).

}  // namespace kanban
