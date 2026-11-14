// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <functional>
#include <morph/core/completion.hpp>
#include <utility>

#include "lims/posting/posting.hpp"

/// @file
/// What a poster needs of the ledger, without including it: posting a request, and the dated rate. The app installs
/// the one port at start-up, before the first dispatch; every poster (billing, inventory) reaches the ledger through
/// it. The storage is inline because `posting` has no sources.

namespace lims {

/// @brief How a posting reaches the ledger.
enum class PostingMode : std::uint8_t {
    Dispatched,         ///< As the ledger's own action, dispatched by the app; it commits alone, before the poster.
    SharedTransaction,  ///< Through the poster's open mapper, in the poster's transaction; both halves commit once.
};

/// @brief The ledger as a poster reaches it.
struct LedgerPort {
    /// @brief Which of `post` and `postIn` a poster uses.
    PostingMode mode = PostingMode::Dispatched;
    /// @brief Records a posting as the ledger's action; settles with its entry, or the ledger's refusal.
    std::function<::morph::async::Completion<PostingResult>(PostingRequest)> post;
    /// @brief Records a posting through the caller's open mapper, in the caller's transaction.
    std::function<PostingResult(Lightweight::DataMapper&, PostingRequest const&)> postIn;
    /// @brief The entry already recorded under a source key; settles with an empty entry when there is none.
    std::function<::morph::async::Completion<PostingResult>(PostingQuery)> find;
    /// @brief The rate valid on a date; settles with an empty rate when there is none.
    std::function<::morph::async::Completion<RateAnswer>(RateQuery)> rateOn;
};

/// @brief The port the app installs, empty until it does.
/// @return The process's one port.
[[nodiscard]] inline LedgerPort& ledgerPortStorage() {
    static LedgerPort port;
    return port;
}

/// @brief Installs @p port: called once by the app at start-up and by a test, before any poster runs.
/// @param port The port; an empty one uninstalls.
inline void setLedgerPort(LedgerPort port) { ledgerPortStorage() = std::move(port); }

/// @brief The installed port.
/// @return It; its functions are empty when none is installed.
[[nodiscard]] inline LedgerPort const& ledgerPort() { return ledgerPortStorage(); }

}  // namespace lims
