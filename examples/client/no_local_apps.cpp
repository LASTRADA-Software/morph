// SPDX-License-Identifier: Apache-2.0
//
// morph_client links no application: it is remote only, and its binary carries no example code.

#include <app/app_source.hpp>
#include <vector>

#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<LocalApp> localApps() { return {}; }

}  // namespace morph::examples::client
