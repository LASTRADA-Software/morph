// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QProcess>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace morph::ladder::testkit {

/// @brief One spawned client process.
///
/// Thin, test-shaped wrapper over `QProcess`, modelled on the pattern
/// `tests/qt/test_qt_websocket.cpp` already uses to drive `qt_test_client`.
/// The value it adds over raw `QProcess` is `kill()` — an *uncatchable*
/// termination, which is the only way to produce the one condition no
/// in-process test can reach: a client that stops existing without unwinding.
/// An in-process "crash" still runs destructors, deregisters handlers, and
/// closes its backend; a `SIGKILL`ed process does none of that, so the server
/// observes exactly what it would in production when a client segfaults or
/// its machine loses power.
class ClientProcess {
public:
    /// @param program Absolute path to the client binary.
    /// @param args    Command-line arguments for this client.
    ClientProcess(QString program, QStringList args) : _proc{std::make_unique<QProcess>()} {
        _proc->setProgram(std::move(program));
        _proc->setArguments(std::move(args));
        // Separate channels: the client's stderr carries the step name it
        // failed at, which is what makes a nonzero exit code diagnosable.
        _proc->setProcessChannelMode(QProcess::SeparateChannels);
    }

    ClientProcess(const ClientProcess&) = delete;
    ClientProcess& operator=(const ClientProcess&) = delete;
    ClientProcess(ClientProcess&&) noexcept = default;
    ClientProcess& operator=(ClientProcess&&) noexcept = default;

    /// @brief Kills the process if it is still running.
    ///
    /// A test that fails a `REQUIRE` mid-scenario unwinds without reaching any
    /// explicit cleanup, so this must not be left to the caller: a leaked
    /// client would hold its connection open and make every later assertion
    /// about `liveModels` wrong.
    ~ClientProcess() {
        if (_proc && _proc->state() != QProcess::NotRunning) {
            _proc->kill();
            _proc->waitForFinished(2000);
        }
    }

    /// @brief Starts the process and waits for it to be running.
    /// @param timeoutMs How long to wait for the spawn itself.
    /// @return `true` if the process started within @p timeoutMs.
    [[nodiscard]] bool start(int timeoutMs = 5000) {
        _proc->start();
        return _proc->waitForStarted(timeoutMs);
    }

    /// @brief Waits for the process to exit on its own.
    /// @param timeoutMs How long to wait.
    /// @return `true` if it exited within @p timeoutMs.
    [[nodiscard]] bool waitForFinished(int timeoutMs = 15000) { return _proc->waitForFinished(timeoutMs); }

    /// @brief Terminates the process without giving it a chance to clean up.
    ///
    /// This is the point of the whole harness: the client's `Bridge`,
    /// `BridgeHandler` and backend destructors never run, so nothing sends a
    /// `deregister` and nothing closes the socket politely. The server learns
    /// the client is gone only from the transport.
    void kill() {
        _proc->kill();
        _proc->waitForFinished(2000);
    }

    /// @brief Whether the process is still running.
    /// @return `true` while it has not exited.
    [[nodiscard]] bool running() const { return _proc->state() != QProcess::NotRunning; }

    /// @brief The process's exit code. Only meaningful once it has exited.
    /// @return The exit code.
    [[nodiscard]] int exitCode() const { return _proc->exitCode(); }

    /// @brief Whether the process exited abnormally (killed, or crashed).
    /// @return `true` on abnormal exit.
    [[nodiscard]] bool crashed() const { return _proc->exitStatus() != QProcess::NormalExit; }

    /// @brief Everything the client wrote to stderr, for failure diagnosis.
    /// @return The captured stderr text.
    [[nodiscard]] std::string stderrText() const { return _proc->readAllStandardError().toStdString(); }

    /// @brief The underlying process, for cases this wrapper does not cover.
    /// @return A reference to the `QProcess`.
    [[nodiscard]] QProcess& process() { return *_proc; }

private:
    std::unique_ptr<QProcess> _proc;
};

/// @brief A pool of client processes all running the same binary.
///
/// Exists because `QtExecutor` posts to `QCoreApplication::instance()` and
/// nothing else, so "N clients" inside one test process is always N clients
/// sharing one event loop on one pumped thread. That is enough to exercise
/// interleaving, but it cannot exercise *separation*: no in-process client has
/// its own address space to lose. This pool gives each client a real one.
///
/// Destroying the pool kills anything still running, so a failed assertion
/// cannot leak clients into the next test.
class ProcessPool {
public:
    /// @param program Absolute path to the client binary every `spawn()` runs.
    explicit ProcessPool(QString program) : _program{std::move(program)} {}

    ProcessPool(const ProcessPool&) = delete;
    ProcessPool& operator=(const ProcessPool&) = delete;

    /// @brief Spawns one client.
    /// @param args      Command-line arguments for this client.
    /// @param timeoutMs How long to wait for the spawn.
    /// @return A reference to the spawned client, or `nullptr` if it failed to start.
    [[nodiscard]] ClientProcess* spawn(const QStringList& args, int timeoutMs = 5000) {
        auto client = std::make_unique<ClientProcess>(_program, args);
        if (!client->start(timeoutMs)) {
            return nullptr;
        }
        _clients.push_back(std::move(client));
        return _clients.back().get();
    }

    /// @brief Whether every spawned client has exited.
    ///
    /// A *predicate*, deliberately, rather than a blocking `waitForAll()`.
    /// When the server under test lives in the test process -- which is the
    /// normal arrangement, since it lets assertions read
    /// `RemoteServer::health()` directly instead of over IPC -- blocking in
    /// `QProcess::waitForFinished` stops the test's event loop, so the server
    /// never services the very clients being waited on and both sides hang
    /// until the timeout. Feed this to `testkit::pumpUntil` instead, which
    /// keeps the loop running:
    ///
    /// ```cpp
    /// REQUIRE(pumpUntil([&] { return pool.allExited(); }, std::chrono::seconds{20}));
    /// ```
    /// @return `true` once no spawned client is still running.
    [[nodiscard]] bool allExited() const {
        for (const auto& client : _clients) {
            if (client->running()) {
                return false;
            }
        }
        return true;
    }

    /// @brief Kills every client that is still running.
    void killAll() {
        for (auto& client : _clients) {
            if (client->running()) {
                client->kill();
            }
        }
    }

    /// @brief Number of clients spawned so far.
    /// @return The client count.
    [[nodiscard]] std::size_t size() const noexcept { return _clients.size(); }

    /// @brief Accesses one spawned client.
    /// @param idx Index into the spawn order.
    /// @return A reference to that client.
    [[nodiscard]] ClientProcess& operator[](std::size_t idx) { return *_clients[idx]; }

private:
    QString _program;
    std::vector<std::unique_ptr<ClientProcess>> _clients;
};

}  // namespace morph::ladder::testkit
