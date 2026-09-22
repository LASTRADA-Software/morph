// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>

#include "action_log.hpp"

/// @file
/// @brief The `LogEntry` JSON codec, kept apart from `action_log.hpp`.
///
/// `action_log.hpp` defines `LogEntry`, `Outcome`, `IActionLog` and the
/// process-wide log slot; `core/model.hpp` includes it for `IActionLog` alone,
/// and every consumer that reaches a model therefore used to compile
/// `<glaze/glaze.hpp>` whether or not it ever serialised anything. That is the
/// cliff morph#521 measured and morph#573 step 4 names: `action_log.hpp` was
/// 252,559 preprocessed lines and 2.67 CPU-s against `core/strand.hpp`'s
/// 127,217 and 1.20.
///
/// Include this header instead when you need `toJson`/`fromJson`. Inside
/// morph, exactly one header does: `file_action_log.hpp`.

/// @brief Reflects `Outcome` as the strings `"Succeeded"`/`"Failed"` rather than
/// its underlying `0`/`1`, so a journal line stays readable without
/// cross-referencing the enum. `LogEntry` otherwise needs no `glz::meta` at
/// all (see `toJson`'s doc comment) -- this is the one field that does.
template <>
struct glz::meta<morph::journal::Outcome> {
    using enum morph::journal::Outcome;
    static constexpr auto value = glz::enumerate(Succeeded, Failed);
};

namespace morph::journal {

namespace detail {

/// @brief Converts a Glaze error into a `SerializationError`, or does nothing
///        if @p errCode reports success.
///
/// Shared by `toJson` and `fromJson` on purpose, not just for DRY: this is one
/// non-template function, so both call through the exact same compiled branch.
/// `fromJson`'s failure path is easy to exercise for real (malformed JSON is
/// everyday input); `toJson`'s is not — Glaze's buffer-writer has no reachable
/// failure mode for a flat struct of strings/integers like `LogEntry` (its
/// only write-relevant error codes are for recursion-depth limits `LogEntry`
/// can't hit, and `dump_int_error`, which nothing in Glaze's own source ever
/// actually raises). Routing both through here means `toJson`'s error branch
/// is the same branch `fromJson`'s test already exercises, rather than a
/// second, structurally-unreachable copy of the same three lines.
/// @param errCode Result of a `glz::write_json`/`glz::read_json` call.
/// @param context Buffer or input passed to `glz::format_error` for the message.
inline void throwOnGlazeError(const glz::error_ctx& errCode, std::string_view context) {
    if (errCode) {
        throw SerializationError{glz::format_error(errCode, context)};
    }
}

/// @brief Write options that escape ASCII control bytes as `\\uXXXX` sequences.
///
/// glaze 7.4 leaves control bytes (0x00-0x1F) unescaped by default, which
/// breaks a `LogEntry` carrying one in `entityKey`/`payload`/`error`/`principal`/
/// `idempotencyKey` two ways: RFC 8259 requires those bytes escaped, so the raw
/// byte alone yields JSON `fromJson`'s `glz::read` throws on; worse, once the
/// same string also contains an escaped `\` or `"`, glaze's chunked writer path
/// silently rewrites the control byte as two 0x00 bytes, corrupting the payload
/// before it ever reaches disk. This mirrors `morph::wire::detail::EscapingWriteOpts`
/// (`core/wire.hpp`) exactly; duplicated here (rather than shared) so this header
/// stays free of a `core/` dependency for `MORPH_CLIENT_ONLY`-style consumers
/// that only want the journal. Escaping is lossless, so any such byte still
/// round-trips through `fromJson` unchanged.
struct EscapingWriteOpts : glz::opts {
    /// @brief Emit control bytes as `\\uXXXX` rather than raw.
    // NOLINTNEXTLINE(readability-identifier-naming) — glaze's option name, matched by name.
    bool escape_control_characters = true;
};

}  // namespace detail

/// @brief Encodes @p entry as JSON.
///
/// `LogEntry` is a plain aggregate, so Glaze reflects it without a `glz::meta`
/// specialisation of its own — the same automatic reflection
/// `BRIDGE_REGISTER_ACTION` relies on for user action structs. (Its `outcome`
/// field is `Outcome`, which does have a `glz::meta` — see above — so it reads
/// back as `"Succeeded"`/`"Failed"`, not `0`/`1`.) Used by sinks that need an
/// opaque string representation (`FileActionLog`).
///
/// Writes with `detail::EscapingWriteOpts` so a raw ASCII control byte in any
/// string field (`entityKey`, `payload`, `error`, `principal`, `idempotencyKey`)
/// round-trips through `fromJson` instead of producing invalid or silently
/// corrupted JSON — see that struct's doc comment.
/// @param entry Entry to encode.
/// @return The entry as a single line of JSON.
/// @throws SerializationError on encode failure (see `detail::throwOnGlazeError`
///         for why this is not realistically reachable for `LogEntry`).
inline std::string toJson(const LogEntry& entry) {
    std::string out;
    detail::throwOnGlazeError(glz::write<detail::EscapingWriteOpts{}>(entry, out), out);
    return out;
}

/// @brief Decodes @p json into a `LogEntry`.
///
/// Reads leniently: `glz::read<glz::opts{.error_on_unknown_keys = false}>`,
/// the same stance `morph::wire::decode` takes (`include/morph/core/wire.hpp`) —
/// an unknown/extra JSON key (e.g. an additive field written by a newer morph
/// build) is ignored rather than rejected. Same duplicate-key caveat as
/// `wire::decode`: last-wins, not a security boundary (glaze offers no reject
/// option). Syntactically malformed JSON still throws. After a successful
/// decode, also enforces the line-format version rule: `v <= kLogFormatVersion`
/// decodes normally, `v` greater than this build's `kLogFormatVersion` throws
/// — a build refuses to guess at a line format newer than any it has seen.
/// @param json One JSON-encoded entry, as `toJson` wrote it.
/// @return The decoded entry.
/// @throws SerializationError if @p json is not valid JSON, does not decode
///         into a `LogEntry`, or decodes with `v` greater than
///         `kLogFormatVersion`.
inline LogEntry fromJson(std::string_view json) {
    LogEntry entry{};
    // `null_terminated = false`: `json` is a caller-supplied view (a line read from a
    // journal file, with no guaranteed trailing '\0') — see the identical rationale on
    // `morph::wire::decode` (`wire.hpp`), whose fuzz harness found the resulting
    // heap-buffer-overflow in glaze's `skip_ws`.
    static constexpr glz::opts kLenient{.null_terminated = false, .error_on_unknown_keys = false};
    detail::throwOnGlazeError(glz::read<kLenient>(entry, json), json);
    if (entry.v > kLogFormatVersion) {
        throw SerializationError{
            "journal::fromJson: line format v" + std::to_string(entry.v) +
            " is newer than this build supports (kLogFormatVersion = " + std::to_string(kLogFormatVersion) + ")"};
    }
    return entry;
}

}  // namespace morph::journal
