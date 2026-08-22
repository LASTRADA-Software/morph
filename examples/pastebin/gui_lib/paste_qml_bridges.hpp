// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Guarded exactly like paste_presenter.hpp's own includes: AUTOMOC runs moc
// over this header, and moc must not be pointed at morph's template-heavy
// bridge.hpp or at paste_model.hpp, which pulls in Lightweight's DataMapper
// machinery — moc is not a C++ front end and mis-parses it, emitting the rest
// of the file inside a namespace it wrongly believes is still open. moc needs
// nothing from these headers: the macros, signals and `Q_INVOKABLE`
// signatures below are all it reads.
// Outside the Q_MOC_RUN guard, and it must stay outside: this header defines
// MORPH_QML_FORMS_BRIDGE_MEMBERS, and moc has to expand it. Guarded, the macro
// is undefined under moc and its invocation expands to *nothing* -- moc then
// emits a metaobject with no submitIfValid at all, silently, and the bridge
// compiles and links while QML's calls into it go nowhere. The macro guards
// its own template-heavy member internally instead.
#include <morph/qt/forms/qml_forms_bridge.hpp>

#ifndef Q_MOC_RUN
#include "paste_schemas.hpp"

#include "paste_presenter.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

/// @file
/// The two QML-facing adapters pastebin's shells put in front of the Task 10
/// GUI-layer classes. They live in `gui_lib` — not in a shell's `main.cpp` —
/// because *both* shells need them and must be the same program:
/// `gui/main.cpp` (desktop) and `gui_wasm/main_wasm.cpp` (browser) differ
/// only in how they choose a deployment mode, per `examples/TESTING.md`'s
/// "same client code" requirement and its ban on bank's shadow-header
/// pattern.
///
/// @par Why these adapters exist at all
/// Neither Task 10 class is directly consumable from QML — deliberately.
/// `FormsControllerCore<PasteModel>` is a plain class template (no `Q_OBJECT`) whose
/// `submitIfValid` takes C++ callbacks, and `PastePresenter`'s signals carry
/// raw C++ DTOs (`PasteView`, `ListPastesResult`) that QML has no reading of.
/// The two classes below are the thinnest possible translation from those
/// surfaces to the `QString`/`QVariantMap` shapes QML binds against. They
/// decide nothing: every conditional and every rule stays in the model, and
/// the only formatting they perform is rendering a `Timestamp`/`Quantity` as
/// the text a `Label` shows (`TESTING.md` presenter rule 6's "QML is
/// bindings-only", `IMPLEMENTATION.md` rule 2's "pure glue").
///
/// @par Qt6::Core only
/// Nothing here needs Qt Quick or Qt Qml: a `QVariantMap` is Qt Core, and the
/// engine-facing side is `setInitialProperties` in each shell. That keeps
/// `ladder_pastebin_gui_lib` inside presenter rule 1's Qt6::Core-only bound
/// and keeps these adapters instantiable under a plain `QCoreApplication`.
///
/// @par Threading, and why no `Q_DECLARE_METATYPE`/`qRegisterMetaType`
/// Everything in a client process lives on the one Qt event-loop thread: the
/// engine, both adapters, and the `PastePresenter` they wrap are all
/// constructed on it, and `AppContext`'s executor is a `QtExecutor`, so every
/// completion callback — and therefore every `PastePresenter` signal emission
/// — is delivered on that same thread too. A same-thread `AutoConnection` is
/// a *direct* connection: the argument is passed straight through as a C++
/// reference and Qt never asks the meta-type system to copy it. So the DTO
/// signals need no `Q_DECLARE_METATYPE` and no `qRegisterMetaType`, and none
/// is added: an unused registration would be a speculative stub, and the
/// worker pool that does run on other threads is behind the `Bridge`, which
/// never emits a Qt signal. The one thing that *would* break this is moving a
/// presenter to another thread or connecting one to a QML object across
/// contexts — neither of which either shell does, and both of which would
/// fail loudly ("Cannot queue arguments of type 'pastebin::PasteView'")
/// rather than silently.

namespace pastebin::gui {

/// @brief QML-facing forms adapter, built on
///        `morph::qt::forms::FormsControllerCore<PasteModel>` via
///        `MORPH_QML_FORMS_BRIDGE_MEMBERS`.
///
/// Same surface `DynamicForm.qml` expects of a controller — a `schemasJson`
/// property, `submitIfValid(actionType, bodyJson)`, and a `replyReceived`
/// signal — so the shipped renderer needs no pastebin-specific knowledge.
class FormsBridge : public QObject {
    Q_OBJECT

    /// @brief `{actionType: schema}` JSON — everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

    // Constructor, schemasJson() and submitIfValid() are byte-for-byte
    // identical in every rung's forms bridge; the macro holds that half once
    // (morph/qt/forms/qml_forms_bridge.hpp). Q_OBJECT, the Q_PROPERTY above
    // and the signals block below stay here because moc does not see a
    // `signals:` specifier that arrives through a macro expansion -- see the
    // macro's own doc comment.
    MORPH_QML_FORMS_BRIDGE_MEMBERS(FormsBridge, ::pastebin::PasteModel, ::pastebin::gui::pasteSchemasJson())

signals:
    /// @brief Emitted once per `submitIfValid`. @p payload is the result JSON
    ///        when @p ok, otherwise the error message.
    /// @param actionType The action the reply belongs to.
    /// @param ok         Whether the dispatch succeeded.
    /// @param payload    Result JSON, or the error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

};

/// @brief QML-facing face of `pastebin::gui::PastePresenter`.
///
/// Turns the presenter's DTO-carrying signals into `QVariantMap`/`QVariantList`
/// property bags and its typed `create`/`get`/`list`/`remove` calls into
/// id-string invokables. No decisions: burn/expiry, visibility and pagination
/// are all the model's, and this only relays what the server computed.
class PasteBridge : public QObject {
    Q_OBJECT

public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    PasteBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Fetches the first page of public pastes.
    Q_INVOKABLE void refresh();

    /// @brief Reads @p id — which consumes one read, so a burn-after-N paste
    ///        moves one step closer to being burned. Emits `loaded`, or
    ///        `failed` with the model's own message for a burned/expired/absent
    ///        paste.
    /// @param id The paste to open.
    Q_INVOKABLE void open(const QString& id);

    /// @brief Deletes @p id.
    /// @param id The paste to delete.
    Q_INVOKABLE void remove(const QString& id);

signals:
    /// @brief Emitted once the wrapped presenter's registration round trip
    ///        settles — successfully or not (`Presenter::bound()`,
    ///        `morph/core/bridge.hpp`'s `whenBound()`). `Remote` mode's first
    ///        dispatch attempt fails fast with "handler not bound" until this
    ///        fires; `Local` mode fires it synchronously from this
    ///        constructor, since its handler is already bound by
    ///        construction. QML's `Main.qml` gates its bootstrap `refresh()`
    ///        on this instead of retrying on a `Timer`.
    void bound();

    /// @brief One page of `ListPastes` rows, each a `{id, syntax, createdAt, visibility}` map.
    /// @param rows The page's rows.
    void listed(const QVariantList& rows);
    /// @brief A fetched paste, as a property bag.
    /// @param paste The paste's fields, rendered as display strings.
    void loaded(const QVariantMap& paste);
    /// @brief A `DeletePaste` succeeded.
    void removed();
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

private:
#ifndef Q_MOC_RUN
    PastePresenter _presenter;
#endif
};

}  // namespace pastebin::gui
