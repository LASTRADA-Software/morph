// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// The metadata-fetch worker's one injectable seam.
///
/// **Why no real HTTP client**: morph ships none, and building one is
/// squarely out of this rung's scope — the framework subsystem under stress
/// here is the *background-job dispatch pattern* (an internal client routing
/// through the full server pipeline: authorize, authenticate, dispatch,
/// journal), not network I/O. `IBookmarkMetadataFetcher` is the extension
/// point a real deployment implements; this rung ships only
/// `NullMetadataFetcher`, which performs no I/O and returns an empty
/// `FetchedMetadata`, so nothing in the test suite depends on timing or on a
/// network being reachable.

namespace bookmarks::app {

/// @brief What a metadata fetch produces. Both fields empty is a legitimate
///        "found nothing" result, not a distinguished failure — mirrors
///        `RecordMetadata`'s own "empty = leave the stored value alone" DTO
///        convention.
struct FetchedMetadata {
    /// @brief The page title, or empty if none was found.
    std::string title;
    /// @brief A path/URL to the page's favicon, or empty if none was found.
    std::string faviconPath;
};

/// @brief Pluggable page-metadata fetcher. See this file's own `@file`
///        comment for why this rung ships no real HTTP implementation.
class IBookmarkMetadataFetcher {
public:
    IBookmarkMetadataFetcher() = default;
    virtual ~IBookmarkMetadataFetcher() = default;
    IBookmarkMetadataFetcher(const IBookmarkMetadataFetcher&) = delete;
    IBookmarkMetadataFetcher& operator=(const IBookmarkMetadataFetcher&) = delete;
    IBookmarkMetadataFetcher(IBookmarkMetadataFetcher&&) = delete;
    IBookmarkMetadataFetcher& operator=(IBookmarkMetadataFetcher&&) = delete;

    /// @brief Fetches title/favicon metadata for @p url.
    ///
    /// Called synchronously from `App::fetchMetadataOnce()`, once per
    /// untitled bookmark, on whichever thread drove that pass. An
    /// implementation that really does network I/O is responsible for its
    /// own timeout — a fetcher that blocks indefinitely blocks the sweep.
    /// @param url The bookmark's url.
    /// @return The fetched metadata, or an empty one if nothing was found.
    [[nodiscard]] virtual FetchedMetadata fetch(const std::string& url) = 0;
};

/// @brief The shipped default: performs no I/O, always returns an empty
///        result. Deterministic and instant, for tests and for a deployment
///        that has not yet plugged in a real fetcher.
class NullMetadataFetcher : public IBookmarkMetadataFetcher {
public:
    /// @brief Ignores @p url and reports "nothing found".
    /// @param url Ignored.
    /// @return A default-constructed `FetchedMetadata`.
    [[nodiscard]] FetchedMetadata fetch([[maybe_unused]] const std::string& url) override { return {}; }
};

}  // namespace bookmarks::app
