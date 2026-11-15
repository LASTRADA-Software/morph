// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../reactive/signal.hpp"
#include "interpreter.hpp"
#include "shell_parts.hpp"
#include "shell_parts_view.hpp"

/// @file
/// @brief `morph::ui::ShellPartsModel`: reads an application's declared shell parts from the app root's queries and
///        answers what a `ShellFrame` draws, holds the chosen scope, and handles the `setContext` command.
///
/// Specified in `docs/spec/ui/shell_parts.md`.

namespace morph::ui {

/// @brief A `morph.shell.*` text: the bundle's catalogue wins, then the built-in English or German.
/// @param key    The key.
/// @param format The session's formatting facts.
/// @return The text.
[[nodiscard]] MORPH_API std::string shellText(std::string_view key, FormatContext const& format);

/// @brief The frame's texts in the session's locale.
/// @param format The session's formatting facts.
/// @return The labels.
[[nodiscard]] MORPH_API ShellLabels shellLabels(FormatContext const& format);

/// @brief What the model asks of its client.
struct ShellModelOptions {
    /// @brief A module's label in the navigator, for a search heading with no title of its own.
    std::function<std::string(std::string_view module)> moduleLabel;
    /// @brief The title of a screen in the catalog, for the inbox button.
    std::function<std::string(std::string_view screen)> screenTitle;
    /// @brief Whether the catalog lists a screen. A part that names one the catalog does not list is left out, not
    ///        reported: the authoriser hides it from this person. Empty: every screen is taken to be listed.
    std::function<bool(std::string_view screen)> screenListed;
    /// @brief Whether the catalog lists a module. A part that names one the catalog does not list is left out, not
    ///        reported: the authoriser hides it from this person. Empty: every module is taken to be listed.
    std::function<bool(std::string_view module)> moduleListed;
    /// @brief Whether the catalog says an action's reply holds a registered record, which is what makes a read of it
    ///        refresh when the record changes. Empty: every action is taken to.
    std::function<bool(std::string_view action)> replyIsRecord;
    /// @brief The locales the bundle has catalogues for, for a menu that lists none of its own.
    std::function<std::vector<std::string>()> catalogueLocales;
    /// @brief How many open tabs hold unsaved changes.
    std::function<std::size_t()> dirtyTabs;
    /// @brief Asks once whether to discard the unsaved work of @p tabs tabs; the answer comes to @p done.
    std::function<void(std::size_t tabs, std::function<void(bool)> done)> confirmDiscard;
    /// @brief Closes the tabs with unsaved changes without asking.
    std::function<void()> closeDirtyTabs;
    /// @brief Receives each problem once.
    std::function<void(std::string const&)> onProblem;
};

/// @brief The declared parts of a shell, read from the app root.
///
/// Neither copyable nor movable; used on the owner thread of its session. It borrows the session, the app root and the
/// parts, and must go before any of them.
class MORPH_API ShellPartsModel {
public:
    /// @brief The model; it installs the `setContext` handler and starts following the switcher.
    /// @param session The session. Borrowed.
    /// @param root    The app root whose queries the parts are. Borrowed.
    /// @param parts   The parts. Borrowed.
    /// @param options What it asks of its client.
    ShellPartsModel(Session& session MORPH_LIFETIMEBOUND, AppRoot& root MORPH_LIFETIMEBOUND,
                    ShellParts const& parts MORPH_LIFETIMEBOUND, ShellModelOptions options);

    ShellPartsModel(ShellPartsModel const&) = delete;
    ShellPartsModel& operator=(ShellPartsModel const&) = delete;
    ShellPartsModel(ShellPartsModel&&) = delete;
    ShellPartsModel& operator=(ShellPartsModel&&) = delete;

    /// @brief Takes the handler and the scope off the session.
    ~ShellPartsModel();

    /// @brief Whether the switcher has answered or failed, or none is declared: the start screen may mount. A tracked
    ///        read.
    /// @return Whether it is ready.
    [[nodiscard]] bool contextReady() const { return _answered.get(); }

    /// @brief What the frame draws. A tracked read.
    /// @return The view.
    [[nodiscard]] ShellPartsView view() const;

    /// @brief Sets the connection state the footer draws.
    /// @param state The client's connection.
    void setConnection(ConnectionState state) { _connection.set(state); }

    /// @brief One module's count. A tracked read.
    struct ModuleState {
        /// @brief The module.
        std::string module;
        /// @brief The count; empty while unanswered.
        std::optional<long long> count;
        /// @brief Whether the read failed.
        bool failed = false;
    };

    /// @brief Every declared module's count. A tracked read.
    /// @return The counts, in declaration order; a denied one, and one of a module the catalog does not list, left
    /// out.
    [[nodiscard]] std::vector<ModuleState> moduleCounts() const;

    /// @brief Chooses an option of the switcher, as the person does: the same checks as the command's.
    /// @param index The option's place in the last answer.
    void chooseContext(std::size_t index);

    /// @brief The `setContext` command: changes the scope to @p value if it is one of the options, asking once first
    ///        when open tabs hold unsaved changes.
    /// @param value The option's value.
    /// @return Whether the request was accepted: the value is an option's. The switch itself may wait for the answer.
    bool requestContext(Value const& value);

