// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/envelope_transport.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/attributes.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/file_store.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/theme.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

std::optional<std::string> MemoryBundleStore::get(std::string_view hash) const {
    std::scoped_lock const lock{_mutex};
    auto const found = _files.find(hash);
    if (found == _files.end()) {
        return std::nullopt;
    }
    return found->second;
}

void MemoryBundleStore::put(std::string const& hash, std::string bytes) {
    std::scoped_lock const lock{_mutex};
    _files.insert_or_assign(hash, std::move(bytes));
}

void MemoryBundleStore::remove(std::string_view hash) {
    std::scoped_lock const lock{_mutex};
    if (auto const found = _files.find(hash); found != _files.end()) {
        _files.erase(found);
    }
}

std::optional<std::string> MemoryBundleStore::lastDigest(std::string_view application) const {
    std::scoped_lock const lock{_mutex};
    auto const found = _digests.find(application);
    if (found == _digests.end()) {
        return std::nullopt;
    }
    return found->second;
}

void MemoryBundleStore::setLastDigest(std::string const& application, std::string digest) {
    std::scoped_lock const lock{_mutex};
    _digests.insert_or_assign(application, std::move(digest));
}

std::size_t MemoryBundleStore::size() const {
    std::scoped_lock const lock{_mutex};
    return _files.size();
}

VerifiedCache::VerifiedCache(std::shared_ptr<IBundleStore> store) : _store{std::move(store)} {
    if (!_store) {
        throw std::invalid_argument("VerifiedCache: the store is null");
    }
}

std::optional<std::string> VerifiedCache::get(std::string const& hash) {
    auto bytes = _store->get(hash);
    if (bytes && contentHash(*bytes) != hash) {
        ::morph::log::logWarn("[ui-cache] dropped {}: the stored bytes do not hash to it", hash);
        _store->remove(hash);
        return std::nullopt;
    }
    return bytes;
}

std::string VerifiedCache::put(std::string bytes) {
    std::string hash = contentHash(bytes);
    _store->put(hash, std::move(bytes));
    return hash;
}

Exchange exchangeOver(::morph::backend::IEnvelopeTransport& backend MORPH_LIFETIMEBOUND,
                      ::morph::exec::IExecutor& cbExec MORPH_LIFETIMEBOUND) {
    return [transport = &backend, owner = &cbExec](::morph::wire::Envelope request,
                                                   std::function<void(ExchangeReply reply)> done) {
        auto const answer = std::make_shared<std::function<void(ExchangeReply reply)>>(std::move(done));
        transport->request(std::move(request), *owner)
            .thenDetached([answer](::morph::wire::Envelope const& reply) { (*answer)(reply); })
            .onErrorDetached([answer](std::exception_ptr const& failure) {
                std::string message = "the request was not delivered";
                try {
                    std::rethrow_exception(failure);
                } catch (std::exception const& error) {
                    message = error.what();
                } catch (...) {  // NOLINT(bugprone-empty-catch): the default message stands for a non-std failure
                }
                (*answer)(std::unexpected(std::move(message)));
            });
    };
}

UiClient::UiClient(Exchange exchange, HelloRequest client, std::shared_ptr<IBundleStore> store)
    : _shared{std::make_shared<Shared>(std::move(exchange), std::move(client), VerifiedCache{std::move(store)})} {
    if (!_shared->exchange) {
        throw std::invalid_argument("UiClient: the exchange is empty");
    }
}

void UiClient::open(::morph::session::Context session, Done done) const {
    auto const run = std::make_shared<Run>(_shared, std::move(session), std::move(done));
    sendHello(run);
}

