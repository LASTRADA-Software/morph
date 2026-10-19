// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <morph/client/args.hpp>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::client {

namespace {

/// The value options, by name, and where each is read into.
struct ValueOption {
    std::string_view name;
    std::function<void(Args&, std::string)> store;
};

std::array<ValueOption, 5> const& valueOptions() {
    static std::array<ValueOption, 5> const options{{
        {.name = "--server", .store = [](Args& out, std::string value) { out.server = std::move(value); }},
        {.name = "--app", .store = [](Args& out, std::string value) { out.app = std::move(value); }},
        {.name = "--db", .store = [](Args& out, std::string value) { out.db = std::move(value); }},
        {.name = "--user", .store = [](Args& out, std::string value) { out.user = std::move(value); }},
        {.name = "--screen", .store = [](Args& out, std::string value) { out.screen = std::move(value); }},
    }};
    return options;
}

constexpr std::string_view kExitAfterMount = "--exit-after-mount";
constexpr std::string_view kDev = "--dev";

/// What reading one argument as a value option did: how many arguments it used, or why it is refused.
struct OptionRead {
    std::size_t used = 0;
    std::string error;
};

// Reads `args[index]` as a value option, `--name value` or `--name=value`, into @p read. It uses no argument when
// `args[index]` is no value option.
OptionRead readValueOption(std::vector<std::string> const& args, std::size_t index, Args& read) {
    std::string_view const arg = args.at(index);
    for (ValueOption const& option : valueOptions()) {
        if (arg == option.name) {
            if (index + 1 >= args.size() || std::string_view{args.at(index + 1)}.starts_with("--")) {
                return OptionRead{.used = 0, .error = std::string{option.name} + " needs a value"};
            }
            option.store(read, args.at(index + 1));
            return OptionRead{.used = 2, .error = {}};
        }
        if (arg.starts_with(option.name) && arg.size() > option.name.size() && arg.at(option.name.size()) == '=') {
            std::string value{arg.substr(option.name.size() + 1)};
            if (value.empty()) {
                return OptionRead{.used = 0, .error = std::string{option.name} + " needs a value"};
            }
            option.store(read, std::move(value));
            return OptionRead{.used = 1, .error = {}};
        }
    }
    return OptionRead{};
}

}  // namespace

std::expected<Args, std::string> readArgs(std::vector<std::string>& args) {
    Args read;
    std::vector<std::size_t> used;
    for (std::size_t index = 1; index < args.size(); ++index) {
        std::string_view const arg = args.at(index);
        if (arg == "--") {
            break;
        }
        if (arg == kExitAfterMount || arg == kDev) {
            (arg == kDev ? read.dev : read.exitAfterMount) = true;
            used.push_back(index);
            continue;
        }
        OptionRead const option = readValueOption(args, index, read);
        if (!option.error.empty()) {
            return std::unexpected(option.error);
        }
        for (std::size_t offset = 0; offset < option.used; ++offset) {
            used.push_back(index + offset);
        }
        if (option.used > 1) {
            index += option.used - 1;
        }
    }
    if (read.server.has_value() == read.app.has_value()) {
        return std::unexpected(std::string{"give exactly one of --server URL and --app NAME"});
    }
    for (std::size_t const index : used | std::views::reverse) {
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return read;
}

}  // namespace morph::client
