// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bundle.hpp"
#include "interpreter.hpp"
#include "value.hpp"

/// @file
/// @brief `HostVocabulary`: the commands and values only the client application has (`host/1`, spec 6 §14),
///        registered with their types and versions, run with checked arguments behind a trust gate, and declared to
///        the server in `ui-hello`.
///
/// Specified in `docs/spec/ui/host_vocabulary.md`.

namespace morph::ui {

/// @brief One argument of a host command.
struct HostArg {
    /// @brief The argument's name, the key of the document's `args` record.
    std::string name;
    /// @brief Its document type, such as `string`, `int?` or a list of strings.
    std::string type;
};

/// @brief A command the application registers, such as `restartApp` or `openPath`.
struct HostCommand {
    /// @brief The command's name, as `{"host": name}` gives it.
    std::string name;
    /// @brief Its version, declared in `ui-hello`.
    std::int64_t version = 1;
    /// @brief Its arguments. An optional type makes the argument optional.
    std::vector<HostArg> args;
    /// @brief Whether a document that came over a connection that is not verified may run it. A command that opens
    ///        files, reaches the network or touches the host is not.
    bool safeForUntrusted = false;
    /// @brief Runs the command with its checked arguments, by name; an absent optional argument is `null`.
    std::function<void(ValueRecord const&)> run;
};

/// @brief A value the application registers, such as `sync.needsYouCount`.
struct HostValue {
    /// @brief The value's name, as `{"host": name}` reads it.
    std::string name;
    /// @brief Its version, declared in `ui-hello`.
    std::int64_t version = 1;
    /// @brief Its document type.
    std::string type;
    /// @brief Its value until the application sets another; it must have the type.
    Value initial;
};

/// @brief The client's host vocabulary: what it registered, and whether its documents came over a verified
///        connection.
///
/// An application builds one at start-up, registers its commands and values, connects it to the interpreter's
/// `Session`, and tells it when the connection is verified (spec 5 §13: a verified TLS connection, or loopback with
/// the development flag). Until then a command not marked `safeForUntrusted` is refused. It lives on the runtime's
/// owner, like the session, and outlives the session it is connected to.
class HostVocabulary {
public:
    /// @brief Registers a command.
    /// @param command The command. Refused (`std::invalid_argument`): an invalid name, a duplicate, an argument whose
    ///        name is empty or repeated or whose type does not parse, or no `run`.
    void add(HostCommand command) {
        checkName(command.name, "a host command");
        if (_commands.contains(command.name)) {
            throw std::invalid_argument{"host command `" + command.name + "` is registered twice"};
        }
        if (!command.run) {
            throw std::invalid_argument{"host command `" + command.name + "` has nothing to run"};
        }
        Registered registered;
        for (HostArg const& arg : command.args) {
            if (arg.name.empty() || registered.args.contains(arg.name)) {
                throw std::invalid_argument{"host command `" + command.name + "` has an empty or repeated argument"};
            }
            registered.args.emplace(arg.name, typeOf(arg.type, "argument `" + arg.name + "` of `" + command.name));
        }
        registered.command = std::move(command);
        std::string name = registered.command.name;
        _commands.emplace(std::move(name), std::move(registered));
        publish();
    }

    /// @brief Registers a value.
    /// @param value The value. Refused (`std::invalid_argument`): an invalid name, a duplicate, a type that does not
    ///        parse, or an initial value not of the type.
    void add(HostValue value) {
        checkName(value.name, "a host value");
        if (_valueTypes.contains(value.name)) {
            throw std::invalid_argument{"host value `" + value.name + "` is registered twice"};
        }
        Type type = typeOf(value.type, "host value `" + value.name);
        if (!type.admits(value.initial)) {
            throw std::invalid_argument{"the initial value of `" + value.name + "` is not a " + type.name()};
        }
        _valueTypes.emplace(value.name, std::move(type));
        _valueVersions.emplace(value.name, value.version);
        _values.insert_or_assign(std::move(value.name), std::move(value.initial));
        publish();
    }

    /// @brief Changes a value; every binding on it updates.
    /// @param name  The value.
    /// @param value Its new value.
    /// @return Whether the value is registered and @p value has its type; otherwise nothing changes.
    bool set(std::string const& name, Value value) {
        auto const type = _valueTypes.find(name);
        if (type == _valueTypes.end() || !type->second.admits(value)) {
            return false;
        }
        _values.insert_or_assign(name, std::move(value));
        if (_session != nullptr) {
            _session->setHostValues(_values);
        }
        return true;
    }