void UiClient::poll(::morph::session::Context session, std::string knownDigest, PollDone done) const {
    auto const answer = std::make_shared<PollDone>(std::move(done));
    auto const run =
        std::make_shared<Run>(_shared, std::move(session), [answer](std::expected<LoadedBundle, ConnectError> opened) {
            if (!opened) {
                (*answer)(std::unexpected(std::move(opened.error())));
                return;
            }
            (*answer)(std::optional<LoadedBundle>{std::move(*opened)});
        });
    run->knownDigest = std::move(knownDigest);
    run->unchanged = [answer] { (*answer)(std::optional<LoadedBundle>{}); };
    sendHello(run);
}

std::expected<LoadedBundle, ConnectError> UiClient::openOffline(std::string_view application) const {
    auto const digest = _shared->cache.store().lastDigest(application);
    if (!digest) {
        return std::unexpected(
            ConnectError{.kind = ConnectError::Kind::Unreachable,
                         .message = "no verified bundle of `" + std::string{application} + "` is cached"});
    }
    auto const text = _shared->cache.get(*digest);
    if (!text) {
        return std::unexpected(ConnectError{
            .kind = ConnectError::Kind::Unreachable,
            .message = "the cached manifest of `" + std::string{application} + "` is gone or does not verify"});
    }
    auto manifest = parseManifest(*text);
    if (!manifest) {
        return std::unexpected(ConnectError{.kind = ConnectError::Kind::Unsupported, .message = manifest.error()});
    }
    LoadedBundle loaded;
    loaded.manifest = std::move(*manifest);
    for (ManifestFile const& file : loaded.manifest.files) {
        if (!usable(_shared->client, file.kind)) {
            continue;
        }
        if (auto bytes = _shared->cache.get(file.hash)) {
            loaded.files.emplace(file.hash, std::move(*bytes));
        }
    }
    assemble(loaded, *digest);
    return loaded;
}

bool UiClient::usable(HelloRequest const& client, std::string_view kind) {
    if (kind == kComponentFile) {
        return client.components;
    }
    return kind == kScreenFile || kind == kAppFile || kind == kThemeFile || kind == kMessagesFile || kind == kIconFile;
}

void UiClient::assemble(LoadedBundle& loaded, std::string const& digest) {
    loaded.bundle.applicationId = loaded.manifest.application;
    loaded.bundle.manifestDigest = digest.substr(kHashPrefix.size());
    for (ManifestFile const& file : loaded.manifest.files) {
        if (file.kind != kScreenFile && file.kind != kAppFile && file.kind != kThemeFile) {
            continue;
        }
        if (auto const found = loaded.files.find(file.hash); found != loaded.files.end()) {
            auto& into = file.kind == kThemeFile ? loaded.bundle.themes : loaded.bundle.documents;
            into.insert_or_assign(file.id, found->second);
        }
    }
    // The icon set the theme index names, each icon's SVG by its name; a build refuses an index naming a file
    // the bundle lacks.
    for (auto const& [fileId, text] : loaded.bundle.themes) {
        static_cast<void>(fileId);
        if (!isThemeIndex(text)) {
            continue;
        }
        auto const index = parseThemeIndex(text);
        if (!index) {
            continue;
        }
        for (auto const& [name, iconId] : index->icons) {
            auto const file = std::ranges::find_if(loaded.manifest.files, [&iconId](ManifestFile const& listed) {
                return listed.kind == kIconFile && listed.id == iconId;
            });
            if (file == loaded.manifest.files.end()) {
                continue;
            }
            if (auto const found = loaded.files.find(file->hash); found != loaded.files.end()) {
                loaded.bundle.icons.insert_or_assign(name, found->second);
            }
        }
    }
}

::morph::wire::Envelope UiClient::requestOf(std::shared_ptr<Run> const& run, std::string_view kind) {
    ::morph::wire::Envelope request;
    request.kind = std::string{kind};
    request.session = run->session;
    return request;
}

void UiClient::sendHello(std::shared_ptr<Run> const& run) {
    ::morph::wire::Envelope request = requestOf(run, kHelloKind);
    request.body = detail::writeMessage(run->shared->client);
    run->shared->exchange(std::move(request), [run](ExchangeReply reply) { onHello(run, std::move(reply)); });
}

