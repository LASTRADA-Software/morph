// SPDX-License-Identifier: Apache-2.0
//
// morph_examples_client links every example application for local mode.

#include <app/app_source.hpp>
#include <gallery/gallery.hpp>
#include <vector>

#include "local_apps.hpp"

namespace morph::examples::client {

std::vector<LocalApp> localApps() { return {gallery::localApp()}; }

}  // namespace morph::examples::client
