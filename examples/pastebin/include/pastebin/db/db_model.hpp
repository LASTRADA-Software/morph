// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef __EMSCRIPTEN__
#include <Lightweight/DataMapper/DataMapper.hpp>

#include <optional>
#endif

/// @file
/// Small mixin that gives a model a lazily-opened Lightweight `DataMapper`.
///
/// morph runs each model single-threaded on its own strand, so a model can own
/// its own database connection with no synchronisation. The connection is
/// created on first use (i.e. on the strand thread, during the first
/// `execute(...)`) rather than at construction, keeping ODBC handles on the
/// thread that actually uses them.
///
/// @par The Emscripten branch, and why it is not bank's shadow-header pattern
/// A WASM client is a *pure remote client* (`examples/IMPLEMENTATION.md` rule
/// 4's WASM clause: ODBC cannot run in the browser and no browser-side
/// substitute store may be written), so it never constructs `PasteModel` and
/// never calls `mapper()`. It does, however, have to **name** `PasteModel`:
/// `BridgeHandler<PasteModel>` — the whole client-side dispatch surface — is a
/// template over the model type, so `paste_model.hpp` (and through it this
/// header) is on the WASM client's include path even though no line of model
/// implementation is compiled there. `MORPH_CLIENT_ONLY`
/// (`docs/spec/core/registry.md`) removes the *link* dependency on the model's
/// constructor and `execute()` bodies for exactly this case; morph has since
/// grown `BRIDGE_REGISTER_ACTION_FOR_CLIENT(M, A, RESULT, NAME, ...)`
/// (`include/morph/core/registry.hpp`), which also removes the *header*
/// dependency by letting a client name `M`'s result type explicitly instead
/// of deducing it from a complete `M::execute(A)` — but only if `M` itself is
/// a declaration-only facade the client's `BridgeHandler<M>` never completes.
/// This rung's `PasteModel` is the real model, not a facade, so this WASM
/// client still pulls in this header transitively; adopting the facade
/// pattern to drop that dependency would be a rung-shape change, not done
/// here.
///
/// So under Emscripten this mixin becomes an empty base: same class, same
/// name, same models, no ODBC. `mapper()` is deliberately **absent** rather
/// than stubbed, so any attempt to actually reach the database from a browser
/// build fails to compile with "no member named 'mapper'" instead of linking
/// and failing at runtime. This is a two-line branch inside the persistence
/// layer, not bank's `gui_wasm/include/` shadow-header tree — no model, DTO,
/// presenter or QML file has a WASM variant, and the client code the two
/// shells share is byte-for-byte identical (`examples/TESTING.md`, "Do not
/// copy bank's `gui_wasm` shadow-header pattern").

namespace pastebin::db {

#ifndef __EMSCRIPTEN__

/// @brief Base providing `mapper()` — one lazily-constructed DataMapper per model.
class WithMapper {
protected:
    WithMapper() = default;

    /// @brief Returns this model's DataMapper, opening it on first use.
    [[nodiscard]] Lightweight::DataMapper& mapper() {
        if (!_mapper.has_value()) {
            _mapper.emplace();
        }
        return *_mapper;
    }

private:
    std::optional<Lightweight::DataMapper> _mapper;
};

#else

/// @brief Persistence-free base for the browser build — see this file's
///        Emscripten note. No `mapper()`: a WASM client has no database.
class WithMapper {
protected:
    WithMapper() = default;
};

#endif

}  // namespace pastebin::db
