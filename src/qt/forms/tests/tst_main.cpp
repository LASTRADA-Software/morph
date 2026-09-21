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

namespace {

/// @brief Publishes the two shared test corpora to the QML engine.
///
/// Each corpus is one file with two readers — a C++ suite and a QML one:
///
/// - `tests/data/rule_corpus.json` (morph#176) — read by
///   `tests/test_forms_rule_corpus.cpp` and `tst_DynamicFormRuleCorpus.qml`;
/// - `tests/data/instance_bounds.json` (morph#164) — read by
///   `tests/test_forms_instance_constraints.cpp` and
///   `tst_DynamicFormInstanceBounds.qml`.
///
/// QML cannot fetch either itself: an XMLHttpRequest against a `file:` URL is
/// refused unless `QML_XHR_ALLOW_FILE_READ` is exported, and a suite whose
/// coverage depends on how it was invoked is the failure mode these corpora
/// exist to rule out. Reading them here keeps each path in one place (the
/// CMake compile definitions) and hands QML the bytes **verbatim**: both files
/// store schemas as JSON text, and any parse/re-serialise on the way in would
/// round the very int64 literals some rows exist to pin.
class MorphFormsQmlTestSetup : public QObject {
    Q_OBJECT

public Q_SLOTS:
    /// @brief Injects `ruleCorpusJson` and `instanceBoundsJson` into the QML
    ///        root context.
    /// @param engine The engine Qt Quick Test just created.
    //
    // The body never touches the object, so
    // `readability-convert-member-functions-to-static` offers to make it
    // static. Declined: this is not our call to make. QUICK_TEST_MAIN_WITH_SETUP
    // constructs a `MorphFormsQmlTestSetup` and hands Qt Quick Test its address
    // as a `QObject*` (quicktest.h), and Qt finds this hook on that instance's
    // metaobject by name -- the signature is a contract with Qt, which
    // documents the hook as a slot, not a shape this file is free to change.
    // A slot is a member function by definition; `static` would say something
    // about the fixture that is not true of the interface it implements.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void qmlEngineAvailable(QQmlEngine* engine) {
        // An unreadable file lands as an empty string, which the QML side fails
        // on explicitly rather than silently running zero rows.
        engine->rootContext()->setContextProperty(QStringLiteral("ruleCorpusJson"),
                                                  readAll(QStringLiteral(MORPH_FORMS_RULE_CORPUS)));
        engine->rootContext()->setContextProperty(QStringLiteral("instanceBoundsJson"),
                                                  readAll(QStringLiteral(MORPH_FORMS_INSTANCE_BOUNDS)));
    }

private:
    /// @brief Reads a whole UTF-8 file, or returns an empty string.
    /// @param path Absolute path, from a CMake compile definition.
    /// @return The file's contents, verbatim.
    static QString readAll(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(morph_forms_qml_tests, MorphFormsQmlTestSetup)

#include "tst_main.moc"
