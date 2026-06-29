// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `payees` (beneficiaries) table.
struct PayeeRecord {
    static constexpr std::string_view TableName = "payees";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;
    Light::Field<Light::SqlAnsiString<34>, Light::SqlRealName{"iban"}> iban;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"bank_name"}> bankName;
};

}  // namespace bank::db