void UiClient::sendCatalog(std::shared_ptr<Run> const& run) {
    run->shared->exchange(requestOf(run, kCatalogKind),
                          [run](ExchangeReply reply) { onCatalog(run, std::move(reply)); });
}

void UiClient::fetch(std::shared_ptr<Run> const& run, std::string const& hash,
                     std::function<void(ExchangeReply)> then) {
    ::morph::wire::Envelope request = requestOf(run, kFetchKind);
    request.body = detail::writeMessage(FetchRequest{.hash = hash});
    run->shared->exchange(std::move(request), std::move(then));
}

void UiClient::fail(std::shared_ptr<Run> const& run, ConnectError error) {
    ::morph::log::logWarn("[ui-fetch] {}", error.message);
    run->done(std::unexpected(std::move(error)));
}

std::expected<std::string, ConnectError> UiClient::bodyOf(ExchangeReply reply) {
    if (!reply) {
        return std::unexpected(ConnectError{.kind = ConnectError::Kind::Unreachable, .message = reply.error()});
    }
    if (reply->kind != "ok") {
        return std::unexpected(
            ConnectError{.kind = ConnectError::Kind::Refused, .message = std::move(reply->message)});
    }
    return std::move(reply->body);
}

std::expected<std::string, ConnectError> UiClient::verified(ExchangeReply reply, Expected const& expected) {
    auto body = bodyOf(std::move(reply));
    if (!body) {
        body.error().message = expected.what + ": " + body.error().message;
        return body;
    }
    if (body->size() > kMaxFileBytes) {
        return std::unexpected(
            ConnectError{.kind = ConnectError::Kind::Unverified,
                         .message = expected.what + " is larger than " + std::to_string(kMaxFileBytes) + " bytes"});
    }
    if (std::string const actual = contentHash(*body); actual != expected.hash) {
        return std::unexpected(ConnectError{.kind = ConnectError::Kind::Unverified,
                                            .message = expected.what + " does not match its hash (expected " +
                                                       expected.hash + ", got " + actual + ")"});
    }
    return body;
}

void UiClient::onHello(std::shared_ptr<Run> const& run, ExchangeReply reply) {
    auto body = bodyOf(std::move(reply));
    if (!body) {
        ConnectError error = std::move(body.error());
        if (error.message == "unknown envelope kind: " + std::string{kHelloKind}) {
            error.kind = ConnectError::Kind::NoUiService;
            error.message = "the server serves no UI";
        } else if (error.message.starts_with("missing vocabulary: ")) {
            error.kind = ConnectError::Kind::Unsupported;
        }
        fail(run, std::move(error));
        return;
    }
    auto hello = parseHelloReply(*body);
    if (!hello) {
        fail(run, ConnectError::Kind::Unsupported, std::move(hello.error()));
        return;
    }
    HelloRequest const& client = run->shared->client;
    for (std::string const& vocabulary : hello->vocabularies) {
        if (std::ranges::find(client.vocabularies, vocabulary) == client.vocabularies.end()) {
            fail(run, ConnectError::Kind::Unsupported,
                 "the application needs the vocabulary " + vocabulary + ", which this client does not speak");
            return;
        }
    }
    if (run->unchanged && hello->digest == run->knownDigest) {
        ::morph::log::logDebug("[ui-fetch] polling `{}`: digest {}, known {}, manifest unchanged", hello->application,
                               hello->digest, run->knownDigest);
        run->unchanged();
        return;
    }
    ::morph::log::logDebug("[ui-fetch] {} `{}`: digest {}, known {}, manifest {}",
                           run->unchanged ? "polling" : "opening", hello->application, hello->digest,
                           run->knownDigest.empty() ? std::string{"<none>"} : run->knownDigest,
                           hello->digest == run->knownDigest ? "unchanged" : "changed");
    run->loaded.hello = std::move(*hello);
    std::string const& digest = run->loaded.hello.digest;
    if (auto cached = run->shared->cache.get(digest)) {
        onManifest(run, std::move(*cached));
        return;
    }
    fetch(run, digest, [run](ExchangeReply fetched) {
        auto text = verified(std::move(fetched), Expected{.hash = run->loaded.hello.digest, .what = "the manifest"});
        if (!text) {
            fail(run, std::move(text.error()));
            return;
        }
        onManifest(run, std::move(*text));
    });
}

