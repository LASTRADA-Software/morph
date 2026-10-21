// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../ui/backend.hpp"
#include "../ui/frontend.hpp"
#include "../ui/host.hpp"
#include "../ui/interpreter.hpp"
#include "../ui/route_history.hpp"
#include "../ui/value.hpp"
#include "source.hpp"

/// @file
/// @brief What the generic client mounts once its source is open: the library's `ShellFrame` (the sidebar with the
///        navigator, the top bar, the page) with a `Workspace` of tabs in its page, each a screen mounted through the
///        library's `DocumentScreenHost`; and the application host a shell of an application's own mounts with.
///
/// Specified in `docs/spec/client/client.md`.

namespace morph::client {

/// @brief What a sign-in installs: the principal, and the session token its server needs (empty for none).
struct SignedIn {
    /// @brief The principal.
    std::string principal;
    /// @brief The session token; empty when the server needs none, as in process.
    std::string token;
};

/// @brief Maps a sign-in mutation's reply to the principal and token to install; `std::nullopt` refuses the sign-in.
using SignInMapper = std::function<std::optional<SignedIn>(ui::Value const& reply)>;

/// @brief The default sign-in mapping: a reply `{ok, principal, token}`, refused when `ok` is `false` or there is no
///        principal; `token` may be absent.
/// @param reply The sign-in mutation's reply.
/// @return The principal and token, or `std::nullopt`.
[[nodiscard]] std::optional<SignedIn> defaultSignIn(ui::Value const& reply);

/// @brief The application's side of the commands a document host does not answer itself (spec 6): sign-in and
///        sign-out, which install a principal into the source, and the file commands, which read a picked file from
///        disk and save a downloaded one, moving the bytes over the source's file side channel or inline.
///
/// A shell of an application's own builds one and passes it as `DocumentScreenHostOptions::application`;
/// `mountShell` builds its own.
class ApplicationHost final : public ui::InterpreterHost {
public:
    /// @brief A host over @p source.
    /// @param source    The source sign-ins are installed into and files move through. Borrowed: it outlives the
    ///                  host.
    /// @param signIn    Maps a sign-in reply to what is installed. Required.
    /// @param downloads Where a `download` saves a file; the working directory when empty.
    /// @param signedIn  Called with what a sign-in installs, for the shell to install it and reload.
    /// @param signedOut Called on a `signOut`.
    /// @param onProblem Called with a refused sign-in's reason; may be empty.
    /// @throws std::invalid_argument when @p signIn is empty.
    ApplicationHost(Source& source, SignInMapper signIn, std::filesystem::path downloads,
                    std::function<void(SignedIn)> signedIn, std::function<void()> signedOut,
                    std::function<void(std::string const&)> onProblem = {});

    /// @brief Maps the reply and hands a principal to `signedIn`.
    /// @param reply The sign-in mutation's reply.
    /// @return Whether it signed in: the mapper answered a principal.
    bool signIn(ui::Value const& reply) override;

    /// @brief Calls `signedOut`.
    void signOut() override;

    /// @brief Reads the picked file from disk and sends it through the source's file store.
    /// @param request  The file and the mutation it is for.
    /// @param progress Called with the progress, as an exact fraction.
    /// @param done     Called with the file's reference, or the failure.
    /// @return Whether it can: the source has a file side channel and the file is a path.
    bool uploadFor(ui::UploadRequest const& request, std::function<void(ui::Value)> progress,
                   std::function<void(std::expected<ui::Value, std::string>)> done) override;

    /// @brief Fetches a file by reference and saves it in the downloads directory. A preview is not supported.
    /// @param file   The reference.
    /// @param option `dialog`; `preview` answers not supported.
    /// @param name   The suggested name; `null` keeps the file's own.
    /// @param done   Called with the saved path, or the failure.
    /// @return Whether it can.
    bool download(ui::Value const& file, std::string const& option, ui::Value const& name,
                  std::function<void(std::expected<ui::Value, std::string>)> done) override;

    /// @brief Reads a picked file whose bytes a document moves inline; its size is read first, and one over the cap
    ///        is refused without reading it.
    /// @param file     The picked path.
    /// @param maxBytes The session's inline cap.
    /// @param done     Called with the file, or the failure.
    /// @return Whether it can: the file is a path.
    bool readFile(ui::Value const& file, std::size_t maxBytes,
                  std::function<void(std::expected<ui::InlineFile, std::string>)> done) override;

