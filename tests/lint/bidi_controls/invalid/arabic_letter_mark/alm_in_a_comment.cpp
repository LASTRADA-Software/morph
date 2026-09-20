// SPDX-License-Identifier: Apache-2.0
//
// morph#628's own reproduction, byte for byte: a JSON-payload tool decoded
// the escape before the file was written, so the comment below carries a
// single raw U+061C where six characters were typed.

// Before morph#591 this edge emitted "؜-1050.25" -- the sign is invisible.

constexpr const char* kSign = "؜-";
