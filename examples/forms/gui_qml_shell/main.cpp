// SPDX-License-Identifier: Apache-2.0

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "FormsShellController.hpp"

int main(int argc, char** argv) {
    QCoreApplication::setOrganizationName(QStringLiteral("morph"));
    QCoreApplication::setApplicationName(QStringLiteral("morph-shell"));

    QGuiApplication app{argc, argv};
    QQmlApplicationEngine engine;

    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const auto& w : warnings)
                qWarning("%s", qPrintable(w.toString()));
        });

    FormsShellController shellController;
    engine.rootContext()->setContextProperty(QStringLiteral("morphController"), &shellController);

    engine.loadFromModule(QStringLiteral("MorphFormsShell"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        qWarning("Failed to load MorphFormsShell module");
        return 1;
    }

    return app.exec();
}