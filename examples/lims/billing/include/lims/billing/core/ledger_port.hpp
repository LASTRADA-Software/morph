// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <functional>
#include <morph/core/completion.hpp>

#include "lims/posting/posting.hpp"

/// @file
/// What billing needs of the ledger, without including it: posting a request, and the dated rate. The app installs
/// the port at start-up, before the first dispatch; billing-internal, since it names the ORM.

namespace lims {

/// @brief How a posting reaches the ledger.
enum class PostingMode : std::uint8_t {
    Dispatched,         ///< As the ledger's own action, dispatched by the app; it commits alone, before billing.
    SharedTransaction,  ///< Through billing's open mapper, in billing's transaction; both halves commit once.
};

/// @brief The ledger as billing reaches it.
struct LedgerPort {
    /// @brief Which of `post` and `postIn` `PostInvoice` uses.
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

/// @brief Installs @p port. Called once at start-up (and by tests), before any billing action runs.
/// @param port The port; an empty one uninstalls.
void setLedgerPort(LedgerPort port);

/// @brief The installed port; its members empty until one is installed.
/// @return It.
[[nodiscard]] LedgerPort const& ledgerPort();

}  // namespace lims