    /// @brief Says whether the documents came over a verified connection. A command not marked `safeForUntrusted`
    ///        runs, and `can` is true for it, only while this is `true`. It starts `false`.
    /// @param verified Whether the connection is verified.
    void setConnectionVerified(bool verified) {
        _verified = verified;
        publish();
    }

    /// @brief Gives @p session the values, the names `{"can": "host:name"}` answers for, and the runner for
    ///        `{"host": name}` commands. Later registrations and changes reach it too.
    /// @param session The session. Borrowed: this vocabulary outlives its use of it.
    void connect(Session& session) {
        _session = &session;
        publish();
    }

    /// @brief Fills the host command and value names and versions of a `ui-hello` request, so the server knows what
    ///        this client can do before it serves a document that needs it.
    /// @param hello The request.
    void describe(HelloRequest& hello) const {
        for (auto const& [name, registered] : _commands) {
            hello.hostCommands.insert_or_assign(name, registered.command.version);
        }
        for (auto const& [name, version] : _valueVersions) {
            hello.hostValues.insert_or_assign(name, version);
        }
    }

    /// @brief Runs a command as a document's `{"host": name, "args": args}` does.
    /// @param name The command.
    /// @param args Its arguments: a record, or `null` for none.
    /// @return `Ran`; `Missing` for a command nobody registered; `Refused`, with the reason, for an argument that is
    ///         missing, of the wrong type or not declared, or for an unsafe command on an unverified connection.
    [[nodiscard]] HostCommandResult run(std::string const& name, Value const& args) const {
        auto const found = _commands.find(name);
        if (found == _commands.end()) {
            return HostCommandResult{.kind = HostCommandResult::Kind::Missing, .reason = {}};
        }
        Registered const& registered = found->second;
        if (!registered.command.safeForUntrusted && !_verified) {
            return refused("host command `" + name + "` needs a verified connection");
        }
        ValueRecord checked;
        ValueRecord const* given = args.asRecord();
        if (given == nullptr && !args.isNull()) {
            return refused("the arguments of host command `" + name + "` are a record");
        }
        if (given != nullptr) {
            for (auto const& [argName, value] : *given) {
                static_cast<void>(value);
                if (!registered.args.contains(argName)) {
                    std::string reason = "host command `" + name;
                    reason += "` has no argument `";
                    reason += argName;
                    reason += '`';
                    return refused(std::move(reason));
                }
            }
        }
        for (auto const& [argName, type] : registered.args) {
            Value value;
            if (given != nullptr) {
                if (auto const supplied = given->find(argName); supplied != given->end()) {
                    value = supplied->second;
                }
            }
            if (!type.admits(value)) {
                std::string reason = "argument `" + argName;
                reason += "` of host command `";
                reason += name;
                reason += "` is not a ";
                reason += type.name();
                return refused(std::move(reason));
            }
            checked.insert_or_assign(argName, std::move(value));
        }
        registered.command.run(checked);
        return HostCommandResult{.kind = HostCommandResult::Kind::Ran, .reason = {}};
    }

private:
    struct Registered {
        HostCommand command;
        std::map<std::string, Type, std::less<>> args;
    };

    [[nodiscard]] static HostCommandResult refused(std::string reason) {
        return HostCommandResult{.kind = HostCommandResult::Kind::Refused, .reason = std::move(reason)};
    }

    static void checkName(std::string const& name, std::string_view what) {
        if (name.empty()) {
            throw std::invalid_argument{std::string{what} + " has a name"};
        }
        if (auto reason = detail::checkName(name, what)) {
            throw std::invalid_argument{*reason};
        }
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the type's text, then what it types, as the reason reads.
    [[nodiscard]] static Type typeOf(std::string const& text, std::string const& what) {
        auto type = parseType(text);
        if (!type) {
            throw std::invalid_argument{"the type of " + what + "` does not parse: " + type.error()};
        }
        return std::move(*type);
    }

    /// Hands the session the values and the commands runnable now.
    void publish() {
        if (_session == nullptr) {
            return;
        }
        std::set<std::string, std::less<>> available;
        for (auto const& [name, registered] : _commands) {
            if (registered.command.safeForUntrusted || _verified) {
                available.insert(name);
            }
        }
        _session->runtime().batch([&] {
            _session->setHostValues(_values);
            _session->setHostCommands(
                HostCommands{.available = std::move(available),
                             .run = [this](std::string const& name, Value const& args) { return run(name, args); }});
        });
    }

    std::map<std::string, Registered, std::less<>> _commands;
    std::map<std::string, Type, std::less<>> _valueTypes;
    std::map<std::string, std::int64_t, std::less<>> _valueVersions;
    ValueRecord _values;
    Session* _session = nullptr;
    bool _verified = false;
};

}  // namespace morph::ui
