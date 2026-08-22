// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// `MORPH_QML_FORMS_BRIDGE_MEMBERS` — the mechanical half of a QML-facing
/// forms bridge, written once.
///
/// Qt's moc cannot process class templates, so every Qt/QML-facing model needs
/// its own concrete `QObject` subclass even though
/// `morph::qt::forms::FormsControllerCore<Model>` already holds the real
/// generic logic. Comparing those subclasses across the ladder, roughly two
/// thirds of each is byte-for-byte identical: constructor forwarding,
/// `schemasJson()`, and `submitIfValid`'s `QString`↔`std::string` conversion
/// plus its `.then`/`.onError` → `emit replyReceived` plumbing. Only the
/// DTO→`QVariantMap` projections and the per-model invokable/signal vocabulary
/// are genuinely per-model.
///
/// This macro supplies the identical two thirds.

#include <QObject>
#include <QString>

#ifndef Q_MOC_RUN
#include <morph/qt/forms/forms_controller_core.hpp>

#include <exception>
#include <string>
#include <utility>
#endif

/// @brief Declares the mechanical members of a QML forms bridge.
///
/// **Members only — deliberately not the whole class.** The enclosing `class`,
/// `Q_OBJECT`, and the `signals:` block must stay hand-written in the header,
/// because moc does not see a `signals:` access specifier that arrives through
/// a macro expansion. It does not diagnose this: it expands the macro, emits a
/// metaobject containing the `Q_INVOKABLE` members, and silently omits the
/// signals — so a macro that tried to emit the whole class produces an object
/// that looks correctly wired and whose signals do not exist. That fails at
/// link if the signal is emitted, and would fail at runtime as a connection
/// that never fires if it were not. `Q_INVOKABLE` and ordinary members survive
/// expansion, which is why this macro covers exactly them.
///
/// **Include this header outside any `#ifndef Q_MOC_RUN` guard.** The ladder's
/// bridge headers routinely guard morph's template-heavy includes from moc,
/// and putting this one inside that guard leaves the macro undefined under
/// moc — so the invocation expands to *nothing*, moc emits a metaobject with
/// no `submitIfValid`, and the bridge still compiles and links while QML's
/// calls into it go nowhere. Nothing diagnoses it. The macro guards its own
/// template-heavy member internally so it is safe for moc to read.
///
/// The enclosing class must declare, itself:
/// - `Q_OBJECT`
/// - `Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)`
/// - `signals: void replyReceived(const QString& actionType, bool ok, const QString& payload);`
///
/// Usage:
/// ```cpp
/// class FormsBridge : public QObject {
///     Q_OBJECT
///     Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)
///     MORPH_QML_FORMS_BRIDGE_MEMBERS(FormsBridge, paste::PasteModel, pasteSchemasJson())
///
///   signals:
///     void replyReceived(const QString& actionType, bool ok, const QString& payload);
/// };
/// ```
///
/// @param ClassName   The enclosing class's own name.
/// @param Model       The model type to bind `FormsControllerCore` to.
/// @param SchemasExpr Expression yielding the rung's `{actionType: schema}`
///        JSON (e.g. `pasteSchemasJson()`). Taken as an expression rather than
///        a constructor argument so the generated constructor keeps the
///        `(Bridge&, IExecutor*, QObject* parent = nullptr)` shape every
///        existing bridge and its call sites already use.
#define MORPH_QML_FORMS_BRIDGE_MEMBERS(ClassName, Model, SchemasExpr)                                       \
  public:                                                                                                    \
    /* @brief Binds this bridge to @p bridge, delivering replies on @p executor. */                          \
    ClassName(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr) \
        : QObject{parent}, _core{bridge, executor, (SchemasExpr)} {}                                         \
                                                                                                             \
    /* @brief The schema document the QML renderer parses. */                                                \
    [[nodiscard]] QString schemasJson() const { return QString::fromStdString(_core.schemasJson()); }         \
                                                                                                             \
    /* @brief Dispatches @p bodyJson as @p actionType's body; emits replyReceived. */                        \
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson) {                     \
        _core.submitIfValid(                                                                                 \
            actionType.toStdString(), bodyJson.toStdString(),                                                \
            [this, actionType](std::string resultJson) {                                                     \
                emit replyReceived(actionType, true, QString::fromStdString(resultJson));                    \
            },                                                                                               \
            [this, actionType](const std::exception_ptr& err) {                                              \
                try {                                                                                        \
                    std::rethrow_exception(err);                                                             \
                } catch (const std::exception& e) {                                                          \
                    emit replyReceived(actionType, false, QString::fromUtf8(e.what()));                      \
                }                                                                                            \
            });                                                                                              \
    }                                                                                                        \
                                                                                                             \
  private:                                                                                                   \
    /* Hidden from moc like every other template-heavy member in the ladder's */                              \
    /* bridges: moc needs the macros above, never morph's template headers.  */                               \
    MORPH_QML_FORMS_BRIDGE_CORE_MEMBER(Model)

/// @brief The `FormsControllerCore` member, hidden from moc.
///
/// Separated so the `Q_MOC_RUN` guard can wrap a member declaration without a
/// preprocessor conditional inside a line-continued macro body, which is not
/// legal.
/// @param Model The model type to bind `FormsControllerCore` to.
#ifdef Q_MOC_RUN
#  define MORPH_QML_FORMS_BRIDGE_CORE_MEMBER(Model)
#else
#  define MORPH_QML_FORMS_BRIDGE_CORE_MEMBER(Model) ::morph::qt::forms::FormsControllerCore<Model> _core;
#endif
