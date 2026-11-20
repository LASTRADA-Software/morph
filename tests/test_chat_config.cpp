// SPDX-License-Identifier: Apache-2.0
//
// A chat's configuration read from a file: what it accepts, what it refuses, and that a refusal never quotes a secret.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <morph/ui/chat/config.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace chat = morph::ui::chat;

namespace {

constexpr std::string_view kSecret = "sk-very-secret";

// An environment holding one variable.
std::optional<std::string> environment(std::string_view name) {
    return name == "THE_KEY" ? std::optional<std::string>{std::string{kSecret}} : std::nullopt;
}

}  // namespace

// Mutation: leave the key's variable unread.
TEST_CASE("a_whole_file_fills_the_config", "[chat][config]") {
    auto const config = chat::parseChatConfig(R"({
      "provider": { "name": "anthropic", "baseUrl": "https://proxy.example", "model": "the-model",
                    "apiKeyEnv": "THE_KEY", "maxTokens": 1000 },
      "limits": { "maxToolCallsPerTurn": 3, "maxTurnsPerPrincipal": 4, "maxTurnsPerConnection": 5,
                  "maxConversationsPerPrincipal": 6, "maxMessagesPerConversation": 7, "maxTextBytes": 8,
                  "maxToolResultBytes": 10, "maxConversationBytes": 11,
                  "maxProposalBytes": 12, "maxDraftBytes": 13, "maxInlineDocumentBytes": 14, "maxProposalsPerTurn": 2,
                  "idleRetentionMinutes": 9 },
      "systemPrompt": "Be brief." })",
                                              environment);
    REQUIRE(config.has_value());
    CHECK(config->provider.name == "anthropic");
    CHECK(config->provider.baseUrl == "https://proxy.example");
    CHECK(config->provider.model == "the-model");
    CHECK(config->provider.apiKey == kSecret);
    CHECK(config->provider.maxTokens == 1000);
    CHECK(config->limits.maxToolCallsPerTurn == 3);
    CHECK(config->limits.maxTurnsPerPrincipal == 4);
    CHECK(config->limits.maxTurnsPerConnection == 5);
    CHECK(config->limits.maxConversationsPerPrincipal == 6);
    CHECK(config->limits.maxMessagesPerConversation == 7);
    CHECK(config->limits.maxTextBytes == 8);
    CHECK(config->limits.maxToolResultBytes == 10);
    CHECK(config->limits.maxConversationBytes == 11);
    CHECK(config->limits.maxProposalBytes == 12);
    CHECK(config->limits.maxDraftBytes == 13);
    CHECK(config->limits.maxInlineDocumentBytes == 14);
    CHECK(config->limits.maxProposalsPerTurn == 2);
    CHECK(config->limits.idleRetention == std::chrono::minutes{9});
    CHECK(config->systemPrompt == "Be brief.");
}

// Mutation: require every limit.
TEST_CASE("what_a_file_leaves_out_keeps_its_default", "[chat][config]") {
    auto const config = chat::parseChatConfig(
        R"({ "provider": { "name": "anthropic", "model": "m", "apiKey": "inline" } })", environment);
    REQUIRE(config.has_value());
    chat::ChatLimits const defaults;
    CHECK(config->provider.apiKey == "inline");
    CHECK(config->provider.baseUrl.empty());
    CHECK(config->provider.maxTokens == chat::ProviderConfig{}.maxTokens);
    CHECK(config->limits.maxToolCallsPerTurn == defaults.maxToolCallsPerTurn);
    CHECK(config->limits.idleRetention == defaults.idleRetention);
    CHECK(config->systemPrompt.empty());
}

// Mutation: ignore unknown keys.
TEST_CASE("a_misspelled_key_is_refused", "[chat][config]") {
    CHECK_FALSE(chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
                                           "limits": { "maxToolCallsPerTrun": 3 } })",
                                      environment)
                    .has_value());
    CHECK_FALSE(chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKey": "k", "modle": "x" } })",
                                      environment)
                    .has_value());
    CHECK_FALSE(chat::parseChatConfig(R"({ "provders": {} })", environment).has_value());
}

