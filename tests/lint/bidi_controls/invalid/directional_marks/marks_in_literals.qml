// SPDX-License-Identifier: Apache-2.0
//
// morph#642's shape: assertion literals carrying raw U+200E / U+200F while
// their neighbours in the same file use the escape.

import QtQuick

Item {
    property string azIR: "‎+‎"
    property string ckbIQ: "‏+"
}
