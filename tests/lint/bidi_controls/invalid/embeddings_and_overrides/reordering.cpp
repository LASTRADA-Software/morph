// SPDX-License-Identifier: Apache-2.0
//
// The embeddings, their overrides and their terminator. These are the
// trojan-source characters: they change how the line renders relative to
// how it compiles, so the reviewer and the compiler read different
// programs.

constexpr const char* kEmbed = "‪a‬";
constexpr const char* kEmbedRtl = "‫a‬";
constexpr const char* kOverride = "‭a‮b";
