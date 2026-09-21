// SPDX-License-Identifier: Apache-2.0
//
// The embeddings, their overrides and their terminator. These are the
// trojan-source characters: they change how the line renders relative to
// how it compiles, so the reviewer and the compiler read different
// programs.
//
// `inline` keeps clang-diagnostic-unused-const-variable off a fixture
// nothing compiles; see invalid/arabic_letter_mark/.

inline constexpr const char* kEmbed = "‪a‬";
inline constexpr const char* kEmbedRtl = "‫a‬";
// LRE with no terminating PDF: misc-misleading-bidirectional reads the
// string's *content* and is right about it. This gate reads the source
// *bytes*, which is why the fixture has to keep them.
// NOLINTNEXTLINE(misc-misleading-bidirectional)
inline constexpr const char* kOverride = "‭a‮b";
