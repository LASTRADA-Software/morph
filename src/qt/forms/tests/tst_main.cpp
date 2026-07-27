// SPDX-License-Identifier: Apache-2.0

// Qt Quick Test runner for the MorphForms QML module. The module is linked
// statically (its plugin is imported below), so `import MorphForms` resolves
// from the embedded resources.

#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

Q_IMPORT_QML_PLUGIN(MorphFormsPlugin)

QUICK_TEST_MAIN(morph_forms_qml_tests)
