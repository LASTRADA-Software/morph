---
id: 007
title: QtExecutor has no optional QObject* context target
subsystem: qt
severity: paper-cut
source: examples/LADDER.md framework prerequisite 2
disposition: open
test: spec-cited
issue: https://github.com/LASTRADA-Software/morph/issues/47
---

`QtExecutor` (`include/morph/qt/qt_executor.hpp`) hardcodes `QCoreApplication::instance()` as the target for `QMetaObject::invokeMethod`. There is no per-thread-affinity constructor parameter to post work to a different `QObject`, making it inflexible when an app needs to dispatch to a specific thread that is not the main application thread.

**What happens instead:** multi-threaded UIs that need executor affinity to non-main threads must implement their own `IExecutor` shim. This becomes relevant once a rung needs N client threads (none do yet).
