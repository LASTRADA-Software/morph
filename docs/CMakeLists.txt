find_package(Doxygen REQUIRED)
message(STATUS "Doxygen found: ${DOXYGEN_EXECUTABLE}")

include(FetchContent)
FetchContent_Declare(
    doxygen-awesome-css
    GIT_REPOSITORY https://github.com/jothepro/doxygen-awesome-css.git
    GIT_TAG        v2.3.4
)
FetchContent_MakeAvailable(doxygen-awesome-css)

# ── Doxygen settings ────────────────────────────────────────────────────────
set(DOXYGEN_PROJECT_NAME           "morph")
set(DOXYGEN_PROJECT_BRIEF          "Asynchronous bridge between GUI thread and business models")
set(DOXYGEN_BUILTIN_STL_SUPPORT    YES)
set(DOXYGEN_FILE_PATTERNS          *.hpp)
set(DOXYGEN_RECURSIVE              YES)
set(DOXYGEN_GENERATE_TREEVIEW      YES)
set(DOXYGEN_HIDE_UNDOC_CLASSES     YES)
set(DOXYGEN_HIDE_FRIEND_COMPOUNDS  YES)
set(DOXYGEN_HIDE_IN_BODY_DOCS      YES)
set(DOXYGEN_MARKDOWN_ID_STYLE      GITHUB)
set(DOXYGEN_QT_AUTOBRIEF           YES)
set(DOXYGEN_SORT_BY_SCOPE_NAME     YES)
set(DOXYGEN_SORT_MEMBER_DOCS       NO)
set(DOXYGEN_SOURCE_BROWSER         YES)
set(DOXYGEN_STRIP_CODE_COMMENTS    NO)
set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${PROJECT_SOURCE_DIR}/docs/ARCHITECTURE.md")
set(DOXYGEN_HTML_EXTRA_STYLESHEET  "${doxygen-awesome-css_SOURCE_DIR}/doxygen-awesome.css")
set(DOXYGEN_OUTPUT_DIRECTORY       "${PROJECT_BINARY_DIR}/html")
set(DOXYGEN_FULL_PATH_NAMES        YES)
set(DOXYGEN_STRIP_FROM_PATH        "${PROJECT_SOURCE_DIR}/include")
set(DOXYGEN_CREATE_SUBDIRS         YES)

# ── Strictness: every public symbol must be documented ──────────────────────
set(DOXYGEN_EXTRACT_ALL            NO)
set(DOXYGEN_EXTRACT_LOCAL_CLASSES  NO)
set(DOXYGEN_WARN_IF_UNDOCUMENTED   YES)
set(DOXYGEN_WARN_IF_DOC_ERROR      YES)
set(DOXYGEN_WARN_NO_PARAMDOC       YES)
set(DOXYGEN_WARN_AS_ERROR          FAIL_ON_WARNINGS)

# ── Hide every per-topic detail namespace and the catch-all std/detail ──────
set(DOXYGEN_EXCLUDE_SYMBOLS
    "morph::log::detail"
    "morph::log::detail::*"
    "morph::observe::detail"
    "morph::observe::detail::*"
    "morph::exec::detail"
    "morph::exec::detail::*"
    "morph::async::detail"
    "morph::async::detail::*"
    "morph::model::detail"
    "morph::model::detail::*"
    "morph::backend::detail"
    "morph::backend::detail::*"
    "morph::bridge::detail"
    "morph::bridge::detail::*"
    "morph::offline::detail"
    "morph::offline::detail::*"
    "morph::qt::detail"
    "morph::qt::detail::*"
    "morph::net::detail"
    "morph::net::detail::*"
    "morph::math::detail"
    "morph::math::detail::*"
    "morph::units::detail"
    "morph::units::detail::*"
    "morph::forms::detail"
    "morph::forms::detail::*"
    # Third-party customisation points (glaze codecs/schemas, std::formatter)
    # are integration shims, not morph API surface.
    "glz"
    "glz::*"
    "std::formatter*"
)

set(DOCS_SOURCES
    "${PROJECT_SOURCE_DIR}/include/morph"
    "${PROJECT_SOURCE_DIR}/docs/ARCHITECTURE.md"
)

doxygen_add_docs(doc
    ${DOCS_SOURCES}
    ALL
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Generate HTML documentation"
)