void UiClient::onManifest(std::shared_ptr<Run> const& run, std::string text) {
    auto manifest = parseManifest(text);
    if (!manifest) {
        fail(run, ConnectError::Kind::Unsupported, std::move(manifest.error()));
        return;
    }
    static_cast<void>(run->shared->cache.put(std::move(text)));
    run->loaded.manifest = std::move(*manifest);
    sendCatalog(run);
}

void UiClient::onCatalog(std::shared_ptr<Run> const& run, ExchangeReply reply) {
    auto body = bodyOf(std::move(reply));
    if (!body) {
        fail(run, std::move(body.error()));
        return;
    }
    auto catalog = parseCatalogReply(*body);
    if (!catalog) {
        fail(run, ConnectError::Kind::Unsupported, std::move(catalog.error()));
        return;
    }
    run->loaded.catalog = std::move(*catalog);
    std::vector<Expected> missing;
    for (ManifestFile const& file : run->loaded.manifest.files) {
        if (!wanted(run->shared->client, run->loaded.catalog, file)) {
            continue;
        }
        if (auto cached = run->shared->cache.get(file.hash)) {
            run->loaded.files.insert_or_assign(file.hash, std::move(*cached));
        } else {
            Expected expected{.hash = file.hash, .what = "the " + file.kind + " `" + file.id + "`"};
            ::morph::log::logDebug("[ui-fetch] fetching {} ({})", expected.hash, expected.what);
            missing.push_back(std::move(expected));
        }
    }
    // One count for each fetch and one for this loop, so a synchronous exchange that answers every fetch
    // before the next is sent cannot settle the run early.
    run->pending = missing.size() + 1;
    for (Expected& expected : missing) {
        std::string const hash = expected.hash;
        fetch(run, hash, [run, expected = std::move(expected)](ExchangeReply fetched) {
            auto bytes = verified(std::move(fetched), expected);
            {
                std::scoped_lock const lock{run->mutex};
                if (!bytes) {
                    if (!run->failure) {
                        run->failure = std::move(bytes.error());
                    }
                } else {
                    static_cast<void>(run->shared->cache.put(*bytes));
                    run->loaded.files.insert_or_assign(expected.hash, std::move(*bytes));
                }
            }
            settle(run);
        });
    }
    settle(run);
}

bool UiClient::wanted(HelloRequest const& client, CatalogReply const& catalog, ManifestFile const& file) {
    if (!usable(client, file.kind)) {
        return false;
    }
    if (file.kind != kScreenFile) {
        return true;
    }
    return std::ranges::any_of(catalog.entries, [&file](CatalogEntry const& entry) {
        return entry.id == file.id && (entry.available || entry.isPublic);
    });
}

void UiClient::settle(std::shared_ptr<Run> const& run) {
    {
        std::scoped_lock const lock{run->mutex};
        if (--run->pending != 0) {
            return;
        }
    }
    // Past the last count nothing else touches the run, so it is read without the lock.
    if (run->failure) {
        fail(run, std::move(*run->failure));
        return;
    }
    assemble(run->loaded, run->loaded.hello.digest);
    run->shared->cache.store().setLastDigest(run->loaded.hello.application, run->loaded.hello.digest);
    run->done(std::move(run->loaded));
}

