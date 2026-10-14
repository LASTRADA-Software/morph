// SPDX-License-Identifier: Apache-2.0

#include "lims_screens.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>
#include <vector>

#include "lims/dto/analysis_dto.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
#include "lims/dto/verification_dto.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"

namespace lims::screens {

namespace ui = morph::ui::author;

namespace {

// Registration: a client, then a sample for it; the new sample's lifecycle opens in its own tab.
ui::Screen registerScreen() {
    return ui::screen(std::string{kRegister}, [](ui::Scope& screen) {
        screen.title("Register a sample");
        auto lab = screen.model<SampleModel>("lab", {.instance = ui::Instance::Private});
        auto registerClient = screen.mutation<RegisterClient>("registerClient", lab);
        auto registerSample = screen.mutation<RegisterSample>("registerSample", lab);
        auto client = screen.let("client", registerClient.result());
        auto registered = screen.let("registered", registerSample.result());
        // A registered sample opens in its own lifecycle screen.
        screen.watch(registerSample.result(),
                     ui::ifThen(ui::engaged(registerSample.result()),
                                ui::navigate(std::string{kLifecycle},
                                             {.params = ui::obj({{"sampleId", registered.at(&SampleView::id)}})})));
        auto clientName = ui::element("clientName");
        auto reference = ui::element("reference");
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
                 {.title = "Client", .padding = 8}),
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
                 {.title = "Sample", .padding = 8})},
            {.gap = 12});
    });
}

// One sample's lifecycle. The sample model is keyed by the sample id, so every screen showing this sample shares
// its instance; opening attaches it, and every transition answers the sample as it now is.
ui::Screen lifecycleScreen() {
    return ui::screen(std::string{kLifecycle}, [](ui::Scope& screen) {
        auto sampleId = screen.param<std::int64_t>("sampleId");
        auto sample = screen.model<SampleModel>("sample", {.instance = sampleId});
        auto opened = screen.state("opened", false);
        auto open = screen.mutation<OpenSample>(
            "open", sample, {.body = {{&OpenSample::sampleId, sampleId}}, .onSuccess = {ui::set(opened, true)}});
        auto transition = [&](auto action, std::string const& name) {
            return screen.mutation<decltype(action)>(name, sample, {.concurrency = ui::Mode::Serial});
        };
        auto receive = transition(ReceiveSample{}, "receive");
        auto start = transition(StartWork{}, "start");
        auto submit = transition(SubmitForVerification{}, "submit");
        auto publish = transition(PublishSample{}, "publish");
        // The sample as it is now: fetched once the screen attached it, and again after every transition.
        auto current = screen.query<GetSample>(
            "current", sample, {},
            {.when = opened,
             .refreshOn = sample.actions<ReceiveSample, StartWork, SubmitForVerification, PublishSample>()});
        auto view = current.value();

        screen.title(ui::format("Sample {0}", {sampleId}));
        screen.identity(sampleId);
        screen.onMount(ui::run(open));

        auto state = view.at(&SampleView::state);
        auto failure = ui::coalesce(
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
                     {.gap = 4}),
             ui::text({.text = failure}).testId("error")},
            {.gap = 8});
    });
}

// Result entry for one sample: the capture form over the sample's own instance, the results it has, verification
// per result, and what an offline replay could not apply.
ui::Screen resultsScreen() {
    return ui::screen(std::string{kResults}, [](ui::Scope& screen) {
        auto sampleId = screen.param<std::int64_t>("sampleId");
        auto sample = screen.model<SampleModel>("sample", {.instance = sampleId});
        auto catalog = screen.model<AnalysisCatalogModel>("catalog");
        auto opened = screen.state("opened", false);
        auto open = screen.mutation<OpenSample>(
            "open", sample, {.body = {{&OpenSample::sampleId, sampleId}}, .onSuccess = {ui::set(opened, true)}});
        auto verify = screen.mutation<VerifyResult>("verify", sample, {.concurrency = ui::Mode::Serial});
        auto results = screen.query<ListResults>(
            "results", sample, {},
            {.when = opened, .refreshOn = sample.actions<CaptureConcentration, VerifyResult>()});
        auto conflicts = screen.query<ListConflicts>("conflicts", sample, {}, {.when = opened});
        auto analyses = screen.query<ListAnalyses>("analyses", catalog);

        screen.title(ui::format("Results {0}", {sampleId}));
        screen.identity(sampleId);
        screen.onMount(ui::run(open));

        return ui::column(
            {ui::text(
                 {.text = ui::format("{0} analyses", {ui::count(analyses.value().at(&ListAnalysesResult::analyses))})})
                 .testId("analyses"),
             ui::form<CaptureConcentration>(sample, {.submit = "Capture"})
                 .id(ui::element("capture"))
                 .testId("capture"),
             ui::table(results.value().at(&ListResultsResult::results),
                       {.key = ui::row(&ResultView::id),
                        .columns = {{.name = "analysis",
                                     .label = "Analysis",
                                     .cell = ui::text({.text = ui::row("analysisVersionId")})},
                                    {.name = "value", .label = "Value", .cell = ui::text({.text = ui::row("value")})},
                                    {.name = "outOfSpec",
                                     .label = "Out of spec",
                                     .cell = ui::text({.text = ui::row("outOfSpec")})},
                                    {.name = "verify",
                                     .label = "",
                                     .cell = ui::button({.label = "Verify",
                                                         .onClick = ui::run(verify, {{&VerifyResult::resultId,
                                                                                      ui::row(&ResultView::id)}})})}}})
                 .a11y("Results")
                 .testId("results"),
             ui::text({.text = verify.error()}).testId("verifyError"),
             ui::text({.text = ui::format("{0} conflicts",
                                          {ui::count(conflicts.value().at(&ListConflictsResult::conflicts))})})
                 .testId("conflicts")},
            {.gap = 8});
    });
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(registerScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({std::string{kRegister}, "Register a sample", screens.add(registerScreen())});
    listed.push_back({std::string{kLifecycle}, "Lifecycle", screens.add(lifecycleScreen())});
    listed.push_back({std::string{kResults}, "Results", screens.add(resultsScreen())});
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
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace lims::screens
