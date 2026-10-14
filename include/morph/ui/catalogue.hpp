// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../render/i18n.hpp"
#include "bundle.hpp"
#include "message.hpp"
#include "value.hpp"

/// @file
/// @brief Message catalogues (spec 6 §19): a bundle's `messages` files read into a `TranslationProvider` that `t`
///        resolves through, and the search that matches a title in every locale the bundle has.
///
/// Specified in `docs/spec/ui/catalogue.md`.

namespace morph::ui {

/// @brief The most keys one catalogue may hold.
inline constexpr std::size_t kMaxCatalogueKeys = 16384;

/// @brief The id prefix of a message catalogue: `i18n/LOCALE.json`.
inline constexpr std::string_view kCataloguePrefix = "i18n/";

/// @brief The id suffix of a message catalogue.
inline constexpr std::string_view kCatalogueSuffix = ".json";

/// @brief One locale's messages, by key, with the patterns already parsed.
struct Catalogue {
    /// @brief The pattern text of each key whose pattern is in the `ui/1` subset.
    std::map<std::string, std::string, std::less<>> messages;
    /// @brief One line per key whose pattern is outside the subset; those keys show their fallback.
    std::vector<std::string> dropped;
};

/// @brief Reads the locale from a catalogue's id.
/// @param fileId The id, such as `i18n/de-AT.json`.
/// @return The locale (`de-AT`), or nothing when the id is not `i18n/LOCALE.json` with a locale of ASCII letters,
///         digits and `-` that starts with a letter.
[[nodiscard]] inline std::optional<std::string> catalogueLocale(std::string_view fileId) {
    if (!fileId.starts_with(kCataloguePrefix) || !fileId.ends_with(kCatalogueSuffix) ||
        fileId.size() <= kCataloguePrefix.size() + kCatalogueSuffix.size()) {
        return std::nullopt;
    }
    std::string_view const locale =
        fileId.substr(kCataloguePrefix.size(), fileId.size() - kCataloguePrefix.size() - kCatalogueSuffix.size());
    auto const letter = [](char const symbol) {
        return (symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z');
    };
    bool const valid = letter(locale.front()) && std::ranges::all_of(locale, [&letter](char const symbol) {
                           return letter(symbol) || (symbol >= '0' && symbol <= '9') || symbol == '-';
                       });
    return valid ? std::optional<std::string>{std::string{locale}} : std::nullopt;
}

/// @brief Parses a catalogue: a flat JSON object of message key to MessageFormat 2 pattern.
///
/// Refused: text that is not JSON or nests deeper than `kMaxJsonDepth`, anything but an object, a must-understand
/// (`!`) key, a value that is not a string, an empty key or one longer than `kMaxNameBytes`, and more than
/// `kMaxCatalogueKeys` keys. A pattern outside the `ui/1` subset does not refuse the catalogue: its key is left out
/// and listed in `dropped`, so `t` shows the fallback for it.
/// @param text The catalogue's JSON text.
/// @return The catalogue, or the reason it is refused.
[[nodiscard]] inline std::expected<Catalogue, std::string> parseCatalogue(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return std::unexpected(json.error());
    }
    auto const* const object = std::get_if<Json::object_t>(&json->data);
    if (object == nullptr) {
        return std::unexpected(std::string{"a catalogue is an object of message keys"});
    }
    if (auto reason = detail::mustUnderstand(*json, "the catalogue")) {
        return std::unexpected(std::move(*reason));
    }
    if (object->size() > kMaxCatalogueKeys) {
        return std::unexpected("a catalogue holds more than " + std::to_string(kMaxCatalogueKeys) + " keys");
    }
    Catalogue catalogue;
    for (auto const& [key, value] : *object) {
        if (key.empty() || key.size() > kMaxNameBytes) {
            return std::unexpected("a message key is 1 to " + std::to_string(kMaxNameBytes) + " bytes");
        }
        auto const* const pattern = std::get_if<std::string>(&value.data);
        if (pattern == nullptr) {
            return std::unexpected("the message `" + key + "` is not a string");
        }
        if (auto parsed = parseMessage(*pattern); !parsed) {
            catalogue.dropped.push_back("`" + key + "`: " + parsed.error());
            continue;
        }
        catalogue.messages.emplace(key, *pattern);
    }
    return catalogue;
}

/// @brief The message catalogues of one bundle, by locale, and the provider `t` resolves through.
class MessageCatalogues {
public:
    /// @brief Adds, or replaces, a locale's catalogue.
    /// @param locale    The locale.
    /// @param catalogue Its messages.
    void add(std::string locale, Catalogue catalogue) {
        auto next = std::make_shared<Table>(*_table);
        next->insert_or_assign(std::move(locale), std::move(catalogue.messages));
        _table = std::move(next);
    }

    /// @brief The locales with a catalogue, sorted.
    /// @return They.
    [[nodiscard]] std::vector<std::string> locales() const {
        std::vector<std::string> out;
        out.reserve(_table->size());
        for (auto const& [locale, messages] : *_table) {
            static_cast<void>(messages);
            out.push_back(locale);
        }
        return out;
    }

    /// @brief What went wrong while loading: a catalogue left out, or a pattern dropped. Each line names the file.
    /// @return The lines.
    [[nodiscard]] std::vector<std::string> const& problems() const noexcept { return _problems; }

    /// @brief Records a loading problem.
    /// @param line What went wrong, naming the file.
    void note(std::string line) { _problems.push_back(std::move(line)); }

