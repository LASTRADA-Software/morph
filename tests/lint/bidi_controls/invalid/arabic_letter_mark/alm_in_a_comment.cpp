// SPDX-License-Identifier: Apache-2.0
//
// morph#628's own reproduction, byte for byte: a JSON-payload tool decoded
// the escape before the file was written, so the comment below carries a
// single raw U+061C where six characters were typed.
//
// `inline` is load-bearing, not style: a namespace-scope `constexpr`
// has internal linkage, and clang-tidy-diff analyses every line of a
// new file, so an unused one is reported as
// clang-diagnostic-unused-const-variable and fails the job. Nothing
// here is compiled, but the gate that reads it does not know that.

// Before morph#591 this edge emitted "؜-1050.25" -- the sign is invisible.

inline constexpr const char* kSign = "؜-";
