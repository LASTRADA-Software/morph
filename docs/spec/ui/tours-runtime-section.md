## Registering tours at runtime

`TourRegistry` (`tour_registry.hpp`) holds the tours a server adds while it runs, beside the app document's. It reads
the bundle served now through a getter, as `CustomScreens` does, so a development reload is seen.

- **Same form, same checks.** `add(json)` and `replace(json)` take one tour in the form of an entry of the app
  document's `tours` (`author::tourJson` writes it). The tour is checked as if appended to the served app document's
  `tours`: the loader checks its shape and its texts in the app scope and refuses an id the app document uses, and
  `checkTour`, the check the bundle's build runs, checks its screens and targets. A `{"t": key}` whose key is in no
  message catalogue of the bundle and not among the server's texts (`UiRegistry::setFieldTexts`) is refused. A refusal
  is a `TourRefusal{tourId, path, reason}`: it names the runtime tour by its id and the step in its reason, and its path is a
  JSON Pointer into the submitted tour (`/steps/2/target`), never into the app document. `add` refuses an id another runtime tour
  holds; `replace` and `remove` refuse one the registry does not hold, so neither touches an app-document tour. A
  bundle with no app document is refused: a runtime tour's texts are read in the app scope.
- **`list`.** `registerTours(tours, authorizer, dispatcher, registry)` serves the model `morph.tours` with one read
  action, `morph.tours.list`, which answers the runtime tours in registration order as `ListedTour{id, revision,
  tour}`, leaving out a tour whose `screen` or any step's `screen` the caller may not open (`screenAdmitted`): a tour
  is offered whole or not at all. `revision` grows with each `add` and `replace`. There is no write action: who may
  add a tour is the application's policy, and an application that offers an editor exposes its own action and calls
  the registry.
- **Changes.** Every change, a drop included, is told to `setOnChanged`'s handler outside the registry's lock;
  `publishTourChanges(tours, server)` makes it a whole-type change of the record `morph.tours` (`toursChanged()`)
  fanned out from the server's strand to every connection watching it. A server that does not call it sends no event,
  and its clients see runtime tours change at their next frame or reconnect; the first change made while no handler is
  set is logged as a warning, once, so that omission is heard.
- **Bundle reload.** Every call compares the served bundle's digest with the one last checked; when it differs, every
  runtime tour is checked again first, and one that fails is dropped, logged at error with its refusal and told to
  `setOnDropped`'s handler. `recheck()` does it at once, for an application that swaps its bundle.
- **Lifetime.** In memory only. An application that wants its runtime tours to survive a restart stores them and adds
  them again at start-up.
