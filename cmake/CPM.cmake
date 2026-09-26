# SPDX-License-Identifier: Apache-2.0
#
# CPM.cmake bootstrap: downloads the pinned CPM once, verifies it and includes it.
# https://github.com/cpm-cmake/CPM.cmake
#
# Byte-for-byte core-cpp's cmake/CPM.cmake below this comment, so the two
# projects load the same CPM and a diff between the copies shows only this
# header. The pin is 0.40.8 with its SHA-256; the download is bounded when the
# configure defines FASTCACHED_FETCH_SILENCE_SECONDS and unbounded otherwise,
# because an empty INACTIVITY_TIMEOUT would break the argument list.
#
# With CPM_SOURCE_CACHE set (CMakeLists.txt defaults it to .cache/cpm) the
# bootstrap itself lives in the cache, so a warm cache downloads nothing.
set(_coreCppCpmBound "")
if(DEFINED FASTCACHED_FETCH_SILENCE_SECONDS)
    set(_coreCppCpmBound INACTIVITY_TIMEOUT "${FASTCACHED_FETCH_SILENCE_SECONDS}")
endif()

set(CPM_DOWNLOAD_VERSION 0.40.8)
set(CPM_HASH_SUM "78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791")
set(CPM_DOWNLOAD_URL
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake")

if(CPM_SOURCE_CACHE)
    set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

# Expand a relative path, or one that starts with a tilde.
get_filename_component(CPM_DOWNLOAD_LOCATION "${CPM_DOWNLOAD_LOCATION}" ABSOLUTE)

# `INACTIVITY_TIMEOUT` rather than `TIMEOUT`: the bound is on silence, so a slow
# download that keeps delivering still completes. `STATUS` because a failed
# `file(DOWNLOAD)` without it is silent and leaves a truncated file behind, which
# the `include()` below would then report as a syntax error in a file nobody wrote.
file(DOWNLOAD
    "${CPM_DOWNLOAD_URL}"
    "${CPM_DOWNLOAD_LOCATION}"
    EXPECTED_HASH "SHA256=${CPM_HASH_SUM}"
    ${_coreCppCpmBound}
    STATUS cpmDownloadStatus
)
list(GET cpmDownloadStatus 0 cpmDownloadCode)
if(NOT cpmDownloadCode EQUAL 0)
    message(FATAL_ERROR
        "could not download the CPM.cmake bootstrap: ${cpmDownloadStatus}\n"
        "  from: ${CPM_DOWNLOAD_URL}\n"
        "  into: ${CPM_DOWNLOAD_LOCATION}\n"
        "Re-run the configure, point CPM_SOURCE_CACHE at a directory that already holds "
        "the bootstrap, or provide every dependency and set CORE_CPP_FETCH_DEPS=OFF. If the "
        "transfer stalled, cmake/FetchTransferBound.cmake is what abandoned it and why.")
endif()

include("${CPM_DOWNLOAD_LOCATION}")