std::expected<std::string, std::string> evaluationValue(ExchangeReply reply) {
    if (!reply) {
        return std::unexpected(std::move(reply.error()));
    }
    if (reply->kind != "ok") {
        return std::unexpected(std::move(reply->message));
    }
    auto evaluated = parseEvaluateReply(reply->body);
    if (!evaluated) {
        return std::unexpected(std::move(evaluated.error()));
    }
    Json::object_t errors;
    for (auto const& [field, message] : evaluated->errors) {
        errors.insert_or_assign(field, Json(message));
    }
    auto& members = evaluated->body.get<Json::object_t>();
    members.insert_or_assign("errors", Json(std::move(errors)));
    members.insert_or_assign("ready", Json(evaluated->ready));
    auto text = glz::write_json(evaluated->body);
    if (!text) {
        return std::unexpected("the ui-evaluate reply's body cannot be written");
    }
    return std::move(*text);
}

std::function<::morph::async::Completion<std::string>(std::string const&, std::string const&, std::string const&)>
evaluatorOver(Exchange exchange, ::morph::session::Context session, ::morph::exec::IExecutor* cbExec) {
    return [exchange = std::move(exchange), session = std::move(session), cbExec](
               std::string const& typeId, std::string const& actionId, std::string const& bodyJson) {
        auto [completion, promise] = ::morph::async::Completion<std::string>::makeSettleable(cbExec);
        auto settle = std::make_shared<decltype(promise)>(std::move(promise));
        ::morph::wire::Envelope request;
        request.kind = std::string{kEvaluateKind};
        request.session = session;
        request.modelType = typeId;
        request.actionType = actionId;
        request.body = bodyJson;
        exchange(std::move(request), [settle](ExchangeReply reply) {
            auto value = evaluationValue(std::move(reply));
            if (value) {
                settle->resolve(std::move(*value));
            } else {
                settle->reject(std::make_exception_ptr(std::runtime_error{value.error()}));
            }
        });
        return std::move(completion);
    };
}

std::function<::morph::async::Completion<std::string>(std::string const&, std::string const&)> watcherOver(
    Exchange exchange, std::function<::morph::session::Context()> currentSession, ::morph::exec::IExecutor* cbExec) {
    return [exchange = std::move(exchange), currentSession = std::move(currentSession), cbExec](
               std::string const& kind, std::string const& bodyJson) {
        auto [completion, promise] = ::morph::async::Completion<std::string>::makeSettleable(cbExec);
        auto settle = std::make_shared<decltype(promise)>(std::move(promise));
        ::morph::wire::Envelope request;
        request.kind = kind;
        request.session = currentSession ? currentSession() : ::morph::session::Context{};
        request.body = bodyJson;
        exchange(std::move(request), [settle](ExchangeReply reply) {
            if (!reply) {
                settle->reject(std::make_exception_ptr(std::runtime_error{reply.error()}));
            } else if (reply->kind != "ok") {
                settle->reject(std::make_exception_ptr(std::runtime_error{reply->message}));
            } else {
                settle->resolve(std::move(reply->body));
            }
        });
        return std::move(completion);
    };
}

FileTransfer::FileTransfer(Exchange exchange, ::morph::session::Context session)
    : _shared{std::make_shared<Shared>(Shared{.exchange = std::move(exchange), .session = std::move(session)})} {
    if (!_shared->exchange) {
        throw std::invalid_argument("FileTransfer: the exchange is empty");
    }
}

void FileTransfer::upload(FileInfo info, std::string bytes, std::string model, std::string action,
                          std::function<void(double)> progress,
                          std::function<void(std::expected<std::string, std::string>)> done) const {
    auto const run = std::make_shared<Upload>(Upload{.shared = _shared,
                                                     .bytes = std::move(bytes),
                                                     .upload = {},
                                                     .progress = std::move(progress),
                                                     .done = std::move(done)});
    FileRequest begin;
    begin.op = "begin";
    begin.name = std::move(info.name);
    begin.type = std::move(info.type);
    begin.size = run->bytes.size();
    begin.model = std::move(model);
    begin.action = std::move(action);
    send(*_shared, begin, [run](std::expected<FileReply, std::string> reply) {
        if (!reply || !reply->upload) {
            run->fail(reply ? std::string{"the ui-file begin reply has no upload id"} : std::move(reply.error()));
            return;
        }
        run->upload = std::move(*reply->upload);
        sendChunk(run, 0);
    });
}

