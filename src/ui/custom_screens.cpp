// SPDX-License-Identifier: Apache-2.0

#include <core/async/Task.hpp>
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
namespace detail {

/// @brief What every custom screens instance shares.
struct CustomScreensContext {
    CustomScreensContext(std::shared_ptr<ICustomScreenStore> keptStore,
                         std::function<std::shared_ptr<ServedBundle const>()> currentBundle,
                         std::shared_ptr<::morph::session::IAuthorizer const> keptAuthorizer,
                         CustomScreensOptions const& bounds)
        : store{std::move(keptStore)},
          bundle{std::move(currentBundle)},
          authorizer{std::move(keptAuthorizer)},
          options{bounds},
          verdicts{bounds.verdictCacheSize} {}

    std::shared_ptr<ICustomScreenStore> store;
    std::function<std::shared_ptr<ServedBundle const>()> bundle;
    std::shared_ptr<::morph::session::IAuthorizer const> authorizer;
    CustomScreensOptions options;
    VerdictCache verdicts;
};

}  // namespace detail

namespace {

/// @brief The session of the action being run, copied: the session is the dispatching thread's, and an action
///        resumes elsewhere after its first await.
[[nodiscard]] ::morph::session::Context callerSession() {
    ::morph::session::Context const* const session = ::morph::session::current();
    return session == nullptr ? ::morph::session::Context{} : *session;
}

/// @brief The verified person of @p session, for an action that needs one.
[[nodiscard]] std::string const& requirePerson(::morph::session::Context const& session) {
    if (session.principal.empty()) {
        throw std::runtime_error{"custom screens are stored for a signed-in person"};
    }
    return session.principal;
}

/// @brief Refuses an empty or overlong name.
void checkName(std::string const& name, std::string_view what) {
    if (name.empty() || name.size() > kMaxNameBytes) {
        throw std::invalid_argument{"a custom screen's `" + std::string{what} + "` is 1 to " +
                                    std::to_string(kMaxNameBytes) + " bytes"};
    }
}

/// @brief The bundle being served now, once @p app is the application it serves: custom screens belong to it alone.
[[nodiscard]] std::shared_ptr<ServedBundle const> servedFor(detail::CustomScreensContext const& context,
                                                            std::string const& app) {
    checkName(app, "app");
    std::shared_ptr<ServedBundle const> bundle = context.bundle();
    if (bundle == nullptr || bundle->application.id != app) {
        throw std::invalid_argument{"custom screens belong to `" +
                                    (bundle == nullptr ? std::string{} : bundle->application.id) + "`, not `" + app +
                                    "`"};
    }
    return bundle;
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
    while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xC0U) == 0x80U) {
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
    auto const params = object->find("params");
    if (params == object->end()) {
        return true;
    }
    auto const* const declared = std::get_if<Json::object_t>(&params->second.data);
    return declared != nullptr && declared->empty();
}

}  // namespace

CustomScreensModel::CustomScreensModel(std::shared_ptr<detail::CustomScreensContext> context)
    : _context{std::move(context)} {
    if (_context == nullptr) {
        throw std::invalid_argument{"CustomScreensModel: a null context"};
    }
}

namespace {

/// @brief One screen as a list shows it, checked today against @p permitted, which the caller computed once.
[[nodiscard]] CustomScreenSummary summarize(detail::CustomScreensContext& context, ServedBundle const& bundle,
                                            ::morph::session::Context const& session,
                                            std::vector<std::string> const& permitted, CustomScreen const& screen) {
    return CustomScreenSummary{
        .id = capped(screen.id, kMaxNameBytes),
        .title = capped(screen.title, kMaxNameBytes),
        .basedOn = capped(screen.basedOn, kMaxNameBytes),
        .savedAt = screen.savedAt,
        .savedBy = capped(screen.savedBy, kMaxNameBytes),
        .mine = screen.savedBy == session.principal,
        .opensWithoutParams = declaresNoParams(screen.document),
        .broken = onWire(checkCustomScreen(bundle, session, *context.authorizer, permitted, screen.basedOn,
                                           screen.document, context.options, &context.verdicts)),
    };
}

}  // namespace

::core::async::Task<CustomScreensListed> CustomScreensModel::execute(CustomScreensList request) {
    std::shared_ptr<ServedBundle const> const bundle = servedFor(*_context, request.app);
    ::morph::session::Context const session = callerSession();
    CustomScreensListed reply;
    if (session.principal.empty()) {
        co_return reply;
    }
    std::vector<CustomScreen> const stored = co_await _context->store->list(session.principal, request.app);
    // Computed once for the whole list, and never kept past this call: permissions can change between calls.
    std::vector<std::string> const permitted = permittedActions(*bundle, session, *_context->authorizer);
    reply.truncated = stored.size() > _context->options.maxListed;
    for (CustomScreen const& screen : stored) {
        if (reply.screens.size() == _context->options.maxListed) {
            break;
        }
        reply.screens.push_back(summarize(*_context, *bundle, session, permitted, screen));
    }
    co_return reply;
}

