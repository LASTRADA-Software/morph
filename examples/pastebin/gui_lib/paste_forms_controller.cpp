// SPDX-License-Identifier: Apache-2.0
#include "paste_forms_controller.hpp"

// submitIfValid() is a template (OnReply/OnError deduced per call site,
// exactly like FormsControllerCore's own) and so stays fully defined in the
// header, alongside everything else here — this translation unit exists
// only to give the constructor (and this class generally) exactly one
// non-inline definition, matching every other gui_lib/*.cpp in this rung.

namespace pastebin::gui {

PasteFormsController::PasteFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                            std::string schemasJson)
    : _handler{bridge, executor}, _schemasJson{std::move(schemasJson)} {}

}  // namespace pastebin::gui
