// SPDX-License-Identifier: Apache-2.0
//
// morph_remote_client links no application: it is remote only, and its binary carries no example code.

#include <morph/client/source.hpp>
#include <vector>

#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<::morph::client::LocalApp> localApps() { return {}; }

}  // namespace morph::examples::client
