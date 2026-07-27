// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(__APPLE__)
#include <signal.h>
#endif

#ifndef MSG_NOSIGNAL
// macOS has no MSG_NOSIGNAL; SO_NOSIGPIPE (set per-socket below) is the
// portable equivalent there, so this becomes a harmless no-op flag.
#define MSG_NOSIGNAL 0
#endif

namespace morph::net::detail {

/// @brief RAII wrapper around a POSIX (BSD sockets) TCP file descriptor.
///
/// Linux/macOS only today — see `docs/spec/core/backend.md`'s `morph::net`
/// section for the Windows/Winsock2 follow-up. Move-only.
class TcpSocket {
public:
    /// @brief Constructs an empty (invalid) socket.
    TcpSocket() = default;

    /// @brief Wraps an already-open file descriptor (e.g. from `::accept`).
    /// @param fd Native socket file descriptor; takes ownership.
    explicit TcpSocket(int fd) : _fd{fd} { applyNoSigPipe(fd); }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// @brief Transfers ownership of @p other's file descriptor.
    /// @param other Socket to move from; left invalid.
    TcpSocket(TcpSocket&& other) noexcept : _fd{other._fd} { other._fd = -1; }

    /// @brief Transfers ownership of @p other's file descriptor, closing this one first.
    /// @param other Socket to move from; left invalid.
    /// @return `*this`.
    TcpSocket& operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            closeNow();
            _fd = other._fd;
            other._fd = -1;
        }
        return *this;
    }

    /// @brief Closes the socket if open.
    ~TcpSocket() { closeNow(); }

    /// @brief Connects to `host:port`, failing after @p timeout.
    /// @param host    Numeric or resolvable hostname.
    /// @param port    TCP port.
    /// @param timeout Maximum time to wait for the connection to establish.
    /// @return A connected `TcpSocket`.
    /// @throws std::runtime_error on resolution failure, connect failure, or timeout.
    static TcpSocket connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* resolved = nullptr;
        // Widened explicitly: std::to_string has no uint16_t overload, and
        // letting the integral promotion pick one trips GCC's -Wsign-promo
        // (it would choose the signed `int` overload over `unsigned`).
        std::string const portStr = std::to_string(static_cast<unsigned>(port));
        int const rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &resolved);
        if (rc != 0 || resolved == nullptr) {
            throw std::runtime_error("TcpSocket::connect: getaddrinfo failed for " + host + ": " + ::gai_strerror(rc));
        }
        struct AddrInfoGuard {
            addrinfo* p;
            ~AddrInfoGuard() { ::freeaddrinfo(p); }
        } guard{resolved};

        for (addrinfo* rp = resolved; rp != nullptr; rp = rp->ai_next) {
            int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) {
                continue;
            }
            int const flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int const connectRc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (connectRc == 0) {
                ::fcntl(fd, F_SETFL, flags);
                return TcpSocket{fd};
            }
            if (errno != EINPROGRESS) {
                ::close(fd);
                continue;
            }
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int const pollRc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
            if (pollRc <= 0) {
                ::close(fd);
                continue;
            }
            int soErr = 0;
            socklen_t soErrLen = sizeof(soErr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen);
            if (soErr != 0) {
                ::close(fd);
                continue;
            }
            ::fcntl(fd, F_SETFL, flags);
            return TcpSocket{fd};
        }
        throw std::runtime_error("TcpSocket::connect: could not connect to " + host + ":" + portStr);
    }

    /// @brief Creates a listening socket bound to `127.0.0.1:port`.
    /// @param port    TCP port to bind to; `0` lets the OS choose a free port.
    /// @param backlog Pending-connection backlog passed to `::listen`.
    /// @return A listening `TcpSocket`.
    /// @throws std::runtime_error on socket/bind/listen failure.
    static TcpSocket listen(std::uint16_t port, int backlog = 64) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(std::string{"TcpSocket::listen: socket() failed: "} + std::strerror(errno));
        }
        int const reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            int const err = errno;
            ::close(fd);
            throw std::runtime_error(std::string{"TcpSocket::listen: bind() failed: "} + std::strerror(err));
        }
        if (::listen(fd, backlog) != 0) {
            int const err = errno;
            ::close(fd);
            throw std::runtime_error(std::string{"TcpSocket::listen: listen() failed: "} + std::strerror(err));
        }
        return TcpSocket{fd};
    }

    /// @brief Returns the local port this socket is bound to, or `0` if invalid.
    /// @return The bound TCP port (useful after `listen(0, ...)`).
    [[nodiscard]] std::uint16_t boundPort() const {
        if (_fd < 0) {
            return 0;
        }
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        ::getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        return ntohs(addr.sin_port);
    }

    /// @brief Blocks until a client connects, returning the accepted connection.
    ///
    /// A concurrent `shutdownBoth()` from another thread unblocks a pending
    /// `accept()`, which then throws — the standard mechanism this reference
    /// implementation uses to interrupt a blocked accept loop during shutdown.
    /// @return The accepted `TcpSocket`.
    /// @throws std::runtime_error if `::accept` fails (including the shutdown case above).
    TcpSocket accept() {
        for (;;) {
            int const clientFd = ::accept(_fd, nullptr, nullptr);
            if (clientFd >= 0) {
                return TcpSocket{clientFd};
            }
            // EINTR is not a failure, just a signal delivered while blocked, and
            // `recvSome`/`sendAll` already retry it. Without the same retry
            // here, any signal the host happens to deliver (a profiler's timer,
            // SIGCHLD, SIGWINCH) tears down the accept loop and the server
            // silently stops taking connections. ECONNABORTED is deliberately
            // *not* retried: `shutdownBoth()` is the documented way to break
            // out of this call, and swallowing an abort risks spinning forever
            // on a listener that is being torn down.
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string{"TcpSocket::accept: "} + std::strerror(errno));
        }
    }

    /// @brief Reads up to @p len bytes into @p buf.
    /// @param buf Destination buffer.
    /// @param len Capacity of @p buf.
    /// @return Number of bytes read; `0` means the peer closed the connection
    ///         (or a concurrent `shutdownBoth()` unblocked this call).
    /// @throws std::runtime_error on a socket error other than an internally
    ///         retried `EINTR` or a peer reset (treated as an orderly close).
    std::size_t recvSome(char* buf, std::size_t len) {
        for (;;) {
            ssize_t const n = ::recv(_fd, buf, len, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ECONNRESET) {
                    return 0;
                }
                throw std::runtime_error(std::string{"TcpSocket::recvSome: "} + std::strerror(errno));
            }
            return static_cast<std::size_t>(n);
        }
    }

    /// @brief Writes all @p len bytes of @p data, looping over partial writes.
    /// @param data Bytes to send.
    /// @param len  Number of bytes in @p data.
    /// @throws std::runtime_error on a socket error.
    void sendAll(const char* data, std::size_t len) {
        std::size_t sent = 0;
        while (sent < len) {
            ssize_t const n = ::send(_fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string{"TcpSocket::sendAll: "} + std::strerror(errno));
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    /// @brief Shuts down both directions of the socket, unblocking a concurrent
    ///        `recvSome`/`accept` on another thread. Safe to call from any thread.
    void shutdownBoth() noexcept {
        if (_fd >= 0) {
            ::shutdown(_fd, SHUT_RDWR);
        }
    }

    /// @brief Returns the native file descriptor (`-1` if empty/moved-from).
    /// @return The native fd.
    [[nodiscard]] int nativeHandle() const noexcept { return _fd; }

    /// @brief Returns whether this socket owns an open file descriptor.
    /// @return `true` if `nativeHandle() >= 0`.
    [[nodiscard]] bool valid() const noexcept { return _fd >= 0; }

private:
    static void applyNoSigPipe(int fd) {
#if defined(SO_NOSIGPIPE)
        int const one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
        (void)fd;
#endif
    }

    void closeNow() noexcept {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    int _fd{-1};
};

}  // namespace morph::net::detail
