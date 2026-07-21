#pragma once

namespace demo {

[[deprecated("removed in 2.0.0; use demo::NewThing instead")]]
inline int oldThing() {
    return 0;
}

}  // namespace demo