    /// @brief A message's pattern in a locale: the exact locale's catalogue, then its language's (`de-AT`, then
    ///        `de`).
    /// @param key    The message key.
    /// @param locale The BCP 47 locale.
    /// @return The pattern, or nothing; `t` then shows the fallback.
    [[nodiscard]] std::optional<std::string> lookup(std::string_view key, std::string_view locale) const {
        return find(*_table, key, locale);
    }

    /// @brief A provider over the catalogues as they are now. It shares them, so it stays valid after this object is
    ///        gone, and does not see a later `add`.
    /// @return The provider, for `FormatContext::translations` (`Session::setTranslations`).
    [[nodiscard]] render::TranslationProvider provider() const {
        return [table = _table](std::string_view key, std::string_view locale) { return find(*table, key, locale); };
    }

private:
    using Table = std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>>;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): key, then locale, as TranslationProvider takes them.
    [[nodiscard]] static std::optional<std::string> find(Table const& table, std::string_view key,
                                                         std::string_view locale) {
        std::string_view tag = locale;
        while (true) {
            if (auto const messages = table.find(tag); messages != table.end()) {
                if (auto const message = messages->second.find(key); message != messages->second.end()) {
                    return message->second;
                }
            }
            auto const dash = tag.rfind('-');
            if (dash == std::string_view::npos) {
                return std::nullopt;
            }
            tag = tag.substr(0, dash);
        }
    }

    std::shared_ptr<Table const> _table = std::make_shared<Table const>();
    std::vector<std::string> _problems;
};

/// @brief Reads every message catalogue a verified bundle lists.
///
/// A catalogue whose id is not `i18n/LOCALE.json`, whose bytes are not among @p files, or that `parseCatalogue`
/// refuses, is left out, and its locale shows the fallbacks; a pattern outside the subset is dropped. Each is noted
/// in `problems()`. Nothing is refused later, at a `t`.
/// @param manifest The bundle's manifest (`LoadedBundle::manifest`).
/// @param files    The verified files by content hash (`LoadedBundle::files`).
/// @return The catalogues.
[[nodiscard]] inline MessageCatalogues loadCatalogues(Manifest const& manifest,
                                                      std::map<std::string, std::string, std::less<>> const& files) {
    MessageCatalogues catalogues;
    for (ManifestFile const& file : manifest.files) {
        if (file.kind != kMessagesFile) {
            continue;
        }
        auto locale = catalogueLocale(file.id);
        if (!locale) {
            catalogues.note("`" + file.id + "`: a catalogue's id is i18n/<locale>.json");
            continue;
        }
        auto const bytes = files.find(file.hash);
        if (bytes == files.end()) {
            catalogues.note("`" + file.id + "`: not among the verified files");
            continue;
        }
        auto catalogue = parseCatalogue(bytes->second);
        if (!catalogue) {
            catalogues.note("`" + file.id + "`: " + catalogue.error());
            continue;
        }
        for (std::string const& dropped : catalogue->dropped) {
            catalogues.note("`" + file.id + "`: " + dropped);
        }
        catalogues.add(std::move(*locale), std::move(*catalogue));
    }
    return catalogues;
}

/// @brief A message formatted in every locale the catalogues hold: what a search over titles compares against.
/// @param catalogues The catalogues.
/// @param key        The message key.
/// @param fallback   The source text, shown for a locale without the key.
/// @return One text per locale, in the order of `locales()`; the fallback alone when there are no catalogues.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): key, then fallback, as `t` takes them.
[[nodiscard]] inline std::vector<std::string> textInEveryLocale(MessageCatalogues const& catalogues,
                                                                std::string_view key, std::string_view fallback) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::vector<std::string> const locales = catalogues.locales();
    std::vector<std::string> out;
    auto const format = [](std::string_view pattern, std::string const& locale) -> std::optional<std::string> {
        auto parsed = parseMessage(pattern);
        if (!parsed) {
            return std::nullopt;
        }
        return formatMessage(*parsed, ValueRecord{}, MessageLocale{.locale = locale, .numeric = {}}).text;
    };
    if (locales.empty()) {
        out.push_back(format(fallback, "en").value_or(std::string{fallback}));
        return out;
    }
    out.reserve(locales.size());
    for (std::string const& locale : locales) {
        std::optional<std::string> text;
        if (auto const pattern = catalogues.lookup(key, locale)) {
            text = format(*pattern, locale);
        }
        if (!text) {
            text = format(fallback, locale);
        }
        out.push_back(text.value_or(std::string{fallback}));
    }
    return out;
}

/// @brief Whether a message contains @p query in any locale the catalogues hold, or in its fallback: a navigator's
///        search, so a user finds a screen by the title they know in any language.
///
/// ASCII letters compare without case; every other byte compares exactly.
/// @param catalogues The catalogues.
/// @param key        The message key.
/// @param fallback   The source text.
/// @param query      What the user typed; an empty query matches.
/// @return Whether any locale's text contains it.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): key, fallback and query, as `t` and a search box give them.
[[nodiscard]] inline bool matchesInAnyLocale(MessageCatalogues const& catalogues, std::string_view key,
                                             std::string_view fallback, std::string_view query) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto const fold = [](std::string_view text) {
        std::string out{text};
        std::ranges::transform(out, out.begin(), [](char const symbol) {
            return symbol >= 'A' && symbol <= 'Z' ? static_cast<char>(symbol - 'A' + 'a') : symbol;
        });
        return out;
    };
    std::string const needle = fold(query);
    std::vector<std::string> texts = textInEveryLocale(catalogues, key, fallback);
    texts.emplace_back(fallback);
    return std::ranges::any_of(texts, [&](std::string const& text) { return fold(text).contains(needle); });
}

}  // namespace morph::ui
