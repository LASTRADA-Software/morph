var index =
[
    [ "Overview", "index.html#overview", null ],
    [ "Namespace map", "index.html#namespace-map", null ],
    [ "Layer diagram", "index.html#layer-diagram", null ],
    [ "Deployment topologies", "index.html#deployment-topologies", null ],
    [ "Wire protocol", "index.html#wire-protocol", null ],
    [ "Component detail", "index.html#component-detail", [
      [ "Executors", "index.html#executors", null ],
      [ "StrandExecutor", "index.html#strandexecutor", null ],
      [ "Completion<T>", "index.html#completiont", null ],
      [ "Registry & type erasure", "index.html#registry--type-erasure", null ],
      [ "HandlerBinding — why it exists", "index.html#handlerbinding--why-it-exists", null ],
      [ "Logger", "index.html#logger", null ],
      [ "Authenticated sessions", "index.html#authenticated-sessions", null ],
      [ "NetworkMonitor", "index.html#networkmonitor", null ],
      [ "IOfflineQueue + InMemoryOfflineQueue", "index.html#iofflinequeue--inmemoryofflinequeue", null ],
      [ "Action log — ordered, coalescing, identity-aware execution history", "index.html#action-log--ordered-coalescing-identity-aware-execution-history", null ],
      [ "SyncWorker", "index.html#syncworker", null ],
      [ "ReconnectCoordinator", "index.html#reconnectcoordinator", null ],
      [ "Conflict Resolution — a domain concern, not a framework concern", "index.html#conflict-resolution--a-domain-concern-not-a-framework-concern", null ]
    ] ],
    [ "Thread safety", "index.html#thread-safety", null ],
    [ "Error propagation", "index.html#error-propagation", null ],
    [ "Adding a new model and actions", "index.html#adding-a-new-model-and-actions", null ],
    [ "Subscriptions and fielded actions", "index.html#subscriptions-and-fielded-actions", [
      [ "API", "index.html#api", null ],
      [ "Behavior", "index.html#behavior", null ]
    ] ],
    [ "Exact values, units, and schema-driven forms", "index.html#exact-values-units-and-schema-driven-forms", [
      [ "<tt>morph::math::Rational</tt> — exact numbers on the wire", "index.html#morphmathrational--exact-numbers-on-the-wire", null ],
      [ "<tt>morph::units::Quantity<U></tt> — one kind of empty, units as types", "index.html#morphunitsquantityu--one-kind-of-empty-units-as-types", null ],
      [ "<tt>morph::forms</tt> — schemas for auto-built GUIs", "index.html#morphforms--schemas-for-auto-built-guis", null ],
      [ "<tt>morph::time::Timestamp</tt> and <tt>morph::forms::Choice</tt> — dates and combo boxes", "index.html#morphtimetimestamp-and-morphformschoice--dates-and-combo-boxes", null ]
    ] ],
    [ "Header map", "index.html#header-map", [
      [ "Library headers (<tt>include/morph/</tt>)", "index.html#library-headers-includemorph", [
        [ "<tt>core/</tt> — async core, registry, bridge, backends, wire", "index.html#core--async-core-registry-bridge-backends-wire", null ],
        [ "<tt>journal/</tt> — ordered, replayable action log", "index.html#journal--ordered-replayable-action-log", null ],
        [ "<tt>offline/</tt> — connectivity + replay", "index.html#offline--connectivity--replay", null ],
        [ "<tt>session/</tt> — per-call context + authentication", "index.html#session--per-call-context--authentication", null ],
        [ "<tt>forms/</tt> — JSON-Schema generation for auto-built GUIs", "index.html#forms--json-schema-generation-for-auto-built-guis", null ],
        [ "<tt>util/</tt> — exact values, units, time", "index.html#util--exact-values-units-time", null ]
      ] ],
      [ "Qt integration headers (<tt>include/morph/qt/</tt>)", "index.html#qt-integration-headers-includemorphqt", null ]
    ] ],
    [ "Known limitations", "index.html#known-limitations", [
      [ "Validators do not run server-side", "index.html#validators-do-not-run-server-side", null ],
      [ "<tt>RemoteServer</tt> must be heap-allocated", "index.html#remoteserver-must-be-heap-allocated", null ],
      [ "<tt>NetworkMonitor</tt> callbacks must not block", "index.html#networkmonitor-callbacks-must-not-block", null ],
      [ "<tt>Bridge::switchBackend</tt> must not be called from <tt>onBackendChanged</tt>", "index.html#bridgeswitchbackend-must-not-be-called-from-onbackendchanged", null ],
      [ "A null callback executor drops the callback (but not the orphan log)", "index.html#a-null-callback-executor-drops-the-callback-but-not-the-orphan-log", null ],
      [ "<tt>MainThreadExecutor::runFor</tt> does not drain on timeout", "index.html#mainthreadexecutorrunfor-does-not-drain-on-timeout", null ]
    ] ],
    [ "Key design decisions", "index.html#key-design-decisions", null ]
];