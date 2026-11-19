// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <core/async/Task.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/file_store.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

namespace {

/// @brief The id `check` checks a document under: as long as a real one, so a document that would just fit once saved
///        fits here too.
[[nodiscard]] std::string placeholderId() { return std::string{kCustomScreenIdPrefix} + std::string(32, '0'); }

/// @brief The most the id rewrite can add to a document: the `,"id":` member with its quotes, the comma and the custom
///        id's `kCustomScreenIdPrefix` and 32 hexadecimal digits. A document longer than the bound plus this cannot
///        fit it once rewritten, so it is refused without being parsed.
inline constexpr std::size_t kCustomScreenIdSlack =
    std::string_view{",\"id\":\"\""}.size() + kCustomScreenIdPrefix.size() + 32;

/// @brief The session of the action being run, copied: the session is the dispatching thread's, and an action
///        resumes elsewhere after its first await.
[[nodiscard]] ::morph::session::Context callerSession() {
    ::morph::session::Context const* const session = ::morph::session::current();
    return session == nullptr ? ::morph::session::Context{} : *session;
}

constexpr std::string_view kNoPerson = "custom screens are stored for a signed-in person";

/// @brief The problem with @p name, if it is empty or overlong.
[[nodiscard]] std::optional<std::string> nameProblem(std::string const& name, std::string_view what) {
    if (name.empty() || name.size() > kMaxNameBytes) {
        return "a custom screen's `" + std::string{what} + "` is 1 to " + std::to_string(kMaxNameBytes) + " bytes";
    }
    return std::nullopt;
}

/// @brief The problem with @p id, if it is not a custom screen's: a name that starts with `custom:`. A request never
///        reaches a store with the id of an application screen.
[[nodiscard]] std::optional<std::string> idProblem(std::string const& screenId) {
    if (auto problem = nameProblem(screenId, "id")) {
        return problem;
    }
    if (!screenId.starts_with(kCustomScreenIdPrefix)) {
        return "a custom screen's `id` starts with `" + std::string{kCustomScreenIdPrefix} + "`";
    }
    return std::nullopt;
}

/// @brief Throws `std::invalid_argument` with @p problem, if there is one.
void refuseProblem(std::optional<std::string> const& problem) {
    if (problem) {
        throw std::invalid_argument{*problem};
    }
}

/// @brief The verified person of @p session, for an action that needs one.
[[nodiscard]] std::string const& requirePerson(::morph::session::Context const& session) {
    if (session.principal.empty()) {
        throw std::runtime_error{std::string{kNoPerson}};
    }
    return session.principal;
}

/// @brief @p text cut to at most @p limit bytes, ending with an ellipsis when it was cut, never inside a UTF-8
///        sequence.
[[nodiscard]] std::string capped(std::string text, std::size_t limit) {
    static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    if (text.size() <= limit) {
        return text;
    }
    std::size_t keep = limit - kEllipsis.size();
    // A continuation byte is 10xxxxxx: step back to the start of the sequence the cut would split.
    while (keep > 0 && (static_cast<unsigned char>(text.at(keep)) & 0xC0U) == 0x80U) {
        --keep;
    }
    text.resize(keep);
    text += kEllipsis;
    return text;
}

[[nodiscard]] std::optional<CheckRefusal> onWire(std::optional<Refusal> const& refusal) {
    if (!refusal) {
        return std::nullopt;
    }
    return CheckRefusal{.pointer = capped(refusal->path, kMaxNameBytes),
                        .reason = capped(refusal->reason, kMaxBrokenReasonBytes)};
}

/// @brief Whether @p document declares no params, read from its JSON.
[[nodiscard]] bool declaresNoParams(std::string_view document) {
    auto const parsed = parseJson(document);
    if (!parsed) {
        return false;
    }
    auto const* const object = std::get_if<Json::object_t>(&parsed->data);
    if (object == nullptr) {
        return false;
    }
    Json::object_t::const_iterator const params = object->find("params");
    if (params == object->end()) {
        return true;
    }
    auto const* const declared = std::get_if<Json::object_t>(&params->second.data);
    return declared != nullptr && declared->empty();
}

/// @brief The screen document @p bundle serves under @p id, or nothing.
[[nodiscard]] ServedScreen const* screenOf(ServedBundle const& bundle, std::string_view screenId) {
    auto const found =
        std::ranges::find(bundle.screens, screenId, [](ServedScreen const& screen) { return screen.entry.id; });
    return found == bundle.screens.end() || found->hash.empty() ? nullptr : &*found;
}

}  // namespace

