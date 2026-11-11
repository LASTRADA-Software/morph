// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/// @file
/// A refused action's code and named arguments, which travel to the client beside the message so a screen can word
/// the refusal in the user's locale. See `docs/spec/ui/refusals.md`.

namespace morph::model {

/// @brief One named argument of an `ActionRefusal`: a fact the refusal's sentence needs.
struct RefusalArg {
    /// @brief The argument's name; the sentence reads it as the variable `$name`.
    std::string name;
    /// @brief The argument's value, as the text the sentence should show.
    std::string value;
};

/// @brief The exception an action throws to refuse itself with a code the client can word.
///
/// The message is for logs and tests and is never shown to a person; the code and the arguments are what a screen
/// words from its message catalogue.
class ActionRefusal : public std::runtime_error {
public:
    /// @brief A refusal.
    /// @param code    The application's dotted code, such as `lims.billing.invoiceLeased`.
    /// @param args    The named arguments of the sentence.
    /// @param message The text for logs, returned by `what()`.
    ActionRefusal(std::string code, std::vector<RefusalArg> args, std::string message)
        : std::runtime_error{std::move(message)}, _code{std::move(code)}, _args{std::move(args)} {}

    /// @brief The refusal's code.
    /// @return The code.
    [[nodiscard]] std::string const& code() const noexcept { return _code; }

    /// @brief The refusal's named arguments.
    /// @return The arguments, in the order given.
    [[nodiscard]] std::vector<RefusalArg> const& args() const noexcept { return _args; }

private:
    std::string _code;
    std::vector<RefusalArg> _args;
};

}  // namespace morph::model
