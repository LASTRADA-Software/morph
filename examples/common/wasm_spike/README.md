# WASM-remote spike

Proves `morph::qt::QtWebSocketBackend` works from a WASM client — per
[`../../TESTING.md`](../../TESTING.md), "Bank's WASM build is local-only... a
WASM client over `QtWebSocketBackend` has never been run." This is a client
only; point it at a native `RemoteServer` + `QtWebSocketServer` hosting
`SpikeEchoModel` (see `spike_model.hpp`), started separately — for example
`ladder_common_tests`' own `[wasm-spike]`-tagged test case
(`../testkit/test_wasm_registration_path_native.cpp`) demonstrates the exact
registration/execute call sequence natively; a standalone server binary
hosting `SpikeEchoModel` for the browser smoke would be built the same way.

## Environment note (as of this task, and still true)

This spike's source (`spike_model.hpp`, `main_wasm.cpp`, this
`CMakeLists.txt`) was written and reviewed, but **no Emscripten toolchain
(`emcc`/`emcmake`) was available in the environment this was authored in**, so
the actual WASM compile gate below has never been run against it. The CMake
is written in good faith against `../../../CMakeLists.txt`'s existing
`MORPH_BUILD_QT` wiring and bank's `gui_wasm` as a template, but until it is
actually configured under `emcmake`, treat it as unverified. Rung 1's task 13
hit the identical wall (`emcmake: command not found`) while writing pastebin's
WASM client, and added `.github/workflows/wasm-ladder.yml` — a compile gate
that builds *this* target by name alongside every rung's `gui_wasm` client. Its
first green run is what retires this note. In particular:
`morph::qt` (which this target links) only exists when the top-level
`MORPH_BUILD_QT=ON`, which itself runs `find_package(Qt6 COMPONENTS
WebSockets REQUIRED)` — whether a standard Qt-for-WebAssembly install
actually ships a working `Qt6::WebSockets` component is itself part of what
the first real `emcmake` attempt against this target needs to establish.

## Manual verification

1. Configure and build for `wasm32-emscripten` (see `../../bank/gui_wasm` for
   the toolchain setup this mirrors).
2. Start a server hosting `SpikeEchoModel` on a known port.
3. Configure with `-DMORPH_LADDER_WASM_SPIKE_SERVER_URL=ws://127.0.0.1:<port>`,
   build `morph_ladder_wasm_spike`, serve the output over plain HTTP (no
   COOP/COEP headers needed — this target avoids `-pthread`, same as bank's
   WASM GUI).
4. Open the page, check the browser console for
   `morph-ladder-wasm-spike: connected` followed by
   `morph-ladder-wasm-spike: result= 99`.

## Fallback plan, if step 4 does not show `result= 99`

Per `TESTING.md`'s framework-gaps list and `LADDER.md`'s framework
prerequisites, the two most likely failure modes and their owning findings:

- **Page aborts before "connected" logs.** Something in the registration path
  still nests a synchronous event loop despite `asyncRegistrationEnabled =
  true` — re-open the *async shared/keyed attach* finding even though this
  spike deliberately avoids the *shared* path; if the *plain* async path also
  aborts, that is a new, more severe finding (the plain path was supposed to
  already be WASM-safe per `[issue26]`'s native tests) — file it as a GitHub
  issue per [`examples/FINDINGS.md`](../../FINDINGS.md), titled for the gap
  ("plain async registration aborts under WASM") with `severity: blocker`,
  and this rung's exit criteria (per `examples/FINDINGS.md`) are **not met**
  until it is at least triaged.
- **"connected" logs but no "result=" ever appears.** The action dispatch
  itself is hanging — check whether `Completion` needs the
  *client-side execute deadline* finding's
  execute-deadline fix to surface the failure at all (today it would just
  hang silently, matching `002`'s description exactly).

If either failure mode reproduces, do **not** silently work around it in this
spike — record it as a finding (per the two bullets above) and mark rung 0's
Task 10 complete anyway with a "documents a real blocker" note; `FINDINGS.md`'s
rung exit criteria explicitly allow a rung to exit with findings still
`open`/`fix-scheduled`, just not un-triaged.

If the Emscripten configure itself fails before either failure mode above
becomes observable (for example, `find_package(Qt6 COMPONENTS WebSockets
REQUIRED)` failing under `emcmake`, per the environment note above), that is
also a real finding, not a CMake bug in this directory to quietly work
around — file it the same way, citing the specific configure error.
