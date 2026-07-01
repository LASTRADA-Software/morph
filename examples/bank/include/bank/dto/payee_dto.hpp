// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Payee (beneficiary) model.

namespace bank::dto {

/// @brief Loose IBAN sanity check: 2 letters, 2 digits, then alphanumerics,
///        total length 15..34. Not a full mod-97 checksum (kept simple).
[[nodiscard]] inline bool looksLikeIban(const std::string& iban) {
    if (iban.size() < 15 || iban.size() > 34) {
        return false;
    }
    if (std::isalpha(static_cast<unsigned char>(iban[0])) == 0 ||
        std::isalpha(static_cast<unsigned char>(iban[1])) == 0) {
        return false;
    }
    for (char chr : iban) {
        if (std::isalnum(static_cast<unsigned char>(chr)) == 0) {
            return false;
        }
    }
    return true;
}

/// @brief A saved beneficiary.
struct PayeeInfo {
    std::int64_t id = 0;
    std::string owner;
    std::string name;
    std::string iban;
    std::string bankName;
};

/// @brief Add a beneficiary for the current owner.
///
/// Designed for field-by-field entry via `BridgeHandler::set<>`: the action only
/// becomes ready once both a name and a plausible IBAN are present.
struct AddPayee {
    std::string name;
    std::string iban;
    std::string bankName;

    [[nodiscard]] bool validate() const { return !name.empty() && looksLikeIban(iban); }
};

/// @brief Remove a beneficiary by id.
struct RemovePayee {
    std::int64_t id = 0;
};

/// @brief List the current owner's beneficiaries.
struct ListPayees {
    std::string owner;  ///< empty => session principal
};

/// @brief Result of `ListPayees`.
struct PayeeList {
    std::vector<PayeeInfo> payees;
};

}  // namespace bank::dto