    /// @brief Sets the search field's text; the sources are asked after their pause.
    /// @param text The text.
    void setSearchText(std::string const& text);

    /// @brief A screen a hit opens.
    struct Target {
        /// @brief The screen.
        std::string screen;
        /// @brief Its params.
        ValueRecord params;
    };

    /// @brief The screen a drawn hit opens. Not a tracked read.
    /// @param source The source's place.
    /// @param hit    The hit's place among the source's drawn hits.
    /// @return The target; empty for a place that is not drawn.
    [[nodiscard]] std::optional<Target> hit(std::size_t source, std::size_t hit) const;

    /// @brief Every drawn hit's target in order, at most `kSearchOpenAllLimit`. Not a tracked read.
    /// @return The targets.
    [[nodiscard]] std::vector<Target> allHits() const;

    /// @brief The screen the inbox button opens.
    /// @return It; empty with no inbox.
    [[nodiscard]] std::string inboxScreen() const { return _parts->inbox ? _parts->inbox->screen : std::string{}; }

    /// @brief The screen the help button opens.
    /// @return It with its params; empty when the button runs an action, or none is declared.
    [[nodiscard]] std::optional<Target> helpScreen() const;

    /// @brief Runs the action the help button names. A failure is reported once, as any part's.
    /// @return Whether an action was run: one is declared.
    bool runHelp();

    /// @brief Chooses a language from the person's menu.
    /// @param index The place in the menu's list.
    void chooseLocale(std::size_t index);

    /// @brief Chooses the number, money and date format from the person's menu.
    /// @param index The place in the menu's list.
    void chooseFormat(std::size_t index);

    /// @brief The language the session is in.
    /// @return Its tag.
    [[nodiscard]] std::string const& currentLocale() const { return _session->formatting().locale; }

private:
    struct Option {
        Value value;
        ShellOptionView shown;

        bool operator==(Option const&) const = default;
    };

    // A module or screen the catalog does not list is hidden from this person by the authoriser, so the part that
    // names it is left out without a word. An unknown name is the server's refusal at load, not the client's report.
    [[nodiscard]] bool moduleHidden(std::string const& module) const {
        return !module.empty() && _options.moduleListed && !_options.moduleListed(module);
    }

    [[nodiscard]] bool screenHidden(std::string const& screen) const {
        return _options.screenListed && !_options.screenListed(screen);
    }

    // What the catalog can say about the declaration, reported once at the start: a count whose reply is not a
    // registered record (it is then read on mount and on a context change only).
    void checkDeclaration() const;

    // Reports a problem once per text.
    void problem(std::string const& text) const;

    // A reply value as the text a widget shows: a string as itself, a typed value in the locale, a message reference
    // through the catalogue.
    [[nodiscard]] static std::string describe(Value const& value, FormatContext const& format);

    [[nodiscard]] static std::string translate(std::string const& textOrKey, FormatContext const& format);

    [[nodiscard]] static std::string memberText(Value const& record, std::string const& name,
                                                FormatContext const& format);

    // A count is a JSON integer, or the wire form of a `morph::units::Quantity`, whose magnitude is a whole number: a
    // `Quantity` is written as its exact value (`num`, `den`, `dp`), which a client reads as a decimal, and with a
    // `unit` as a quantity. The unit is the application's and is not read. Anything else, and a negative number, is
    // not a count.
    [[nodiscard]] std::optional<long long> countOf(Value const& reply, std::string const& field,
                                                   std::string const& query) const;

    // The switcher's options, from the last answer.
    [[nodiscard]] std::vector<Option> parseOptions(AppRoot::QueryRead const& read, FormatContext const& format);

    // Keeps the chosen scope among the options: the first option when it is not, none when there are none.
    void followOptions();

    void apply(Value const& value);

    [[nodiscard]] ShellContextView contextView(FormatContext const& format) const;

    // Whether a reply item is a hit: it names a screen and has a title.
    [[nodiscard]] static bool isHit(Value const& item);

    [[nodiscard]] std::vector<Target> sourceHits(std::size_t source) const;

    [[nodiscard]] ShellSearchView searchView(FormatContext const& format) const;

    [[nodiscard]] ShellInboxView inboxView(FormatContext const& format) const;

    [[nodiscard]] std::vector<std::string> localeTags() const;

    [[nodiscard]] ShellBrandView brandView(FormatContext const& format) const;

    // The button is left out where the catalog does not list its screen to this person.
    [[nodiscard]] ShellHelpView helpView() const;

    // The declared params as the document values their literals are, which is what a document's own literals become.
    [[nodiscard]] ValueRecord helpParams() const;

    [[nodiscard]] ShellPrincipalView principalView(FormatContext const& format) const;

    Session* _session;
    AppRoot* _root;
    ShellParts const* _parts;
    ShellModelOptions _options;
    reactive::Signal<Value> _chosen;
    reactive::Signal<bool> _answered;
    reactive::Signal<std::string> _searchText;
    reactive::Signal<std::vector<Option>> _optionList;
    reactive::Signal<std::optional<ConnectionState>> _connection;
    mutable std::set<std::string> _reported;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::unique_ptr<reactive::Effect> _follow;
};

}  // namespace morph::ui
