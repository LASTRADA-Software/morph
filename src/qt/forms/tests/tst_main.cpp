// SPDX-License-Identifier: Apache-2.0

// Qt Quick Test runner for the MorphForms QML module. The module is linked
// statically (its plugin is imported below), so `import MorphForms` resolves
// from the embedded resources.

#include <QtQml/qqmlextensionplugin.h>
#include <QtQuickTest/quicktest.h>

#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

Q_IMPORT_QML_PLUGIN(MorphFormsPlugin)

/// @brief Publishes the shared `x-rules` corpus to the QML engine.
///
/// `tst_DynamicFormRuleCorpus.qml` and `tests/test_forms_rule_corpus.cpp` read
/// one and the same file (morph#176). QML cannot fetch it itself: an
/// XMLHttpRequest against a `file:` URL is refused unless
/// `QML_XHR_ALLOW_FILE_READ` is exported, which would make the suite's result
/// depend on how it was invoked. Reading it here instead keeps the path in one
/// place — CMake's `MORPH_FORMS_RULE_CORPUS` — for both readers, and hands QML
/// the bytes **verbatim**: the corpus stores each schema as JSON text, and any
/// parse/re-serialise on the way in would round the very int64 literals the
/// `equals-int64` rows exist to pin.
class MorphFormsQmlTestSetup : public QObject {
    Q_OBJECT

public:
    MorphFormsQmlTestSetup() = default;

public Q_SLOTS:
    /// @brief Injects `ruleCorpusJson` into the QML root context.
    /// @param engine The engine Qt Quick Test just created.
    void qmlEngineAvailable(QQmlEngine* engine) {
        QString text;
        QFile corpus(QStringLiteral(MORPH_FORMS_RULE_CORPUS));
        if (corpus.open(QIODevice::ReadOnly | QIODevice::Text)) {
            text = QString::fromUtf8(corpus.readAll());
        }
        // An unreadable corpus lands as an empty string, which the QML side
        // fails on explicitly rather than silently running zero rows.
        engine->rootContext()->setContextProperty(QStringLiteral("ruleCorpusJson"), text);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(morph_forms_qml_tests, MorphFormsQmlTestSetup)

#include "tst_main.moc"