void FileTransfer::download(std::string ref, std::function<void(double)> progress,
                            std::function<void(std::expected<DownloadedFile, std::string>)> done) const {
    auto const run = std::make_shared<Download>(Download{.shared = _shared,
                                                         .ref = std::move(ref),
                                                         .file = {},
                                                         .progress = std::move(progress),
                                                         .done = std::move(done)});
    readChunk(run);
}

void FileTransfer::Upload::fail(std::string reason) const {
    if (done) {
        done(std::unexpected(std::move(reason)));
    }
}

void FileTransfer::send(Shared const& shared, FileRequest const& request,
                        std::function<void(std::expected<FileReply, std::string>)> then) {
    ::morph::wire::Envelope envelope;
    envelope.kind = std::string{kFileKind};
    envelope.session = shared.session;
    envelope.body = detail::writeMessage(request);
    shared.exchange(std::move(envelope), [then = std::move(then)](ExchangeReply reply) {
        if (!reply) {
            then(std::unexpected(std::move(reply.error())));
            return;
        }
        if (reply->kind != "ok") {
            then(std::unexpected(std::move(reply->message)));
            return;
        }
        then(parseFileReply(reply->body));
    });
}

void FileTransfer::sendChunk(std::shared_ptr<Upload> const& run, std::size_t offset) {
    if (offset >= run->bytes.size()) {
        FileRequest end;
        end.op = "end";
        end.upload = run->upload;
        send(*run->shared, end, [run](std::expected<FileReply, std::string> reply) {
            if (!reply || !reply->ref) {
                run->fail(reply ? std::string{"the ui-file end reply has no reference"} : std::move(reply.error()));
                return;
            }
            if (run->done) {
                run->done(std::move(*reply->ref));
            }
        });
        return;
    }
    FileRequest chunk;
    chunk.op = "chunk";
    chunk.upload = run->upload;
    chunk.offset = offset;
    std::string_view const all{run->bytes};
    chunk.data = ::morph::session::detail::base64UrlEncode(all.substr(offset, kMaxFileChunkBytes));
    send(*run->shared, chunk, [run](std::expected<FileReply, std::string> reply) {
        if (!reply || !reply->received) {
            run->fail(reply ? std::string{"the ui-file chunk reply has no count"} : std::move(reply.error()));
            return;
        }
        if (run->progress) {
            run->progress(static_cast<double>(*reply->received) / static_cast<double>(run->bytes.size()));
        }
        sendChunk(run, *reply->received);
    });
}

void FileTransfer::readChunk(std::shared_ptr<Download> const& run) {
    FileRequest read;
    read.op = "read";
    read.ref = run->ref;
    read.offset = run->file.bytes.size();
    send(*run->shared, read, [run](std::expected<FileReply, std::string> reply) {
        std::optional<std::string> bytes;
        if (reply && reply->data && reply->total) {
            bytes = ::morph::session::detail::base64UrlDecode(*reply->data);
        }
        if (!bytes) {
            if (run->done) {
                run->done(std::unexpected(reply ? std::string{"the ui-file read reply is malformed"}
                                                : std::move(reply.error())));
            }
            return;
        }
        run->file.bytes += *bytes;
        run->file.name = reply->name.value_or("");
        run->file.type = reply->type.value_or("");
        std::size_t const total = *reply->total;
        if (run->progress && total > 0) {
            run->progress(static_cast<double>(run->file.bytes.size()) / static_cast<double>(total));
        }
        if (run->file.bytes.size() >= total || bytes->empty()) {
            if (run->done) {
                run->done(std::move(run->file));
            }
            return;
        }
        readChunk(run);
    });
}
}  // namespace morph::ui
