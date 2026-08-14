// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "pastebin/dto/paste_dto.hpp"

#include <exception>

// Guarded like examples/bank/gui/controllers/AccountController.hpp and
// examples/forms/gui_qml/FormsController.hpp: moc only needs the
// Q_OBJECT/signals declarations below (and the DTO types above, which are
// lightweight — no Lightweight/ODBC dependency); it must not be pointed at
// morph's template-heavy bridge.hpp (not a real C++ front end, and
// bridge.hpp's template machinery produces bogus moc output the same way
// paste_model.hpp historically did when it transitively pulled in
// Lightweight's DataMapper machinery through the since-removed
// pastebin/db/db_model.hpp -- paste_model.hpp itself no longer has any
// Lightweight/ODBC dependency at all, now that PasteModel acquires a
// connection per execute() call from Lightweight::GlobalDataMapperPool()
// instead of owning one, but this guard stays for bridge.hpp's own sake).
#ifndef Q_MOC_RUN
#include "pastebin/models/paste_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace pastebin::gui {

/// @brief Routes CreatePaste/GetPaste/EditPaste/DeletePaste/ListPastes
///        through a `BridgeHandler<PasteModel>`, surfacing typed errors to
///        whatever view composes this (QML properties/signals, Task 12).
///        Translates and routes only — no domain logic
///        (`IMPLEMENTATION.md` rule 2).
class PastePresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    PastePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Stores a new paste. Emits `created` on success, `failed` on error.
    /// @param action The paste to store.
    void create(CreatePaste action);

    /// @brief Reads (and consumes one read of) a paste. Emits `loaded` on
    ///        success, `failed` on error.
    /// @param action The paste to read.
    void get(GetPaste action);

    /// @brief Replaces an editable paste's content and syntax. Emits
    ///        `edited` on success, `failed` on error.
    /// @param action The edit to apply.
    void edit(EditPaste action);

    /// @brief Deletes a paste. Emits `removed` on success, `failed` on error.
    /// @param action The paste to delete.
    void remove(DeletePaste action);

    /// @brief Fetches one page of public pastes. Emits `listed` on success,
    ///        `failed` on error.
    /// @param action The page request.
    void list(ListPastes action);

  signals:
    void created(CreatePasteResult result);
    void loaded(PasteView view);
    void edited(PasteView view);
    void removed();
    void listed(ListPastesResult result);
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

  private:
    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument below: rethrows @p err to recover the concrete
    ///        message and emits `failed`. Passed as `track<T>`'s `onErr`
    ///        parameter — see `Presenter::track()`'s doc comment
    ///        (`examples/common/gui/presenter.hpp`) for why that, rather
    ///        than a separate `.onError(...)` attached directly on the
    ///        `Completion<T>` beforehand, is where this belongs. Factored
    ///        out (rather than duplicated per action) since it does not
    ///        depend on the action's result type `T` — only on the
    ///        `std::exception_ptr` every `onErr` callback receives — so it
    ///        stays a plain member function, not a template.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<PasteModel> _handler;
};

}  // namespace pastebin::gui
