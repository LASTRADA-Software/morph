// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/core/units.hpp"

#include <string>

#include "lims/org/core/errors.hpp"

namespace lims {

::morph::math::Rational convertStock(::morph::math::Rational const& value, StockUnit from, StockUnit to) {
    if (from == to) {
        return value;
    }
    for (auto const& relation : ::morph::units::UnitTraits<StockUnit>::relations) {
        // A relation is used in both directions: 1 from == fromTo · to, so the reverse divides.
        if (relation.from == from && relation.to == to) {
            auto const product = ::morph::math::checkedMul(value, relation.fromTo);
            if (!product.has_value()) {
                throw OutOfRange{"a quantity is too large to convert exactly"};
            }
            return *product;
        }
        if (relation.from == to && relation.to == from) {
            auto const quotient = ::morph::math::checkedDiv(value, relation.fromTo);
            if (!quotient.has_value()) {
                throw OutOfRange{"a quantity is too large to convert exactly"};
            }
            return *quotient;
        }
    }
    throw ValidationError{"a quantity in " + std::string{stockUnitName(from)} + " cannot be held in " +
                          std::string{stockUnitName(to)}};
}

}  // namespace lims
