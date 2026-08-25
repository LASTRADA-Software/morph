// SPDX-License-Identifier: Apache-2.0
//
// Trivial main() so the QT_NO_SSL try_compile() guard in tests/qt/CMakeLists.txt
// links a full executable (try_compile builds one by default) around
// src/qt/qt_websocket_backend.cpp, which has no main() of its own.

int main() { return 0; }
