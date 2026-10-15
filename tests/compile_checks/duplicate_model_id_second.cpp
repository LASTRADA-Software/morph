// SPDX-License-Identifier: Apache-2.0
//
// Half of a process that registers two model types under one id, one per translation unit, as two applications linked
// into one binary would. Its static initialisation must stop with the duplicate-id message before main runs.

#include <morph/core/registry.hpp>

namespace {

struct DupSecondModel {
    int value = 0;
};

}  // namespace

BRIDGE_REGISTER_MODEL(DupSecondModel, "Dup_StaticModel")
