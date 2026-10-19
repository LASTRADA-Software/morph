// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
/// that session. Fan-out re-authorises the watch on them at send time, by the
/// rule that admitted it.
struct Watch {
    /// @brief The `watch` envelope's session, its principal already the verified one.
    ::morph::session::Context context;
    /// @brief The principal the server verified for that envelope; empty when none was.
    std::string principal;
};

/// @brief What an admission answer in a `WatchTable` is about.
enum class AdmissionTopic : std::uint8_t {
    /// @brief A record type, by wire name.
    record,
    /// @brief The instances of a model type, by type name.
    modelType,
};

/// @brief Each connection's watched record types and keyed instances. On the
///        server strand: not synchronised.
///
/// A record topic is stored by record name. An instance topic is stored by
/// `(modelType, primary)` and never resolved to an instance id, because the
/// watch is sent before the fetch that may create the instance. Watching a
/// topic the connection already watches keeps the newer `Watch`. One
/// connection watches at most `wire::kMaxWatchTopics` topics, records and
/// instances together, which bounds what a peer can make the server hold.
///
/// The table also caches admission answers per verified principal and token:
/// two tokens of one principal may carry different claims, so they are judged
/// apart. Each `(principal, token)` counts the topics watched with it, and its
/// answers are forgotten when that count returns to zero, so a session that
/// watches again is judged afresh.
class WatchTable {
public:
    /// @brief Adds @p body's topics to @p cid's watch set, or, when
    ///        `body.replace` is set, makes the set exactly @p body's topics.
    ///
    /// A topic not watched yet is refused while the connection already
    /// watches `wire::kMaxWatchTopics` topics.
    /// @param cid   Connection the topics are watched on.
    /// @param body  The admitted topics, without duplicates; `instance` ids are ignored.
    /// @param watch What the topics were admitted on, shared by all of them.
    /// @return The topics refused because the connection was at its cap.
    ::morph::wire::WatchReply add(ConnectionId cid, ::morph::wire::WatchBody const& body, Watch watch) {
        if (body.replace) {
            dropConnection(cid);
        }
        auto const shared = std::make_shared<Watch const>(std::move(watch));
        SessionKey const key = keyOf(*shared);
        ::morph::wire::WatchReply overCap;
        Connection& connection = _connections[cid];
        for (auto const& record : body.records) {
            if (!place(connection, connection.records, record, shared, key)) {
                overCap.refusedRecords.push_back(record);
            }
        }
        for (auto const& topic : body.instances) {
            if (!place(connection, connection.instances, InstanceKey{topic.modelType, topic.primary}, shared, key)) {
                overCap.refusedInstances.push_back(topic);
            }
        }
        if (connection.size() == 0) {
            _connections.erase(cid);
        }
        forgetIfIdle(key);
        return overCap;
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
            erase(found->second.records, record);
        }
        for (auto const& topic : body.instances) {
            erase(found->second.instances, InstanceKey{topic.modelType, topic.primary});
        }
        if (found->second.size() == 0) {
            _connections.erase(found);
        }
    }

    /// @brief Drops every watch @p cid holds.
    /// @param cid Connection that closed, failed a write, or changed principal.
    void dropConnection(ConnectionId cid) {
        auto found = _connections.find(cid);
        if (found == _connections.end()) {
            return;
        }
        for (auto const& [record, watch] : found->second.records) {
            release(keyOf(*watch));
        }
        for (auto const& [topic, watch] : found->second.instances) {
            release(keyOf(*watch));
        }
        _connections.erase(found);
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
        return found == connection->second.records.end() ? nullptr : found->second.get();
    }

    /// @brief What @p cid's watch on the keyed instance `(modelType, primary)`
    ///        was admitted on.
    /// @param cid       Connection.
    /// @param modelType The instance's registered model type.
    /// @param primary   The instance's directory key text.
    /// @return The watch, valid until the table next changes, or null when
    ///         @p cid does not watch that instance.
    [[nodiscard]] Watch const* watchOf(ConnectionId cid, std::string_view modelType, std::string_view primary) const {
        auto const connection = _connections.find(cid);
        if (connection == _connections.end()) {
            return nullptr;
        }
        auto const found =
            connection->second.instances.find(InstanceKey{std::string{modelType}, std::string{primary}});
        return found == connection->second.instances.end() ? nullptr : found->second.get();
    }

    /// @brief Drops every watch @p cid holds that was admitted with
    ///        @p principal and @p token, and keeps the rest.
    ///
    /// What a watch whose token no longer authenticates takes with it: every
    /// topic the connection watched under that same session.
    /// @param cid       Connection.
    /// @param principal The verified principal the watches were admitted for.
    /// @param token     The token they were admitted with.
    void dropSession(ConnectionId cid, std::string const& principal, std::string const& token) {
        auto found = _connections.find(cid);
        if (found == _connections.end()) {
            return;
        }
        SessionKey const key{principal, token};
        auto const dropMatching = [this, &key](auto& topics) {
            for (auto it = topics.begin(); it != topics.end();) {
                if (keyOf(*it->second) == key) {
                    release(key);
                    it = topics.erase(it);
                } else {
                    ++it;
                }
            }
        };
        dropMatching(found->second.records);
        dropMatching(found->second.instances);
        if (found->second.size() == 0) {
            _connections.erase(found);
        }
    }

    /// @brief How many topics @p cid watches, records and instances together.
    /// @param cid Connection.
    /// @return The count; 0 for a connection with no watches.
    [[nodiscard]] std::size_t topicCount(ConnectionId cid) const {
        auto const found = _connections.find(cid);
        return found == _connections.end() ? 0 : found->second.size();
    }

    /// @brief The cached answer to whether @p session may watch @p name.
    /// @param session A session whose principal is the verified one.
    /// @param kind    What @p name names.
    /// @param name    A record's wire name or a model type name.
    /// @return The answer, or `nullopt` when none is cached.
    [[nodiscard]] std::optional<bool> admission(::morph::session::Context const& session, AdmissionTopic kind,
                                                std::string_view name) const {
        auto const bySession = _sessions.find(SessionKey{session.principal, session.token});
        if (bySession == _sessions.end()) {
            return std::nullopt;
        }
        auto const& answers = bySession->second.answers.at(static_cast<std::size_t>(kind));
        auto const found = answers.find(name);
        if (found == answers.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// @brief Caches whether @p session may watch @p name.
    ///
    /// Kept while a topic watched with the same principal and token remains;
    /// forgotten at the end of the next `add` otherwise.
    /// @param session  A session whose principal is the verified one.
    /// @param kind     What @p name names.
    /// @param name     A registered record's wire name or model type name.
    /// @param admitted The answer.
    void noteAdmission(::morph::session::Context const& session, AdmissionTopic kind, std::string_view name,
                       bool admitted) {
        auto& answers = _sessions[SessionKey{session.principal, session.token}].answers;
        answers.at(static_cast<std::size_t>(kind)).insert_or_assign(std::string{name}, admitted);
    }

private:
    using InstanceKey = std::pair<std::string, std::string>;
    // (verified principal, token): what an admission answer was decided on.
    using SessionKey = std::pair<std::string, std::string>;
    using WatchPtr = std::shared_ptr<Watch const>;
    using Answers = std::map<std::string, bool, std::less<>>;

    /// One connection's watches, by topic.
    struct Connection {
        std::map<std::string, WatchPtr, std::less<>> records;
        std::map<InstanceKey, WatchPtr> instances;

        [[nodiscard]] std::size_t size() const { return records.size() + instances.size(); }
    };

    /// How many watched topics carry one session, and its cached answers by `AdmissionTopic`.
    struct Session {
        std::size_t topics = 0;
        std::array<Answers, 2> answers;
    };

    [[nodiscard]] static SessionKey keyOf(Watch const& watch) { return {watch.principal, watch.context.token}; }

    /// Stores @p watch for @p topic, unless that would take @p connection past the cap.
    template <class Topics, class Topic>
    bool place(Connection const& connection, Topics& topics, Topic const& topic, WatchPtr const& watch,
               SessionKey const& key) {
        auto const found = topics.find(topic);
        if (found != topics.end()) {
            // Counted before the old session is released, so a re-watch by the
            // same session never drops that session's answers.
            ++_sessions[key].topics;
            release(keyOf(*found->second));
            found->second = watch;
            return true;
        }
        if (connection.size() >= ::morph::wire::kMaxWatchTopics) {
            return false;
        }
        topics.emplace(topic, watch);
        ++_sessions[key].topics;
        return true;
    }

    /// Removes @p topic, if watched, and releases its session.
    template <class Topics, class Topic>
    void erase(Topics& topics, Topic const& topic) {
        auto const found = topics.find(topic);
        if (found == topics.end()) {
            return;
        }
        release(keyOf(*found->second));
        topics.erase(found);
    }

    /// One topic watched with @p key is gone; the session goes with its last.
    void release(SessionKey const& key) {
        auto const found = _sessions.find(key);
        if (found != _sessions.end() && --found->second.topics == 0) {
            _sessions.erase(found);
        }
    }

    /// Forgets @p key's answers when no watched topic carries it.
    void forgetIfIdle(SessionKey const& key) {
        auto const found = _sessions.find(key);
        if (found != _sessions.end() && found->second.topics == 0) {
            _sessions.erase(found);
        }
    }

    // Ordered by connection id, so `watchersOf` answers in ascending order.
    std::map<ConnectionId, Connection> _connections;
    std::map<SessionKey, Session> _sessions;
};

}  // namespace morph::backend::detail