// Mutation: fall back to an empty key.
TEST_CASE("a_key_variable_that_is_not_set_is_named", "[chat][config]") {
    auto const config =
        chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKeyEnv": "NOT_SET" } })", environment);
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().contains("NOT_SET"));
}

// Mutation: let the inline key win.
TEST_CASE("naming_both_a_key_and_its_variable_is_refused", "[chat][config]") {
    CHECK_FALSE(
        chat::parseChatConfig(
            R"({ "provider": { "name": "a", "model": "m", "apiKey": "k", "apiKeyEnv": "THE_KEY" } })", environment)
            .has_value());
}

// Mutation: quote the document in the refusal, as a JSON error normally would.
TEST_CASE("a_refusal_never_quotes_the_key", "[chat][config]") {
    std::string const text =
        R"({ "provider": { "name": "a", "model": "m", "apiKey": ")" + std::string{kSecret} + R"(", "oops": 1 } })";
    auto const config = chat::parseChatConfig(text, environment);
    REQUIRE_FALSE(config.has_value());
    CHECK_FALSE(config.error().contains(kSecret));
    std::string const broken = R"({ "provider": { "apiKey": ")" + std::string{kSecret} + R"(" )";
    auto const unreadable = chat::parseChatConfig(broken, environment);
    REQUIRE_FALSE(unreadable.has_value());
    CHECK_FALSE(unreadable.error().contains(kSecret));
}

// Mutation: accept a limit of zero, which would make every turn fail.
TEST_CASE("a_limit_of_zero_is_refused", "[chat][config]") {
    CHECK_FALSE(chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
                                           "limits": { "maxToolCallsPerTurn": 0 } })",
                                      environment)
                    .has_value());
    CHECK_FALSE(chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
                                           "limits": { "idleRetentionMinutes": 0 } })",
                                      environment)
                    .has_value());
    CHECK_FALSE(chat::parseChatConfig(
                    R"({ "provider": { "name": "a", "model": "m", "apiKey": "k", "maxTokens": 0 } })", environment)
                    .has_value());
}

// Mutation: treat a missing file as an empty configuration.
TEST_CASE("a_file_is_read_from_disk_and_a_missing_one_is_named", "[chat][config]") {
    auto const path = std::filesystem::temp_directory_path() / "morph_test_chat_config.json";
    {
        std::ofstream file{path};
        file << R"({ "provider": { "name": "anthropic", "model": "from-file", "apiKeyEnv": "THE_KEY" } })";
    }
    auto const config = chat::loadChatConfig(path, environment);
    std::filesystem::remove(path);
    REQUIRE(config.has_value());
    CHECK(config->provider.model == "from-file");

    auto const missing = chat::loadChatConfig(path, environment);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().contains(path.filename().string()));
}

// Mutation: require a key in the file.
TEST_CASE("a_file_may_name_no_key", "[chat][config]") {
    auto const config = chat::parseChatConfig(R"({ "provider": { "name": "anthropic", "model": "m" } })", environment);
    REQUIRE(config.has_value());
    CHECK(config->provider.apiKey.empty());
}

// Mutation: call the lookup without asking whether there is one.
TEST_CASE("a_key_variable_with_no_environment_is_named", "[chat][config]") {
    auto const config =
        chat::parseChatConfig(R"({ "provider": { "name": "anthropic", "model": "m", "apiKeyEnv": "THE_KEY" } })", {});
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().contains("THE_KEY"));
}

// Mutation: let a later limit's problem replace an earlier one's.
TEST_CASE("the_first_bad_limit_is_the_one_named", "[chat][config]") {
    auto const config = chat::parseChatConfig(R"({ "provider": { "name": "anthropic", "model": "m" },
      "limits": { "maxToolCallsPerTurn": 0, "maxTurnsPerPrincipal": 0 } })",
                                              environment);
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().contains("maxToolCallsPerTurn"));
    CHECK_FALSE(config.error().contains("maxTurnsPerPrincipal"));
}