::core::async::Task<CustomScreenFound> CustomScreensModel::execute(CustomScreensGet request) {
    std::shared_ptr<ServedBundle const> const bundle = servedFor(*_context, request.app);
    checkName(request.id, "id");
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    auto found =
        co_await _context->store->get(CustomScreenKey{.person = person, .app = request.app, .id = request.id});
    if (!found) {
        throw std::runtime_error{found.error().reason};
    }
    std::vector<std::string> const permitted = permittedActions(*bundle, session, *_context->authorizer);
    CustomScreenSummary summary = summarize(*_context, *bundle, session, permitted, *found);
    co_return CustomScreenFound{.summary = std::move(summary), .document = std::move(found->document)};
}

::core::async::Task<CustomScreenChecked> CustomScreensModel::execute(CustomScreensCheck request) {
    std::shared_ptr<ServedBundle const> const bundle = servedFor(*_context, request.app);
    checkName(request.basedOn, "basedOn");
    ::morph::session::Context const session = callerSession();
    static_cast<void>(requirePerson(session));
    co_return CustomScreenChecked{
        .refusal = onWire(checkCustomScreen(*bundle, session, *_context->authorizer, request.basedOn, request.document,
                                            _context->options, &_context->verdicts))};
}

::core::async::Task<CustomScreenSaved> CustomScreensModel::execute(CustomScreensSave request) {
    std::shared_ptr<ServedBundle const> const bundle = servedFor(*_context, request.app);
    checkName(request.title, "title");
    checkName(request.basedOn, "basedOn");
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    if (request.id) {
        checkName(*request.id, "id");
        auto existing =
            co_await _context->store->get(CustomScreenKey{.person = person, .app = request.app, .id = *request.id});
        if (!existing) {
            throw std::runtime_error{existing.error().reason};
        }
    }
    if (auto refusal = checkCustomScreen(*bundle, session, *_context->authorizer, request.basedOn, request.document,
                                         _context->options, &_context->verdicts)) {
        throw std::invalid_argument{refusal->message()};
    }
    std::string const id = request.id ? *request.id : newFileRef(kCustomScreenIdPrefix);
    auto text = withScreenId(request.document, id);
    if (!text) {
        throw std::invalid_argument{text.error().message()};
    }
    // The rewritten text carries the longer id; it is what is stored and later listed, so it must fit the bound too.
    if (text->size() > _context->options.maxDocumentBytes) {
        throw std::invalid_argument{"a custom screen is longer than " +
                                    std::to_string(_context->options.maxDocumentBytes) + " bytes"};
    }
    auto stored = co_await _context->store->put(CustomScreenKey{.person = person, .app = request.app, .id = id},
                                                CustomScreen{.id = id,
                                                             .title = std::move(request.title),
                                                             .basedOn = std::move(request.basedOn),
                                                             .document = std::move(*text),
                                                             .savedAt = time::DateTime::now(),
                                                             .savedBy = person});
    if (!stored) {
        throw std::runtime_error{stored.error().reason};
    }
    co_return CustomScreenSaved{.id = id};
}

::core::async::Task<CustomScreenRemoved> CustomScreensModel::execute(CustomScreensRemove request) {
    static_cast<void>(servedFor(*_context, request.app));
    checkName(request.id, "id");
    ::morph::session::Context const session = callerSession();
    std::string const person = requirePerson(session);
    auto removed =
        co_await _context->store->remove(CustomScreenKey{.person = person, .app = request.app, .id = request.id});
    if (!removed) {
        throw std::runtime_error{removed.error().reason};
    }
    co_return CustomScreenRemoved{.removed = true};
}

void registerCustomScreens(std::shared_ptr<ICustomScreenStore> store,
                           std::function<std::shared_ptr<ServedBundle const>()> bundle,
                           std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                           model::detail::ActionDispatcher& dispatcher, model::detail::ModelRegistryFactory& registry,
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
    auto context = std::make_shared<detail::CustomScreensContext>(std::move(store), std::move(bundle),
                                                                  std::move(authorizer), options);
    registry.registerModel<CustomScreensModel>(kCustomScreensModel, [context] {
        return std::make_unique<model::detail::ModelHolder<CustomScreensModel>>(context);
    });
    dispatcher.registerAction<CustomScreensModel, CustomScreensList>(kCustomScreensModel, kCustomScreensList);
    dispatcher.registerAction<CustomScreensModel, CustomScreensGet>(kCustomScreensModel, kCustomScreensGet);
    dispatcher.registerAction<CustomScreensModel, CustomScreensCheck>(kCustomScreensModel, kCustomScreensCheck);
    dispatcher.registerAction<CustomScreensModel, CustomScreensSave>(kCustomScreensModel, kCustomScreensSave);
    dispatcher.registerAction<CustomScreensModel, CustomScreensRemove>(kCustomScreensModel, kCustomScreensRemove);
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