    /// @brief Saves a file a document holds the bytes of in the downloads directory. A preview is not supported.
    /// @param file   The file.
    /// @param option `dialog`; `preview` answers not supported.
    /// @param done   Called with the saved path, or the failure.
    /// @return Whether it can.
    bool saveFile(ui::InlineFile const& file, std::string const& option,
                  std::function<void(std::expected<ui::Value, std::string>)> done) override;

    /// @brief Sets what `notify` calls to show a transient message; empty falls through to the base class's `false`.
    /// @param sink Given the text and the role; returns whether it showed it.
    void setNotifySink(std::function<bool(std::string const&, std::string const&)> sink);

    /// @brief Calls the notify sink if one is set.
    /// @param text The message.
    /// @param role `info`, `success`, `warning` or `error`; empty for the default.
    /// @return Whether the sink showed it; `false` when none is set.
    bool notify(std::string const& text, std::string const& role) override;

private:
    Source* _source;
    SignInMapper _signIn;
    std::filesystem::path _downloads;
    std::function<void(SignedIn)> _signedIn;
    std::function<void()> _signedOut;
    std::function<void(std::string const&)> _onProblem;
    std::function<bool(std::string const&, std::string const&)> _notifySink;
};

/// @brief What a client shell reports, and how it starts.
struct ShellOptions {
    /// @brief The screen mounted first; empty for the first the source lists.
    std::string startScreen;
    /// @brief The start screen's params.
    ui::ParamMap startParams;
    /// @brief Called with each problem: why the source did not open, a document that did not load, a refused mount,
    ///        an interpreter diagnostic. Empty ignores them; the shell shows the first three either way.
    std::function<void(std::string const& problem)> onProblem;
    /// @brief Called with the id of each screen once it is mounted.
    std::function<void(std::string const& screenId)> onMounted;
    /// @brief Where a screen's `download` saves a file, under the name the screen suggests; the working directory
    ///        when empty.
    std::filesystem::path downloads;
    /// @brief The history the shown screen's route is kept in (spec 3 §10), such as the browser's: back and forward
    ///        open the route of the entry moved to, and the route it holds at start is opened once the session has
    ///        the screen. Borrowed: it outlives the shell; null keeps no history.
    ui::IHistory* history = nullptr;
    /// @brief Development mode (spec 5 §12): every second the shell polls the source's `ui-hello` digest, and when it
    ///        changed, remounts every open tab whose document changed, keeping its state. A shell built without
    ///        the development tools never polls. A production client leaves it off: the digest is then read once.
    bool dev = false;
    /// @brief Called after each development reload with the ids of the screens it remounted; empty ignores it.
    std::function<void(std::vector<std::string> const& remounted)> onReloaded;
    /// @brief Maps a sign-in mutation's reply to the principal and token the shell installs. Required.
    SignInMapper signIn = defaultSignIn;
};

/// @brief Mounts the client's shell for what @p source delivered.
///
/// On a bundle: loads each screen of `source.screens()` with the source's catalog facts, opens the start screen as a
/// tab, and on a document's `navigate` or a navigator choice opens the screen asked for as a tab (or focuses the tab
/// that shows it), from a task posted to the owner, never from inside the handler that asked. A sign-in or sign-out
/// closes every tab and rebuilds the frame for the new catalog. On a `ConnectError`, or when no screen can be mounted,
/// shows why as text.
/// @param context The frontend's context.
/// @param backend The view backend the screens mount on. Borrowed: it outlives the shell.
/// @param opened  What the source delivered.
/// @param source  The source; it outlives the shell.
/// @param options The start screen and the observers.
/// @return The shell; destroying it unmounts everything.
/// @throws std::invalid_argument when `options.signIn` is empty.
[[nodiscard]] std::unique_ptr<ui::MountedShell> mountShell(ui::AppContext& context, ui::IViewBackend& backend,
                                                           std::expected<ui::Bundle, ui::ConnectError> opened,
                                                           Source& source, ShellOptions options);

}  // namespace morph::client
