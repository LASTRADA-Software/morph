// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../session/session.hpp"
#include "../connection_writer.hpp"
#include "../wire.hpp"

/// @file
/// @brief The topics each connection watches, as `RemoteServer` keeps them.
///
/// Holds only what the server decided: admission, which needs the record and
/// action registries and the authorizer, happens in the server, which hands
/// this table the admitted topics.

namespace morph::backend::detail {

/// @brief What one watched topic was admitted on.
///
/// A connection has no principal of its own, so each watch keeps the session
/// of the `watch` envelope that created it and the principal verified from
/// that session. Fan-out re-authorises the watch on them at send time.
struct Watch {
    /// @brief The `watch` envelope's session, its principal already the verified one.
    ::morph::session::Context context;
    /// @brief The principal the server verified for that envelope; empty when none was.
    std::string principal;
};

/// @brief Each connection's watched record types and keyed instances. On the
///        server strand: not synchronised.
///
/// A record topic is stored by record name. An instance topic is stored by
/// `(modelType, primary)` and never resolved to an instance id, because the
/// watch is sent before the fetch that may create the instance. Watching a
/// topic the connection already watches keeps the newer `Watch`.
///
/// The table also caches, per principal and record, whether the principal may
/// watch the record, so a connection's repeated watches do not re-run the
/// admission rule. A principal's answers are forgotten once none of its
/// watches is left, so a principal that watches again is judged afresh.
class WatchTable {
public:
    /// @brief Adds @p body's topics to @p cid's watch set, or, when
    ///        `body.replace` is set, makes the set exactly @p body's topics.
    /// @param cid   Connection the topics are watched on.
    /// @param body  The admitted topics; `instance` ids are ignored.
    /// @param watch What the topics were admitted on.
    void add(ConnectionId cid, ::morph::wire::WatchBody const& body, Watch watch) {
        if (body.replace) {
            _connections.erase(cid);
        }
        if (body.records.empty() && body.instances.empty()) {
            forgetIdleAdmissions();
            return;
        }
        Connection& connection = _connections[cid];
        for (auto const& record : body.records) {
            connection.records.insert_or_assign(record, watch);
        }
        for (auto const& topic : body.instances) {
            connection.instances.insert_or_assign(InstanceKey{topic.modelType, topic.primary}, watch);
        }
        forgetIdleAdmissions();
    }

    /// @brief Removes @p body's topics from @p cid's watch set. A topic not
    ///        watched there is ignored.
    /// @param cid  Connection the topics were watched on.
    /// @param body The topics; `replace` and `instance` ids are ignored.
    void remove(ConnectionId cid, ::morph::wire::WatchBody const& body) {
        auto found = _connections.find(cid);
        if (found == _connections.end()) {
            return;
        }
        for (auto const& record : body.records) {
            found->second.records.erase(record);
        }
        for (auto const& topic : body.instances) {
            found->second.instances.erase(InstanceKey{topic.modelType, topic.primary});
        }
        if (found->second.records.empty() && found->second.instances.empty()) {
            _connections.erase(found);
        }
        forgetIdleAdmissions();
    }

    /// @brief Drops every watch @p cid holds.
    /// @param cid Connection that closed, failed a write, or changed principal.
    void dropConnection(ConnectionId cid) {
        if (_connections.erase(cid) != 0) {
            forgetIdleAdmissions();
        }
    }

    /// @brief The connections watching record type @p record.
    /// @param record A record's wire name.
    /// @return The connection ids, ascending.
    [[nodiscard]] std::vector<ConnectionId> watchersOf(std::string_view record) const {
        std::vector<ConnectionId> watchers;
        for (auto const& [cid, connection] : _connections) {
            if (connection.records.contains(record)) {
                watchers.push_back(cid);
            }
        }
        return watchers;
    }

    /// @brief The connections watching the keyed instance `(modelType, primary)`.
    /// @param modelType The instance's registered model type.
    /// @param primary   The instance's directory key text.
    /// @return The connection ids, ascending.
    [[nodiscard]] std::vector<ConnectionId> watchersOf(std::string_view modelType, std::string_view primary) const {
        InstanceKey const key{std::string{modelType}, std::string{primary}};
        std::vector<ConnectionId> watchers;
        for (auto const& [cid, connection] : _connections) {
            if (connection.instances.contains(key)) {
                watchers.push_back(cid);
            }
        }
        return watchers;
    }

    /// @brief What @p cid's watch on record type @p record was admitted on.
    /// @param cid    Connection.
    /// @param record A record's wire name.
    /// @return The watch, valid until the table next changes, or null when
    ///         @p cid does not watch @p record.
    [[nodiscard]] Watch const* watchOf(ConnectionId cid, std::string_view record) const {
        auto const connection = _connections.find(cid);
        if (connection == _connections.end()) {
            return nullptr;
        }
        auto const found = connection->second.records.find(record);
        return found == connection->second.records.end() ? nullptr : &found->second;
    }

    /// @brief The cached answer to whether @p principal may watch @p record.
    /// @param principal A verified principal; empty for none.
    /// @param record    A record's wire name.
    /// @return The answer, or `nullopt` when none is cached.
    [[nodiscard]] std::optional<bool> admission(std::string_view principal, std::string_view record) const {
        auto const byPrincipal = _admissions.find(principal);
        if (byPrincipal == _admissions.end()) {
            return std::nullopt;
        }
        auto const found = byPrincipal->second.find(record);
        if (found == byPrincipal->second.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// @brief Caches whether @p principal may watch @p record.
    /// @param principal A verified principal; empty for none.
    /// @param record    A registered record's wire name.
    /// @param admitted  The answer.
    void noteAdmission(std::string_view principal, std::string_view record, bool admitted) {
        auto byPrincipal = _admissions.find(principal);
        if (byPrincipal == _admissions.end()) {
            byPrincipal = _admissions.emplace(std::string{principal}, Answers{}).first;
        }
        byPrincipal->second.insert_or_assign(std::string{record}, admitted);
    }

private:
    using InstanceKey = std::pair<std::string, std::string>;
    using Answers = std::map<std::string, bool, std::less<>>;

    /// One connection's watches, by topic.
    struct Connection {
        std::map<std::string, Watch, std::less<>> records;
        std::map<InstanceKey, Watch> instances;
    };

    /// Forgets the cached answers of every principal no watch carries.
    void forgetIdleAdmissions() {
        std::erase_if(_admissions, [this](auto const& entry) {
            for (auto const& [cid, connection] : _connections) {
                auto const carries = [&](auto const& topic) { return topic.second.principal == entry.first; };
                if (std::ranges::any_of(connection.records, carries) ||
                    std::ranges::any_of(connection.instances, carries)) {
                    return false;
                }
            }
            return true;
        });
    }

    // Ordered by connection id, so `watchersOf` answers in ascending order.
    std::map<ConnectionId, Connection> _connections;
    std::map<std::string, Answers, std::less<>> _admissions;
};

}  // namespace morph::backend::detail
