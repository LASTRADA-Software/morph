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
      [ "Completion&lt;T&gt;", "index.html#completiont", null ],
      [ "Registry &amp; type erasure", "index.html#registry--type-erasure", null ],
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
      [ "<span class=\"tt\">morph::math::Rational</span> — exact numbers on the wire", "index.html#morphmathrational--exact-numbers-on-the-wire", null ],
      [ "<span class=\"tt\">morph::units::Quantity&lt;U&gt;</span> — one kind of empty, units as types", "index.html#morphunitsquantityu--one-kind-of-empty-units-as-types", null ],
      [ "<span class=\"tt\">morph::forms</span> — schemas for auto-built GUIs", "index.html#morphforms--schemas-for-auto-built-guis", null ],
      [ "<span class=\"tt\">morph::time::Timestamp</span> and <span class=\"tt\">morph::forms::Choice</span> — dates and combo boxes", "index.html#morphtimetimestamp-and-morphformschoice--dates-and-combo-boxes", null ]
    ] ],
    [ "Header map", "index.html#header-map", [
      [ "Library headers (<span class=\"tt\">include/morph/</span>)", "index.html#library-headers-includemorph", [
        [ "<span class=\"tt\">core/</span> — async core, registry, bridge, backends, wire", "index.html#core--async-core-registry-bridge-backends-wire", null ],
        [ "<span class=\"tt\">journal/</span> — ordered, replayable action log", "index.html#journal--ordered-replayable-action-log", null ],
        [ "<span class=\"tt\">offline/</span> — connectivity + replay", "index.html#offline--connectivity--replay", null ],
        [ "<span class=\"tt\">session/</span> — per-call context + authentication", "index.html#session--per-call-context--authentication", null ],
        [ "<span class=\"tt\">forms/</span> — JSON-Schema generation for auto-built GUIs", "index.html#forms--json-schema-generation-for-auto-built-guis", null ],
        [ "<span class=\"tt\">util/</span> — exact values, units, time", "index.html#util--exact-values-units-time", null ]
      ] ],
      [ "Qt integration headers (<span class=\"tt\">include/morph/qt/</span>)", "index.html#qt-integration-headers-includemorphqt", null ]
    ] ],
    [ "Known limitations", "index.html#known-limitations", [
      [ "<span class=\"tt\">RemoteServer</span> must be heap-allocated", "index.html#remoteserver-must-be-heap-allocated", null ],
      [ "<span class=\"tt\">NetworkMonitor</span> callbacks must not block", "index.html#networkmonitor-callbacks-must-not-block", null ],
      [ "<span class=\"tt\">Bridge::switchBackend</span> must not be called from <span class=\"tt\">onBackendChanged</span>", "index.html#bridgeswitchbackend-must-not-be-called-from-onbackendchanged", null ],
      [ "A null callback executor drops the callback (but not the orphan log)", "index.html#a-null-callback-executor-drops-the-callback-but-not-the-orphan-log", null ],
      [ "<span class=\"tt\">MainThreadExecutor::runFor</span> does not drain on timeout", "index.html#mainthreadexecutorrunfor-does-not-drain-on-timeout", null ]
    ] ],
    [ "Versioning &amp; compatibility", "index.html#versioning--compatibility", null ],
    [ "Key design decisions", "index.html#key-design-decisions", null ]
];