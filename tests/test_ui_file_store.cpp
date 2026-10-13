// SPDX-License-Identifier: Apache-2.0

// The file side channel's store (spec 6 §18): chunked uploads, capability references bound to a principal, and
// the bounds of MemoryFileStore.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/ui/file_store.hpp>
#include <set>
#include <string>

using morph::ui::FileError;
using morph::ui::FileInfo;
using morph::ui::MemoryFileStore;
using morph::ui::MemoryFileStoreOptions;

namespace {

FileInfo info(std::size_t size) { return FileInfo{.name = "report.pdf", .type = "application/pdf", .size = size}; }

}  // namespace

// Mutation: accept a chunk at any offset (the repeated chunk is appended twice).
TEST_CASE("MemoryFileStore takes an upload in order, and files it only when complete", "[ui][files]") {
    MemoryFileStore store;
    auto const upload = store.begin("ann", info(6));
    REQUIRE(upload.has_value());
    CHECK(store.append("ann", *upload, 0, "abc") == std::size_t{3});
    CHECK(store.append("ann", *upload, 0, "abc").error() == FileError::BadOffset);  // a repeat is refused
    CHECK(store.finish("ann", *upload).error() == FileError::Incomplete);
    CHECK(store.append("ann", *upload, 3, "defg").error() == FileError::TooLarge);  // past the announced size
    CHECK(store.append("ann", *upload, 3, "def") == std::size_t{6});
    auto const ref = store.finish("ann", *upload);
    REQUIRE(ref.has_value());
    CHECK(ref->starts_with("file:"));
    CHECK(ref->size() == 5 + 32);

    auto const whole = store.read("ann", *ref, 0, 100);
    REQUIRE(whole.has_value());
    CHECK(whole->bytes == "abcdef");
    CHECK(whole->info.size == 6);
    CHECK(whole->info.name == "report.pdf");
    CHECK(store.read("ann", *ref, 2, 3)->bytes == "cde");
    CHECK(store.read("ann", *ref, 9, 3)->bytes.empty());
    CHECK(store.finish("ann", *upload).error() == FileError::NotFound);  // an upload files once
}

// Mutation: skip the principal check in read (bob reads ann's file).
TEST_CASE("An upload id and a reference work only for the principal they were made for", "[ui][files]") {
    MemoryFileStore store;
    auto const upload = store.begin("ann", info(1));
    REQUIRE(upload.has_value());
    CHECK(store.append("bob", *upload, 0, "x").error() == FileError::NotFound);
    CHECK(store.finish("bob", *upload).error() == FileError::NotFound);
    REQUIRE(store.append("ann", *upload, 0, "x").has_value());
    auto const ref = store.finish("ann", *upload);
    REQUIRE(ref.has_value());
    CHECK(store.read("bob", *ref, 0, 1).error() == FileError::NotFound);  // the same answer as a missing file
    CHECK(store.read("ann", "file:00000000000000000000000000000000", 0, 1).error() == FileError::NotFound);

    auto const issued = store.issue("bob", FileInfo{.name = "out.csv", .type = "text/csv", .size = 0}, "a,b\n");
    REQUIRE(issued.has_value());
    CHECK(store.read("bob", *issued, 0, 100)->bytes == "a,b\n");
    CHECK(store.read("bob", *issued, 0, 100)->info.size == 4);
    CHECK(store.read("ann", *issued, 0, 100).error() == FileError::NotFound);
}

TEST_CASE("References are distinct and unguessable in form", "[ui][files]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        std::string const ref = morph::ui::newFileRef();
        CHECK(ref.find_first_not_of("0123456789abcdef", 5) == std::string::npos);
        seen.insert(ref);
    }
    CHECK(seen.size() == 1000);
}

// Mutation: leave in-progress uploads out of the quota (the second begin is accepted).
TEST_CASE("MemoryFileStore bounds a file and a principal, and an upload that stalls expires", "[ui][files]") {
    auto now = std::chrono::steady_clock::time_point{};
    MemoryFileStore store{MemoryFileStoreOptions{
        .maxFileBytes = 10, .maxBytesPerPrincipal = 15, .uploadTtl = std::chrono::seconds{60}, .clock = [&] {
            return now;
        }}};
    CHECK(store.begin("ann", info(11)).error() == FileError::TooLarge);
    auto const first = store.begin("ann", info(10));
    REQUIRE(first.has_value());
    CHECK(store.begin("ann", info(6)).error() == FileError::OverQuota);  // 10 announced + 6 > 15
    CHECK(store.begin("bob", info(10)).has_value());                     // another principal's quota
    CHECK(store.issue("ann", info(0), std::string(6, 'x')).error() == FileError::OverQuota);

    now += std::chrono::seconds{61};  // the first upload stalls past the TTL and frees its share
    CHECK(store.append("ann", *first, 0, "x").error() == FileError::NotFound);
    CHECK(store.begin("ann", info(10)).has_value());
}