CustomScreens::CustomScreens(std::shared_ptr<ICustomScreenStore> store,
                             std::function<std::shared_ptr<ServedBundle const>()> bundle,
                             std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                             CustomScreensOptions options)
    : _store{std::move(store)},
      _bundle{std::move(bundle)},
      _authorizer{std::move(authorizer)},
      _options{options},
      _verdicts{options.verdictCacheSize} {}

std::expected<std::shared_ptr<ServedBundle const>, std::string> CustomScreens::servedFor(
    std::string const& app) const {
    if (auto problem = nameProblem(app, "app")) {
        return std::unexpected(std::move(*problem));
    }
    std::shared_ptr<ServedBundle const> bundle = _bundle();
    if (bundle == nullptr) {
        return std::unexpected(std::string{"custom screens are not served: no bundle"});
    }
    if (bundle->application.id != app) {
        return std::unexpected("custom screens belong to `" + bundle->application.id + "`, not `" + app + "`");
    }
    return bundle;
}

std::expected<std::string, Refusal> CustomScreens::rewrittenAndChecked(ServedBundle const& bundle,
                                                                       ::morph::session::Context const& session,
                                                                       std::string_view basedOn,
                                                                       std::string_view document,
                                                                       std::string_view screenId) const {
    // The rewrite parses the document, so a document no bound could admit is refused before it, with the wording the
    // bounds check gives.
    if (document.size() > _options.maxDocumentBytes + kCustomScreenIdSlack) {
        return std::unexpected(Refusal{
            .document = {},
            .path = {},
            .reason = "a custom screen is longer than " + std::to_string(_options.maxDocumentBytes) + " bytes"});
    }
    auto text = withScreenId(document, screenId);
    if (!text) {
        // Text that is not a JSON object, or too deep to parse: the checks give the person the reason they give for it
        // everywhere else.
        if (auto refusal = checkCustomScreen(bundle, session, *_authorizer, basedOn, document, screenId, _options)) {
            return std::unexpected(std::move(*refusal));
        }
        return std::unexpected(std::move(text.error()));
    }
    if (auto refusal =
            checkCustomScreen(bundle, session, *_authorizer, basedOn, *text, screenId, _options, &_verdicts)) {
        return std::unexpected(std::move(*refusal));
    }
    return text;
}

std::optional<Refusal> CustomScreens::checkedAsNew(ServedBundle const& bundle,
                                                   ::morph::session::Context const& session, std::string_view basedOn,
                                                   std::string_view document) const {
    auto const checked = rewrittenAndChecked(bundle, session, basedOn, document, placeholderId());
    if (checked) {
        return std::nullopt;
    }
    return checked.error();
}

