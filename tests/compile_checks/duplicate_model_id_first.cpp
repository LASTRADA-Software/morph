// SPDX-License-Identifier: Apache-2.0
//
// Half of a process that registers two model types under one id, one per translation unit, as two applications linked
// into one binary would. Its static initialisation must stop with the duplicate-id message before main runs.

#include <morph/core/registry.hpp>

namespace {

struct DupFirstModel {
    int value = 0;
};

}  // namespace

BRIDGE_REGISTER_MODEL(DupFirstModel, "Dup_StaticModel")

#include <cstdio>

int main() {
    std::puts("REACHED MAIN");
    return 0;
}