// Mutation: leave the transcription block's key variable unread.
TEST_CASE("chat_config_dictation_block_and_limits_are_read", "[chat][config][dictation]") {
    auto const config = chat::parseChatConfig(R"({
      "provider": { "name": "anthropic", "model": "m", "apiKey": "k" },
      "transcription": { "name": "openai-transcriptions", "baseUrl": "https://stt.example", "model": "whisper-1",
                         "apiKeyEnv": "THE_KEY", "language": "en" },
      "limits": { "maxDictationSeconds": 30, "maxDictationBytes": 1000000, "maxTranscriptionsPerPrincipal": 3,
                  "transcriptionRetentionSeconds": 90 } })",
                                              environment);
    REQUIRE(config.has_value());
    REQUIRE(config->transcription.has_value());
    CHECK(config->transcription->name == "openai-transcriptions");
    CHECK(config->transcription->baseUrl == "https://stt.example");
    CHECK(config->transcription->model == "whisper-1");
    CHECK(config->transcription->apiKey == kSecret);
    CHECK(config->transcription->language == "en");
    CHECK(config->limits.maxDictationSeconds == 30);
    CHECK(config->limits.maxDictationBytes == 1000000);
    CHECK(config->limits.maxTranscriptionsPerPrincipal == 3);
    CHECK(config->limits.transcriptionRetention == std::chrono::seconds{90});
}

// Mutation: make the transcription block required.
TEST_CASE("chat_config_dictation_defaults_without_a_block", "[chat][config][dictation]") {
    auto const config =
        chat::parseChatConfig(R"({ "provider": { "name": "anthropic", "model": "m", "apiKey": "k" } })", environment);
    REQUIRE(config.has_value());
    CHECK_FALSE(config->transcription.has_value());
    CHECK(config->limits.maxDictationSeconds == 60);
    CHECK(config->limits.maxDictationBytes == std::size_t{4} * 1024 * 1024);
    CHECK(config->limits.maxTranscriptionsPerPrincipal == 2);
    CHECK(config->limits.transcriptionRetention == std::chrono::seconds{60});
}

// Mutation: let the transcription block ignore unknown keys, or let its inline key win over its variable.
TEST_CASE("chat_config_dictation_block_refuses_an_unknown_key_or_both_keys", "[chat][config][dictation]") {
    std::string const unknown = R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
      "transcription": { "name": "openai-transcriptions", "model": "w", "apiKey": ")" +
                                std::string{kSecret} + R"(", "voice": "x" } })";
    auto const refusedUnknown = chat::parseChatConfig(unknown, environment);
    REQUIRE_FALSE(refusedUnknown.has_value());
    CHECK_FALSE(refusedUnknown.error().contains(kSecret));
    auto const both = chat::parseChatConfig(R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" },
      "transcription": { "name": "openai-transcriptions", "model": "w", "apiKey": "k", "apiKeyEnv": "THE_KEY" } })",
                                            environment);
    REQUIRE_FALSE(both.has_value());
    CHECK(both.error() == "the chat configuration's `transcription` names both `apiKey` and `apiKeyEnv`");
}

// Mutation: accept a dictation limit of zero, or a byte bound the envelope could not carry.
TEST_CASE("chat_config_dictation_limit_of_zero_or_past_the_envelope_is_refused", "[chat][config][dictation]") {
    auto const withLimits = [](std::string const& limits) {
        return chat::parseChatConfig(
            R"({ "provider": { "name": "a", "model": "m", "apiKey": "k" }, "limits": {)" + limits + "} }",
            environment);
    };
    for (char const* const name : {"maxDictationSeconds", "maxDictationBytes", "maxTranscriptionsPerPrincipal",
                                   "transcriptionRetentionSeconds"}) {
        auto const refused = withLimits("\"" + std::string{name} + "\": 0");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains(name));
    }
    auto const past = withLimits("\"maxDictationBytes\": " + std::to_string(chat::kMaxDictationBytes + 1));
    REQUIRE_FALSE(past.has_value());
    CHECK(past.error() ==
          "the chat configuration's `maxDictationBytes` must be at most " + std::to_string(chat::kMaxDictationBytes));
    CHECK(withLimits("\"maxDictationBytes\": " + std::to_string(chat::kMaxDictationBytes)).has_value());
}
