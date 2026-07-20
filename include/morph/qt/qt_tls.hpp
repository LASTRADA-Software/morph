// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>

namespace morph::qt {

/// @brief Builds a TLS client configuration that verifies the server against
/// the system/CA trust store.
///
/// This is the recommended production default: pass the result as the `tls`
/// argument to `QtWebSocketBackend`'s constructor to connect over `wss://`
/// with full peer authentication. Prefer `tlsPinnedConfig()` instead for a
/// self-signed deployment.
///
/// @return A `QSslConfiguration` derived from `QSslConfiguration::defaultConfiguration()`
///         with `peerVerifyMode` set to `QSslSocket::VerifyPeer`.
[[nodiscard]] inline QSslConfiguration tlsVerifyingConfig() {
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
    return cfg;
}

/// @brief Builds a TLS client configuration that verifies the server against
/// one pinned certificate instead of the system CA store.
///
/// The correct choice for a self-signed deployment: rather than disabling
/// verification (`tlsInsecureNoVerify()`), this keeps `QSslSocket::VerifyPeer`
/// enabled and trusts exactly @p pinned, so a self-signed server is
/// authenticated rather than merely encrypted-to. A peer presenting any other
/// certificate — including a MITM's own self-signed certificate — fails the
/// TLS handshake.
///
/// @param pinned The exact certificate the server is expected to present.
/// @return A `QSslConfiguration` with `peerVerifyMode` `QSslSocket::VerifyPeer`
///         and @p pinned as the sole trusted CA certificate.
[[nodiscard]] inline QSslConfiguration tlsPinnedConfig(const QSslCertificate& pinned) {
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
    cfg.setCaCertificates({pinned});
    return cfg;
}

/// @brief Builds a TLS client configuration that encrypts but does not
/// authenticate the server.
///
/// Kept for local development and tests only — the name states the hazard so
/// it can be grepped for in a security review. A client built with this
/// configuration completes a TLS handshake with *any* server presenting *any*
/// certificate, which is MITM-vulnerable. Prefer `tlsPinnedConfig()` for
/// self-signed deployments and `tlsVerifyingConfig()` for CA-issued
/// certificates.
///
/// @return A `QSslConfiguration` with `peerVerifyMode` set to `QSslSocket::VerifyNone`.
[[nodiscard]] inline QSslConfiguration tlsInsecureNoVerify() {
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    return cfg;
}

}  // namespace morph::qt