::core::async::Task<std::optional<Refusal>> CustomScreens::check(::morph::session::Context const& session,
                                                                 std::string app, std::string basedOn,
                                                                 std::string document) const {
    return checkTask(session, std::move(app), std::move(basedOn), std::move(document));
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the application, the base, then the document.
::core::async::Task<std::optional<Refusal>> CustomScreens::checkTask(::morph::session::Context session,
                                                                     std::string app, std::string basedOn,
                                                                     std::string document) const {
    auto const refusal = [](std::string reason) {
        return std::optional<Refusal>{Refusal{.document = {}, .path = {}, .reason = std::move(reason)}};
    };
    auto const bundle = servedFor(app);
    if (!bundle) {
        co_return refusal(bundle.error());
    }
    if (auto problem = nameProblem(basedOn, "basedOn")) {
        co_return refusal(std::move(*problem));
    }
    if (session.principal.empty()) {
        co_return refusal(std::string{kNoPerson});
    }
    co_return checkedAsNew(**bundle, session, basedOn, document);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

::core::async::Task<std::expected<CustomScreenFound, CustomScreenRefusal>> CustomScreens::get(
    ::morph::session::Context const& session, std::string app, std::string screenId) const {
    return getTask(session, std::move(app), std::move(screenId));
}

::core::async::Task<std::expected<CustomScreenFound, CustomScreenRefusal>> CustomScreens::getTask(
    ::morph::session::Context session, std::string app, std::string screenId) const {
    auto const bundle = servedFor(app);
    if (!bundle) {
        co_return std::unexpected(CustomScreenRefusal{.reason = bundle.error()});
    }
    if (auto problem = idProblem(screenId)) {
        co_return std::unexpected(CustomScreenRefusal{.reason = std::move(*problem)});
    }
    if (session.principal.empty()) {
        co_return std::unexpected(CustomScreenRefusal{.reason = std::string{kNoPerson}});
    }
    auto found = co_await _store->get(CustomScreenKey{.person = session.principal, .app = app, .id = screenId});
    if (!found) {
        co_return std::unexpected(std::move(found.error()));
    }
    // A store answers whatever it holds; the reply must still fit an envelope.
    if (found->document.size() > _options.maxDocumentBytes) {
        co_return std::unexpected(CustomScreenRefusal{.reason = "a custom screen is longer than " +
                                                                std::to_string(_options.maxDocumentBytes) + " bytes"});
    }
    std::vector<std::string> const permitted = permittedActions(**bundle, session, *_authorizer);
    OpensMemo opens;
    CustomScreenSummary summary = summarize(**bundle, session, permitted, *found, opens);
    co_return CustomScreenFound{.summary = std::move(summary), .document = std::move(found->document)};
}

std::expected<std::string, Refusal> CustomScreens::allowance(::morph::session::Context const& session,
                                                             std::string const& app, std::string_view basedOn) const {
    auto const refusal = [](std::string reason) {
        return std::unexpected(Refusal{.document = {}, .path = {}, .reason = std::move(reason)});
    };
    auto const served = servedFor(app);
    if (!served) {
        return refusal(served.error());
    }
    ServedBundle const* const bundle = served->get();
    if (auto problem = nameProblem(std::string{basedOn}, "basedOn")) {
        return refusal(std::move(*problem));
    }
    if (session.principal.empty()) {
        return refusal(std::string{kNoPerson});
    }
    ServedScreen const* const base = screenOf(*bundle, basedOn);
    if (base == nullptr) {
        return refusal("`basedOn` names no screen document of the application: `" + std::string{basedOn} + "`");
    }
    if (!screenAdmitted(*base, session, *_authorizer)) {
        return refusal("this person may not open the base screen `" + std::string{basedOn} + "`");
    }
    std::string const& text = bundle->files.at(base->hash).bytes;
    auto const loaded = loadDocumentText(text, LoadOptions{.catalog = bundle->catalog});
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return describeAllowance(*loaded, text);
}

CustomScreenSummary CustomScreens::summarize(ServedBundle const& bundle, ::morph::session::Context const& session,
                                             std::vector<std::string> const& permitted, CustomScreen const& screen,
                                             OpensMemo& opens) const {
    std::optional<CheckRefusal> broken = onWire(checkCustomScreen(
        bundle, session, *_authorizer, permitted, screen.basedOn, screen.document, screen.id, _options, &_verdicts));
    // A custom screen declares exactly its base's params, so a screen that is not broken opens from the navigator when
    // its base takes none. The base is read once per list, not once per screen.
    bool opensWithoutParams = false;
    if (!broken) {
        auto const known = opens.find(screen.basedOn);
        if (known != opens.end()) {
            opensWithoutParams = known->second;
        } else {
            ServedScreen const* const base = screenOf(bundle, screen.basedOn);
            opensWithoutParams = base != nullptr && declaresNoParams(bundle.files.at(base->hash).bytes);
            opens.emplace(screen.basedOn, opensWithoutParams);
        }
    }
    return CustomScreenSummary{
        .id = capped(screen.id, kMaxNameBytes),
        .title = capped(screen.title, kMaxNameBytes),
        .basedOn = capped(screen.basedOn, kMaxNameBytes),
        .savedAt = screen.savedAt,
        .savedBy = capped(screen.savedBy, kMaxNameBytes),
        .mine = screen.savedBy == session.principal,
        .hash = contentHash(screen.document),
        .opensWithoutParams = opensWithoutParams,
        .broken = std::move(broken),
    };
}

CustomScreensModel::CustomScreensModel(std::shared_ptr<CustomScreens> context) : _context{std::move(context)} {
    if (_context == nullptr) {
        throw std::invalid_argument{"CustomScreensModel: a null context"};
    }
}

namespace {

/// @brief The bundle @p context serves @p app with; an application it does not serve is refused.
[[nodiscard]] std::shared_ptr<ServedBundle const> served(
    std::expected<std::shared_ptr<ServedBundle const>, std::string> bundle) {
    if (!bundle) {
        throw std::invalid_argument{bundle.error()};
    }
    return *std::move(bundle);
}

}  // namespace

::core::async::Task<CustomScreensListed> CustomScreensModel::execute(CustomScreensList request) {
    std::shared_ptr<ServedBundle const> const bundle = served(_context->servedFor(request.app));
    ::morph::session::Context const session = callerSession();
    CustomScreensListed reply;
    if (session.principal.empty()) {
        co_return reply;
    }
    std::vector<CustomScreen> const stored = co_await _context->_store->list(session.principal, request.app);
    // Computed once for the whole list, and never kept past this call: permissions can change between calls.
    std::vector<std::string> const permitted = permittedActions(*bundle, session, *_context->_authorizer);
    reply.truncated = stored.size() > _context->_options.maxListed;
    CustomScreens::OpensMemo opens;
    for (CustomScreen const& screen : stored) {
        if (reply.screens.size() == _context->_options.maxListed) {
            break;
        }
        reply.screens.push_back(_context->summarize(*bundle, session, permitted, screen, opens));
    }
    co_return reply;
}

::core::async::Task<CustomScreenFound> CustomScreensModel::execute(CustomScreensGet request) {
    ::morph::session::Context const session = callerSession();
    auto found = co_await _context->getTask(session, std::move(request.app), std::move(request.id));
    if (!found) {
        throw std::runtime_error{found.error().reason};
    }
    co_return std::move(*found);
}

::core::async::Task<CustomScreenChecked> CustomScreensModel::execute(CustomScreensCheck request) {
    std::shared_ptr<ServedBundle const> const bundle = served(_context->servedFor(request.app));
    refuseProblem(nameProblem(request.basedOn, "basedOn"));
    ::morph::session::Context const session = callerSession();
    static_cast<void>(requirePerson(session));
    co_return CustomScreenChecked{
        .refusal = onWire(_context->checkedAsNew(*bundle, session, request.basedOn, request.document))};
}

::core::async::Task<CustomScreenSaved> CustomScreensModel::execute(CustomScreensSave request) {
    std::shared_ptr<ServedBundle const> const bundle = served(_context->servedFor(request.app));
    refuseProblem(nameProblem(request.title, "title"));
    refuseProblem(nameProblem(request.basedOn, "basedOn"));
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    if (request.id) {
        refuseProblem(idProblem(*request.id));
        auto existing =
            co_await _context->_store->get(CustomScreenKey{.person = person, .app = request.app, .id = *request.id});
        if (!existing) {
            throw std::runtime_error{existing.error().reason};
        }
    }
    std::string const screenId = request.id ? *request.id : newFileRef(kCustomScreenIdPrefix);
    // The id is rewritten first, and the checks run on the text that is stored: every bound applies to it.
    auto text = _context->rewrittenAndChecked(*bundle, session, request.basedOn, request.document, screenId);
    if (!text) {
        throw std::invalid_argument{text.error().message()};
    }
    auto stored = co_await _context->_store->put(CustomScreenKey{.person = person, .app = request.app, .id = screenId},
                                                 CustomScreen{.id = screenId,
                                                              .title = std::move(request.title),
                                                              .basedOn = std::move(request.basedOn),
                                                              .document = std::move(*text),
                                                              .savedAt = time::DateTime::now(),
                                                              .savedBy = person});
    if (!stored) {
        throw std::runtime_error{stored.error().reason};
    }
    co_return CustomScreenSaved{.id = screenId};
}

::core::async::Task<CustomScreenRemoved> CustomScreensModel::execute(CustomScreensRemove request) {
    static_cast<void>(served(_context->servedFor(request.app)));
    refuseProblem(idProblem(request.id));
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    auto removed =
        co_await _context->_store->remove(CustomScreenKey{.person = person, .app = request.app, .id = request.id});
    if (!removed) {
        throw std::runtime_error{removed.error().reason};
    }
    co_return CustomScreenRemoved{.removed = true};
}

std::shared_ptr<CustomScreens> registerCustomScreens(std::shared_ptr<ICustomScreenStore> store,
                                                     std::function<std::shared_ptr<ServedBundle const>()> bundle,
                                                     std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                                                     model::detail::ActionDispatcher& dispatcher,
                                                     model::detail::ModelRegistryFactory& registry,
                                                     CustomScreensOptions options) {
    if (store == nullptr || !bundle || authorizer == nullptr) {
        throw std::invalid_argument{"registerCustomScreens: a null store, bundle or authorizer"};
    }
    if (options.maxDocumentBytes > kMaxFileBytes) {
        throw std::invalid_argument{
            "registerCustomScreens: maxDocumentBytes is at most kMaxFileBytes, so every "
            "stored screen can be served"};
    }
    if (options.maxDocumentBytes > (::morph::wire::kMaxEnvelopeBytes - kCustomScreenReplyOverhead) / 4) {
        throw std::invalid_argument{
            "registerCustomScreens: maxDocumentBytes is too large for a `get` reply to fit one envelope once "
            "escaped"};
    }
    if (options.maxListed >
        (::morph::wire::kMaxEnvelopeBytes - kCustomScreenReplyOverhead) / (kMaxEscapedGrowth * kMaxSummaryBytes)) {
        throw std::invalid_argument{
            "registerCustomScreens: maxListed is too large for a `list` reply to fit one envelope "
            "once escaped"};
    }
    auto context =
        std::make_shared<CustomScreens>(std::move(store), std::move(bundle), std::move(authorizer), options);
    registry.registerModel<CustomScreensModel>(kCustomScreensModel, [context] {
        return std::make_unique<model::detail::ModelHolder<CustomScreensModel>>(context);
    });
    dispatcher.registerAction<CustomScreensModel, CustomScreensList>(kCustomScreensModel, kCustomScreensList);
    dispatcher.registerAction<CustomScreensModel, CustomScreensGet>(kCustomScreensModel, kCustomScreensGet);
    dispatcher.registerAction<CustomScreensModel, CustomScreensCheck>(kCustomScreensModel, kCustomScreensCheck);
    dispatcher.registerAction<CustomScreensModel, CustomScreensSave>(kCustomScreensModel, kCustomScreensSave);
    dispatcher.registerAction<CustomScreensModel, CustomScreensRemove>(kCustomScreensModel, kCustomScreensRemove);
    return context;
}

}  // namespace morph::ui

// The process-level registries resolve once, at their first read, so the record, the result slots that let a list
// be found holding it, and the effects of save and remove are registered here, at static initialisation, in the
// translation unit a server links by calling registerCustomScreens.
BRIDGE_REGISTER_RECORD(morph::ui::CustomScreenSummary, "morph.customScreens", ::morph::record::privateKeys)
BRIDGE_REGISTER_EFFECTS(morph::ui::CustomScreensSave, morph::effects::changes<morph::ui::CustomScreenSummary>)
BRIDGE_REGISTER_EFFECTS(morph::ui::CustomScreensRemove, morph::effects::changes<morph::ui::CustomScreenSummary>)
#ifndef MORPH_CLIENT_ONLY
namespace {
// NOLINTNEXTLINE(cert-err58-cpp): the registrars are noexcept.
[[maybe_unused]] bool const customScreenResults =
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensList>(morph::ui::kCustomScreensList) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensGet>(morph::ui::kCustomScreensGet) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensCheck>(morph::ui::kCustomScreensCheck) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensSave>(morph::ui::kCustomScreensSave) &&
    morph::model::detail::registerActionResultOnce<morph::ui::CustomScreensRemove>(morph::ui::kCustomScreensRemove);
}  // namespace
#endif
