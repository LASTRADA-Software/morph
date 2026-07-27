#pragma once

namespace demo {

[[deprecated("this is old, do not use")]]
inline int oldThing() {
    return 0;
}

}  // namespace demo
