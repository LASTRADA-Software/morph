// SPDX-License-Identifier: Apache-2.0

#include "lims_screens.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lims/dto/analysis_dto.hpp"
#include "lims/dto/auth_dto.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
#include "lims/dto/sample_entry_dto.hpp"
#include "lims/dto/verification_dto.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/auth_model.hpp"
#include "lims/models/sample_model.hpp"

namespace lims::screens {

namespace ui = morph::ui::author;

namespace {

// Registration: a client, then a sample for it; the new sample's lifecycle opens in its own tab.
ui::Screen registerScreen() {
    return ui::screen(std::string{kRegister}, [](ui::Scope& screen) {
        screen.title("Register a sample");
        auto const lab = screen.model<SampleModel>("lab", {.instance = ui::Instance::Private});
        auto const registerClient = screen.mutation<RegisterClient>("registerClient", lab);
        auto const registerSample = screen.mutation<RegisterSample>("registerSample", lab);
        auto const client = screen.let("client", registerClient.result());
        auto const registered = screen.let("registered", registerSample.result());
        // A registered sample opens in its own lifecycle screen.
        screen.watch(registered,
                     ui::ifThen(ui::engaged(registered),
                                ui::navigate(std::string{kLifecycle},
                                             {.params = ui::obj({{"sampleId", registered.at(&SampleView::id)}})})));
        auto const clientName = ui::element("clientName");
        auto const reference = ui::element("reference");
        return ui::column(
            {ui::panel(
                 {ui::textInput({.label = "Client name", .placeholder = "Client"}).id(clientName).testId("clientName"),
                  ui::button({.label = "Register client",
                              .onClick = ui::run(registerClient, {{&RegisterClient::name, clientName.value()}})})
                      .testId("registerClient"),
                  ui::text({.text = ui::format("Client {0}", {client.at(&RegisterClientResult::clientId)})})
                      .testId("clientId")
                      .visible(ui::engaged(client)),
                  ui::text({.text = registerClient.error()}).testId("clientError")},
                 {.title = "Client", .padding = 2}),
             ui::panel(
                 {ui::textInput({.label = "Reference", .placeholder = "Sample reference"})
                      .id(reference)
                      .testId("reference"),
                  ui::button({.label = "Register sample",
                              .onClick = ui::run(registerSample, {{&RegisterSample::clientId,
                                                                   client.at(&RegisterClientResult::clientId)},
                                                                  {&RegisterSample::reference, reference.value()}})})
                      .testId("registerSample")
                      .enabled(ui::engaged(client)),
                  ui::text({.text = registerSample.error()}).testId("sampleError")},
                 {.title = "Sample", .padding = 2})},
            {.gap = 3});
    });
}

// One sample's lifecycle. The sample model is keyed by the sample id, so every screen showing this sample shares
// its instance; opening attaches it, and every transition answers the sample as it now is.
ui::Screen lifecycleScreen() {
    return ui::screen(std::string{kLifecycle}, [](ui::Scope& screen) {
        auto const sampleId = screen.param<std::optional<std::int64_t>>("sampleId");
        auto const sample = screen.model<SampleModel>("sample", {.instance = sampleId});
        auto const opened = screen.state("opened", false);
        auto const open = screen.mutation<OpenSample>(
            "open", sample, {.body = {{&OpenSample::sampleId, sampleId}}, .onSuccess = {ui::set(opened, true)}});
        auto const transition = [&](auto action, std::string const& name) {
            return screen.mutation<decltype(action)>(name, sample, {.concurrency = ui::Mode::Serial});
        };
        auto const receive = transition(ReceiveSample{}, "receive");
        auto const start = transition(StartWork{}, "start");
        auto const submit = transition(SubmitForVerification{}, "submit");
        auto const publish = transition(PublishSample{}, "publish");
        // The sample as it is now: fetched once the screen attached it; every transition's reply updates it.
        auto const current = screen.query<GetSample>("current", sample, {}, {.when = opened});
        auto const view = current.value();

        screen.title(ui::format("Sample {0}", {sampleId}));
        screen.identity(sampleId);
        // Without a sample there is nothing to attach: the screen waits to be opened on one.
        screen.onMount(ui::ifThen(ui::engaged(sampleId), ui::run(open)));

        auto const state = view.at(&SampleView::state);
        auto const failure = ui::coalesce(
            {receive.error(), start.error(), submit.error(), publish.error(), open.error(), current.error()});
        return ui::column(
            {ui::text({.text = ui::format("sample {0} — {1} (v{2})",
                                          {view.at(&SampleView::id), state, view.at(&SampleView::version)})})
                 .testId("header")
                 .visible(ui::engaged(view)),
             ui::row({ui::button({.label = "Receive", .onClick = ui::run(receive)})
                          .testId("receive")
                          .enabled(ui::eq(state, "Registered")),
                      ui::button({.label = "Start work", .onClick = ui::run(start)})
                          .testId("start")
                          .enabled(ui::eq(state, "Received")),
                      ui::button({.label = "Submit for verification", .onClick = ui::run(submit)})
                          .testId("submit")
                          .enabled(ui::eq(state, "InProgress")),
                      ui::button({.label = "Publish", .onClick = ui::run(publish)})
                          .testId("publish")
                          .enabled(ui::eq(state, "ToBeVerified")),
                      ui::button({.label = "Refresh", .onClick = ui::refetch(current)}).testId("refresh"),
                      ui::button({.label = "Results",
                                  .onClick = ui::navigate(std::string{kResults},
                                                          {.params = ui::obj({{"sampleId", sampleId}})})})
                          .testId("results")},
                     {.gap = 1}),
             ui::text({.text = failure}).testId("error")},
            {.gap = 2});
    });
}

// Result entry for one sample: the capture form over the sample's own instance, the results it has, verification
// per result, and what an offline replay could not apply.
ui::Screen resultsScreen() {
    return ui::screen(std::string{kResults}, [](ui::Scope& screen) {
        auto const sampleId = screen.param<std::optional<std::int64_t>>("sampleId");
        auto const sample = screen.model<SampleModel>("sample", {.instance = sampleId});
        auto const catalog = screen.model<AnalysisCatalogModel>("catalog");
        auto const opened = screen.state("opened", false);
        auto const open = screen.mutation<OpenSample>(
            "open", sample, {.body = {{&OpenSample::sampleId, sampleId}}, .onSuccess = {ui::set(opened, true)}});
        auto const verify = screen.mutation<VerifyResult>("verify", sample, {.concurrency = ui::Mode::Serial});
        auto const results = screen.query<ListResults>("results", sample, {}, {.when = opened});
        auto const conflicts = screen.query<ListConflicts>("conflicts", sample, {}, {.when = opened});
        auto const analyses = screen.query<ListAnalyses>("analyses", catalog);
        // The capture's reported concentration while it is typed: the server evaluates the form's fields as the
        // action it will run, without running it.
        auto const capture = ui::element("capture");
        auto const field = [&capture](std::string const& name) { return capture.get(name + ".value"); };
        auto const reported = screen.evaluate<CaptureConcentration>(
            "reported", sample,
            {{&CaptureConcentration::analysisVersionId, field("analysisVersionId")},
             {&CaptureConcentration::value, field("value")},
             {&CaptureConcentration::qualifier, field("qualifier")},
             {&CaptureConcentration::dilution, field("dilution")},
             {&CaptureConcentration::dilutionFactor, field("dilutionFactor")}},
            {.when = opened, .debounce = std::chrono::milliseconds{300}});

        screen.title(ui::format("Results {0}", {sampleId}));
        screen.identity(sampleId);
        // Without a sample there is nothing to attach: the screen waits to be opened on one.
        screen.onMount(ui::ifThen(ui::engaged(sampleId), ui::run(open)));

        return ui::column(
            {ui::text(
                 {.text = ui::format("{0} analyses", {ui::count(analyses.value().at(&ListAnalysesResult::analyses))})})
                 .testId("analyses"),
             ui::form<CaptureConcentration>(sample,
                                            {
                                                .fields = {{&CaptureConcentration::reportedValue, {.omit = true}}},
                                            })
                 .id(capture)
                 .testId("capture"),
             ui::text({.text = ui::format("Reported {0} mg/L",
                                          {reported.value().at(&CaptureConcentration::reportedValue)})})
                 .stale(reported.pending())
                 .testId("reported"),
             ui::table(
                 results.value().at(&ListResultsResult::results), {.key = ui::row(&ResultView::id)},
                 [&](ui::DeclScope& row) {
                     auto const recapture =
                         row.mutation<CaptureConcentration>("recapture", sample, {.concurrency = ui::Mode::Serial});
                     // At the places typed: the model refuses a reading finer than its version declares, and
                     // padding the text would claim precision nobody measured.
                     auto const recaptureBody = ui::run(
                         recapture,
                         {
                             {&CaptureConcentration::analysisVersionId, ui::row(&ResultView::analysisVersionId)},
                             {&CaptureConcentration::value, ui::parseDecimal(ui::event())},
                         });
                     return std::vector<ui::TableColumn>{
                         {
                             .name = "analysis",
                             .label = "Analysis",
                             .cell = ui::text({.text = ui::row("analysisVersionId")}),
                         },
                         {.name = "value", .label = "Value", .cell = ui::text({.text = ui::row("value")})},
                         {
                             .name = "recapture",
                             .label = "Re-capture",
                             .cell = ui::textInput({
                                                       .label = "Re-capture value",
                                                       .onSubmit = recaptureBody,
                                                   })
                                         .testId("recapture"),
                         },
                         {
                             .name = "outOfSpec",
                             .label = "Out of spec",
                             .cell = ui::text({.text = ui::row("outOfSpec")}),
                         },
                         {
                             .name = "verify",
                             .label = "",
                             .cell = ui::button({
                                 .label = "Verify",
                                 .onClick = ui::run(verify, {{&VerifyResult::resultId, ui::row(&ResultView::id)}}),
                             }),
                         },
                     };
                 })
                 .a11y("Results")
                 .testId("results"),
             ui::text({.text = verify.error()}).testId("verifyError"),
             ui::text({.text = ui::format("{0} conflicts",
                                          {ui::count(conflicts.value().at(&ListConflictsResult::conflicts))})})
                 .testId("conflicts")},
            {.gap = 2});
    });
}

// The message for a refused save, one catalogue entry per reason.
ui::Handle<std::string> saveRefusal(ui::AnyHandle const& error) {
    auto const reason = [&](std::string const& name, std::string const& fallback,
                            ui::Handle<std::string> const& otherwise) {
        return ui::cond(ui::eq(error, name), ui::t("lims.save." + name, fallback), otherwise);
    };
    return reason(
        "noClient", "Choose a client before saving",
        reason("noReference", "Enter a reference before saving",
               reason("noProject", "Choose a project before saving", ui::t("lims.save.notSaved", "Not saved"))));
}

// Sample entry: a record-editing screen over a private working copy of one sample. Each section is a form whose
// submit edits the working copy; Save commits it. The dilution series and its limits are the model's, shown through
// two host components with built-in fallbacks.
ui::Screen sampleScreen() {
    return ui::screen(std::string{kSample}, [](ui::Scope& screen) {
        auto const sampleId = screen.param<std::optional<std::int64_t>>("sampleId");
        auto const sample = screen.model<SampleModel>("sample", {.instance = ui::Instance::Private});
        auto const ready = screen.state("ready", false);
        auto const unsaved = screen.state("unsaved", false);
        auto const showTurbidity = screen.state("showTurbidity", false);
        auto const showNitrate = screen.state("showNitrate", false);
        auto const project = ui::element("project");

        auto const header = screen.query<GetHeader>("header", sample, {}, {.when = ready});
        auto const series = screen.query<GetDilutionSeries>("series", sample, {}, {.when = ready});
        auto const measure = screen.query<GetMeasurements>("measure", sample, {}, {.when = ready});
        auto const projects = screen.query<ListProjects>("projects", sample);

        // Every action here, the section forms' submits included, edits the working copy the queries read, and says
        // so (`changesInstance`), so the queries refetch after each one.
        auto const load = screen.mutation<LoadSample>(
            "load", sample,
            {.body = {{&LoadSample::id, sampleId}}, .onSuccess = {ui::set(ready, true), ui::set(unsaved, false)}});
        auto const make =
            screen.mutation<NewSample>("make", sample, {.onSuccess = {ui::set(ready, true), ui::set(unsaved, false)}});
        auto const clear =
            screen.mutation<ClearOptionalMeasurement>("clear", sample, {.onSuccess = {ui::set(unsaved, true)}});
        auto const pick = screen.mutation<SelectProject>("pick", sample,
                                                         {.onSuccess = {ui::set(unsaved, true), ui::close(project)}});
        auto const save = screen.mutation<SaveSample>("save", sample);
        auto const saved = screen.let("saved", save.result());
        auto const remove = screen.mutation<DeleteSample>("remove", sample, {.onSuccess = {ui::run(make)}});
        auto const saveError = saved.at(&SaveSampleResult::error);
        // A refused save is a reply, not a failure: only `none` means the working copy is stored.
        screen.watch(saved, ui::ifThen(ui::eq(saveError, "none"), ui::set(unsaved, false)));

        auto const view = header.value();
        screen.identity(sampleId);
        auto const title = ui::coalesce({view.at(&HeaderView::sampleNumber), ui::t("lims.sample.new", "New sample")});
        screen.title(title);
        screen.dirty(unsaved);
        screen.onMount(ui::ifThen(ui::engaged(sampleId), ui::run(load), ui::run(make)));
        screen.onBackendChange(
            ui::seq({ui::ifThen(unsaved, ui::notify(ui::t("lims.lost", "Unsaved edits were lost"), ui::Role::Warning)),
                     ui::ifThen(ui::engaged(sampleId), ui::run(load), ui::run(make))}));

        auto const rowsOf = series.value().at(&DilutionSeries::rows);
        auto const seriesGrid = screen.component(
            "SeriesGrid",
            {.host = "SeriesGrid",
             .version = 1,
             .props = {{"rows", ui::PropType::Json}},
             .fallback =
                 ui::table(
                     ui::prop("rows"),
                     {.key = ui::row("step"),
                      .columns =
                          {{.name = "step", .label = "Step", .cell = ui::text({.text = ui::row("step")})},
                           {.name = "nominal", .label = "Nominal", .cell = ui::text({.text = ui::row("nominal")})},
                           {.name = "measured", .label = "Measured", .cell = ui::text({.text = ui::row("measured")})},
                           {.name = "recovery",
                            .label = "Recovery %",
                            .cell = ui::text({.text = ui::row("recoveryPct")})}}})
                     .a11y("Dilution series")
                     .testId("seriesFallback")});
        auto const recoveryCurve = screen.component(
            "RecoveryCurve",
            {.host = "RecoveryCurve",
             .version = 1,
             .props = {{"rows", ui::PropType::Json}},
             .fallback = ui::text({.text = ui::format("Recovery curve over {0} steps", {ui::count(ui::prop("rows"))})})
                             .testId("curveFallback")});

        auto const optionalToggle = [&](std::string_view which, std::string const& label,
                                        ui::Handle<bool> const& shown) {
            return ui::row(
                {ui::button({.label = "Add " + label, .onClick = ui::set(shown, true)})
                     .testId("add-" + std::string{which})
                     .visible(!shown),
                 ui::button({.label = "Remove " + label,
                             .onClick = {ui::run(clear, {{&ClearOptionalMeasurement::which, std::string{which}}}),
                                         ui::set(shown, false)}})
                     .testId("remove-" + std::string{which})
                     .visible(shown)},
                {.gap = 1});
        };
        auto const turbidityShown = showTurbidity || measure.value().at(&MeasurementsView::hasTurbidity);
        auto const nitrateShown = showNitrate || measure.value().at(&MeasurementsView::hasNitrate);

        return ui::column(
            {ui::row({ui::text({.text = title}).testId("title"),
                      ui::button({.label = ui::t("lims.project", "Project"), .onClick = ui::open(project)})
                          .testId("chooseProject"),
                      ui::button({.label = ui::t("lims.save", "Save"), .onClick = ui::run(save)}).testId("save"),
                      ui::button({.label = ui::t("lims.delete", "Delete"), .onClick = ui::run(remove)})
                          .testId("delete")
                          .enabled(ui::engaged(view.at(&HeaderView::sampleId)))},
                     {.gap = 1}),
             ui::text({.text = saveRefusal(saveError)})
                 .testId("saveRefusal")
                 .visible(ui::engaged(saved) && ui::ne(saveError, "none")),
             ui::text({.text = ui::coalesce({load.error(), make.error(), save.error(), remove.error(), pick.error()})})
                 .testId("error"),
             ui::panel({ui::text({.text = ui::format("Project: {0}", {view.at(&HeaderView::projectName)})})
                            .testId("projectName"),
                        ui::text({.text = ui::format("Client {0}", {view.at(&HeaderView::clientId)})})
                            .testId("client")
                            .visible(ui::engaged(view)),
                        ui::form<HeaderSection>(
                            sample, {.prefill = view,
                                     .fields = {{&HeaderSection::receivedOn,
                                                 {.label = ui::t("lims.receivedOn", "Received"),
                                                  .widget = std::string{"epochDays"}}},
                                                {&HeaderSection::remarks, {.blankAs = std::string{"empty"}}}},
                                     .onSuccess = {ui::set(unsaved, true)}})
                            .id(ui::element("headerForm"))
                            .testId("headerForm")},
                       {.title = ui::t("lims.header", "Header"), .padding = 2}),
             ui::panel({ui::form<SamplingSection>(sample, {.onSuccess = {ui::set(unsaved, true)}})
                            .id(ui::element("samplingForm"))
                            .testId("samplingForm"),
                        ui::form<TurbiditySection>(sample, {.onSuccess = {ui::set(unsaved, true)}})
                            .id(ui::element("turbidityForm"))
                            .testId("turbidityForm")
                            .visible(turbidityShown),
                        ui::form<NitrateSection>(sample, {.onSuccess = {ui::set(unsaved, true)}})
                            .id(ui::element("nitrateForm"))
                            .testId("nitrateForm")
                            .visible(nitrateShown),
                        optionalToggle(kTurbidity, "turbidity", showTurbidity),
                        optionalToggle(kNitrate, "nitrate", showNitrate)},
                       {.title = ui::t("lims.measurements", "Measurements"), .padding = 2}),
             ui::panel(
                 {ui::custom(seriesGrid, {.props = {{"rows", rowsOf}}}).a11y("Dilution series").testId("seriesGrid"),
                  ui::custom(recoveryCurve, {.props = {{"rows", rowsOf}}})
                      .a11y("Recovery curve")
                      .testId("recoveryCurve"),
                  ui::text(
                      {.text = ui::t("lims.outside", "{$n} outside limits",
                                     {{"n", ui::coalesce({ui::count(ui::filter(rowsOf, &SeriesRow::outsideLimits)),
                                                          std::int64_t{0}})}})})
                      .testId("outside")},
                 {.title = ui::t("lims.series", "Dilution series"), .padding = 2}),
             ui::dialog(
                 project,
                 {ui::table(
                      projects.value().at(&morph::table::Page<ProjectRow>::rows),
                      {.key = ui::row(&ProjectRow::id),
                       .columns = {{.name = "code", .label = "Code", .cell = ui::text({.text = ui::row("code")})},
                                   {.name = "name", .label = "Name", .cell = ui::text({.text = ui::row("name")})}},
                       .selectionMode = ui::Selection::Single,
                       .onActivate = {ui::run(pick, {{&SelectProject::projectId, ui::row(&ProjectRow::id)}})}})
                      .a11y("Projects")
                      .testId("projects")},
                 {.title = ui::t("lims.chooseProject", "Choose project")})},
            {.gap = 2});
    });
}

// Sign-in: the only screen served before it, so it holds no query. A successful login installs the principal it
// answered and opens sample registration.
ui::Screen loginScreen() {
    return ui::screen(std::string{kLogin}, [](ui::Scope& screen) {
        screen.title("Sign in");
        auto const auth = screen.model<LimsAuthModel>("auth", {.instance = ui::Instance::Private});
        auto const username = ui::element("username");
        auto const login = screen.mutation<LimsLogin>(
            "login", auth,
            {.body = {{&LimsLogin::username, username.value()}},
             .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::navigate(std::string{kRegister})}});
        // A card centred on the page, which shrinks with a narrow window, as the other applications' sign-ins are.
        return ui::column(
                   {ui::panel(
                        {ui::column({ui::text({.text = "Morph LIMS", .role = std::string{"heading"}}).testId("brand"),
                                     ui::textInput({.label = "Username", .placeholder = "Username"})
                                         .id(username)
                                         .testId("username"),
                                     ui::button({.label = "Sign in",
                                                 .emphasis = morph::ui::ButtonEmphasis::Primary,
                                                 .onClick = ui::run(login)})
                                         .testId("signIn"),
                                     ui::text({.text = login.error(), .role = std::string{"error"}}).testId("error")},
                                    {.gap = 3})},
                        {.padding = 5})
                        .sizing(ui::stretch())
                        .maxWidth(44)},
                   {.align = morph::ui::Align::Center, .justify = morph::ui::Justify::Center})
            .sizing(ui::stretch(), ui::stretch());
    });
}

}  // namespace

void prepareLocal() {
    // A development secret: local mode signs nothing that leaves the process.
    auth::setTokenIssuer(
        std::make_shared<morph::session::TokenIssuer>("lims-local-development", morph::session::hmacSha256));
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(registerScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back(
        {.id = std::string{kRegister}, .title = "Register a sample", .registered = screens.add(registerScreen())});
    listed.push_back(
        {.id = std::string{kLifecycle}, .title = "Lifecycle", .registered = screens.add(lifecycleScreen())});
    listed.push_back({.id = std::string{kResults}, .title = "Results", .registered = screens.add(resultsScreen())});
    listed.push_back({.id = std::string{kSample}, .title = "Sample", .registered = screens.add(sampleScreen())});
    listed.push_back({.id = std::string{kLogin}, .title = "Sign in", .registered = screens.add(loginScreen())});
    for (Listed const& screen : listed) {
        if (!screen.registered) {
            return std::unexpected(screen.registered.error());
        }
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    for (Listed const& screen : listed) {
        morph::ui::CatalogEntry entry;
        entry.id = screen.id;
        entry.title = screen.title;
        entry.isPublic = screen.id == kLogin;
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace lims::screens
