// Fixture for scripts/check_automoc_includes.sh: verbatim shape of the defect
// -- moc's default include, climbing six levels out of the build tree back to
// the source root. Resolved against every -I entry as well, which is how it
// reaches a same-named header in a different checkout.
#include "../../../../../../examples/ledger/gui_lib/budget_presenter.hpp"
