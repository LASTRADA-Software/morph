// SPDX-License-Identifier: Apache-2.0

// Transfers, component properties and the value helpers lowering shares.

#include "interpreter_detail.hpp"

namespace morph::ui::detail {

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a download and an upload, each settled.
/// @brief A transfer's share of its screen's pending count.
///
/// A transfer is pending from the host accepting it to its settle: its completion carries on the command list that
/// started it, which the close protocol and save-and-close wait for. The count is taken once the host accepts, so a
/// refused transfer never counts, and a host that settles before it returns leaves nothing taken. It is given back
/// once, also when the transfer settles after its scope is gone, but not after the screen itself is gone, since the
/// count went with it.
class TransferHold {
public:
    /// @brief A hold that counts nothing yet.
    /// @param core  The screen whose count it moves.
    /// @param alive Expires with the screen.
    TransferHold(ScreenCore& core, std::weak_ptr<bool> alive) : _core{&core}, _alive{std::move(alive)} {}

    /// @brief Counts the transfer, unless it has already settled.
    void take() {
        if (_state == State::NotTaken) {
            _state = State::Taken;
            _core->addPending(1);
        }
    }

    /// @brief Gives the count back, if one was taken and the screen is still there; the transfer counts no more.
    void release() {
        if (_state == State::Taken && !_alive.expired()) {
            _core->addPending(-1);
        }
        _state = State::Released;
    }

private:
    enum class State : std::uint8_t { NotTaken, Taken, Released };

    ScreenCore* _core;
    std::weak_ptr<bool> _alive;
    State _state = State::NotTaken;
};

}  // namespace

[[nodiscard]] std::string base64Encode(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        std::size_t const left = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t group = 0;
        for (std::size_t offset = 0; offset < 3; ++offset) {
            group <<= 8U;
            if (offset < left) {
                group |= static_cast<unsigned char>(bytes.at(index + offset));
            }
        }
        for (std::size_t digit = 0; digit < 4; ++digit) {
            out.push_back(digit <= left ? kAlphabet.at((group >> (18U - (6U * digit))) & 0x3FU) : '=');
        }
    }
    return out;
}

[[nodiscard]] std::optional<std::string> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    auto const digitOf = [](char chr) -> int {
        if (chr >= 'A' && chr <= 'Z') {
            return chr - 'A';
        }
        if (chr >= 'a' && chr <= 'z') {
            return chr - 'a' + 26;
        }
        if (chr >= '0' && chr <= '9') {
            return chr - '0' + 52;
        }
        if (chr == '+') {
            return 62;
        }
        return chr == '/' ? 63 : -1;
    };
    std::string out;
    out.reserve((text.size() * 3) / 4);
    std::uint32_t group = 0;
    int bits = 0;
    for (char const chr : text) {
        int const digit = digitOf(chr);
        if (digit < 0) {
            return std::nullopt;
        }
        group = (group << 6U) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((group >> static_cast<std::uint32_t>(bits)) & 0xFFU));
        }
    }
    return out;
}

[[nodiscard]] std::size_t base64Size(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    return (text.size() * 3) / 4;
}

[[nodiscard]] std::string sizeText(std::size_t bytes) {
    constexpr std::size_t kKiB = 1024;
    if (bytes < kKiB) {
        return std::to_string(bytes) + " bytes";
    }
    bool const mebi = bytes >= kKiB * kKiB;
    std::size_t const unit = mebi ? kKiB * kKiB : kKiB;
    // One decimal place, rounded half up, and none when it is zero.
    std::size_t const tenths = ((bytes * 10) + (unit / 2)) / unit;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        text += "." + std::to_string(tenths % 10);
    }
    return text + (mebi ? " MiB" : " KiB");
}

[[nodiscard]] std::expected<std::string, std::string> inlineBytes(Value const& value) {
    if (std::string const* const text = value.asString()) {
        if (std::optional<std::string> bytes = base64Decode(*text)) {
            return *std::move(bytes);
        }
        return std::unexpected(std::string{"the text is not base64"});
    }
    if (ValueList const* const list = value.asList()) {
        std::string bytes;
        bytes.reserve(list->size());
        for (Value const& item : *list) {
            std::int64_t const* const byte = item.asInt();
            if (byte == nullptr || *byte < 0 || *byte > 255) {
                return std::unexpected(std::string{"a byte list holds integers 0 to 255"});
            }
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(*byte)));
        }
        return bytes;
    }
    return std::unexpected(std::string{"inline bytes are base64 text or a list of bytes"});
}

void ScreenCore::runTransfer(Command const& command, CommandCtx const& ctx, Done done) {
    Value const file = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (command.kind == CommandKind::Download) {
        runDownload(command, ctx, file, std::move(done));
    } else {
        runUpload(command, ctx, file, std::move(done));
    }
}

void ScreenCore::runDownload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    Value const name = evalIn(*ctx.scope, ctx.bindings.get(), command.extra);
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    auto const settled = [this, weak = std::weak_ptr<RtScope>{ctx.scope}, target = command.target,
                          preview = command.option == "preview", done,
                          hold](std::expected<Value, std::string> const& reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            bool const succeeded = reply.has_value();
            NameRef const* const found = succeeded && preview ? held->find(target) : nullptr;
            if (found != nullptr && found->category == NameCategory::State) {
                initState(*found->state->scope, *found->state);
                session.noteWrite();
                found->state->value->set(*reply);
            }
            done(succeeded);
            hold->release();
        });
    };
    if (command.bytes) {
        std::optional<InlineFile> const inlined = inlineDownload(command, ctx, file, name);
        if (!inlined) {
            done(false);
            return;
        }
        if (host == nullptr || !host->saveFile(*inlined, command.option, settled)) {
            unsupported(command, "`download`");
            done(false);
            return;
        }
        hold->take();
        return;
    }
    if (host == nullptr || !host->download(file, command.option, name, settled)) {
        unsupported(command, "`download`");
        done(false);
        return;
    }
    hold->take();
}

// Inline bytes are checked before the host is asked: content over the cap, or that does not decode, never reaches it.
std::optional<InlineFile> ScreenCore::inlineDownload(
    Command const& command, CommandCtx const& ctx,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the command's `file` and `name`, in its order.
    Value const& file, Value const& name) {
    std::size_t const cap = session.options().maxInlineFileBytes;
    std::size_t size = 0;
    if (std::string const* const text = file.asString()) {
        size = base64Size(*text);
    } else if (ValueList const* const list = file.asList()) {
        size = list->size();
    }
    std::expected<std::string, std::string> bytes = size > cap ? std::unexpected(std::string{}) : inlineBytes(file);
    if (size > cap || (bytes && bytes->size() > cap)) {
        report(command.path, "download: " + sizeText(bytes ? bytes->size() : size) + " is over the " + sizeText(cap) +
                                 " inline cap; serve it through the file store");
        return std::nullopt;
    }
    if (!bytes) {
        report(command.path, "download: " + bytes.error());
        return std::nullopt;
    }
    return InlineFile{
        .name = textOf(name, ctx.scope->formatting()),
        .mediaType = command.mediaType
                         ? textOf(evalIn(*ctx.scope, ctx.bindings.get(), command.mediaType), ctx.scope->formatting())
                         : std::string{},
        .bytes = *std::move(bytes),
    };
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome an upload can have.
void ScreenCore::runUpload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    RtScope const& scope = *ctx.scope;
    std::weak_ptr<RtScope> const weak = ctx.scope;
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    NameRef const* const transferName = command.transfer.empty() ? nullptr : scope.find(command.transfer);
    TransferState* const transfer =
        transferName != nullptr && transferName->category == NameCategory::Transfer ? transferName->transfer : nullptr;
    auto const progress = [weak, transfer](Value fraction) {
        if (weak.lock() && transfer != nullptr) {
            transfer->progress.set(std::move(fraction));
        }
    };
    // Settles with the members the mutation's body gets beside its own (the reference or the inline file), or the
    // failure.
    auto const settled = [this, weak, kept = detached(ctx), transfer, command = &command, done,
                          hold](std::expected<ValueRecord, std::string> reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            if (transfer != nullptr) {
                transfer->pending.set(false);
                transfer->error.set(reply ? Value{} : Value{reply.error()});
            }
            if (!reply) {
                done(false);
                hold->release();
                return;
            }
            NameRef const* const found = held->find(command->target);
            // The mutation's body cannot name the reference (a body is checked where the mutation is declared),
            // so the reference is added to it under `as`, `file` by default.
            MutationRuntime& mutation = *found->mutation;
            Value const base = evalIn(*held, kept.bindings.get(), mutation.decl().body);
            ValueRecord fields = base.asRecord() == nullptr ? ValueRecord{} : *base.asRecord();
            for (auto& [member, value] : *reply) {
                fields.insert_or_assign(member, std::move(value));
            }
            // The run takes its own count before this one is given back, so pending never reads zero in between.
            mutation.run(Value{std::move(fields)}, std::make_shared<Bindings const>(*kept.bindings),
                         [done](bool succeeded, Value const&) { done(succeeded); });
            hold->release();
        });
    };
    if (host == nullptr) {
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    if (transfer != nullptr) {
        transfer->pending.set(true);
        transfer->progress.set(Value{std::int64_t{0}});
        transfer->error.set(Value{});
    }
    NameRef const* const target = scope.find(command.target);
    UploadRequest request{.file = file, .modelType = {}, .action = {}};
    if (target != nullptr && target->category == NameCategory::Mutation && target->mutation != nullptr) {
        request.modelType = target->mutation->typeId();
        request.action = target->mutation->decl().action;
    }
    std::string const member = command.binding.empty() ? std::string{"file"} : command.binding;
    bool started = false;
    if (command.bytes) {
        std::size_t const cap = session.options().maxInlineFileBytes;
        started = host->readFile(
            file, cap,
            [this, settled, progress, cap, member, command = &command](std::expected<InlineFile, std::string> read) {
                if (!read) {
                    settled(std::unexpected(read.error()));
                    return;
                }
                if (read->bytes.size() > cap) {
                    // The host should have refused it; the cap holds whether or not it did.
                    std::string why = "upload: " + sizeText(read->bytes.size()) + " is over the " + sizeText(cap) +
                                      " inline cap; send it through the file store";
                    report(command->path, why);
                    settled(std::unexpected(std::move(why)));
                    return;
                }
                progress(Value{std::int64_t{1}});
                ValueRecord added;
                added.insert_or_assign(member, Value{base64Encode(read->bytes)});
                if (!command->nameAs.empty()) {
                    added.insert_or_assign(command->nameAs, Value{read->name});
                }
                if (!command->typeAs.empty()) {
                    added.insert_or_assign(command->typeAs, Value{read->mediaType});
                }
                settled(std::move(added));
            });
    } else {
        started = host->uploadFor(request, progress, [settled, member](std::expected<Value, std::string> reference) {
            if (!reference) {
                settled(std::unexpected(reference.error()));
                return;
            }
            ValueRecord added;
            added.insert_or_assign(member, *std::move(reference));
            settled(std::move(added));
        });
    }
    if (!started) {
        if (transfer != nullptr) {
            transfer->pending.set(false);
            transfer->progress.set(Value{});
        }
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    hold->take();
}

// --- Lowering --------------------------------------------------------------------------------------------------

[[nodiscard]] bool boolOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);
    bool const* const flag = value.asBool();
    return flag != nullptr && *flag;
}

[[nodiscard]] std::string stringOf(Value const& value, FormatContext const& format) { return textOf(value, format); }

[[nodiscard]] std::string componentText(Value const& value, std::string_view type, FormatContext const& format) {
    if (type == "json") {
        return canonicalJson(value);
    }
    if (type == "int" || type == "exact") {
        if (std::int64_t const* const whole = value.asInt()) {
            return std::to_string(*whole);
        }
        // A value that cannot be rounded at its own places is written as its exact JSON, never clamped.
        if (Decimal const* const number = value.asDecimal()) {
            return detail::decimalText(number->value, static_cast<std::uint32_t>(number->value.decimalPlaces.value))
                .value_or(canonicalJson(value));
        }
        if (Quantity const* const quantity = value.asQuantity()) {
            auto const text =
                detail::decimalText(quantity->value, static_cast<std::uint32_t>(quantity->value.decimalPlaces.value));
            return text ? *text + " " + quantity->unit : canonicalJson(value);
        }
    }
    return textOf(value, format);
}

[[nodiscard]] double realOf(math::Rational const& value) noexcept {
    return static_cast<double>(value.numerator) / static_cast<double>(value.denominator);
}

[[nodiscard]] double componentNumber(Value const& value) {
    if (std::int64_t const* const whole = value.asInt()) {
        return static_cast<double>(*whole);
    }
    if (Decimal const* const number = value.asDecimal()) {
        return realOf(number->value);
    }
    if (Quantity const* const quantity = value.asQuantity()) {
        return realOf(quantity->value);
    }
    return 0.0;
}

[[nodiscard]] Value componentPayload(std::string const& payload, Type const* type) {
    std::string_view const text{payload};
    if (text.starts_with("i:")) {
        std::int64_t whole = 0;
        std::string_view const digits = text.substr(2);
        char const* const last = std::to_address(digits.end());
        auto const [end, error] = std::from_chars(std::to_address(digits.begin()), last, whole);
        if (error == std::errc{} && end == last) {
            return Value{whole};
        }
    }
    if (text.starts_with("s:")) {
        return Value{std::string{text.substr(2)}};
    }
    if (auto json = parseJson(text); json && type != nullptr) {
        if (auto value = valueFromJson(*json, *type)) {
            return std::move(*value);
        }
    }
    return Value{payload};
}

[[nodiscard]] std::int64_t intOf(Value const& value, std::int64_t fallback) {
    if (std::int64_t const* const number = value.asInt()) {
        return *number;
    }
    if (Decimal const* const number = value.asDecimal()) {
        // Integer division truncates toward zero, and reads the exact value rather than one rounded to its places.
        return number->value.numerator / number->value.denominator;
    }
    return fallback;
}

[[nodiscard]] std::optional<TextRole> roleNamed(std::string_view name) {
    if (name == "normal") {
        return TextRole::Normal;
    }
    if (name == "muted") {
        return TextRole::Muted;
    }
    if (name == "heading" || name == "title") {
        return TextRole::Heading;
    }
    if (name == "section") {
        return TextRole::Section;
    }
    if (name == "error") {
        return TextRole::Error;
    }
    if (name == "success") {
        return TextRole::Success;
    }
    if (name == "mono") {
        return TextRole::Mono;
    }
    if (name == "display") {
        return TextRole::Display;
    }
    if (name == "small") {
        return TextRole::Small;
    }
    if (name == "overline") {
        return TextRole::Overline;
    }
    if (name == "caption") {
        return TextRole::Caption;
    }
    if (name == "label") {
        return TextRole::Label;
    }
    if (name == "headline") {
        return TextRole::Headline;
    }
    return std::nullopt;
}

[[nodiscard]] TextRole roleOf(Value const& value, FormatContext const& format) {
    return roleNamed(textOf(value, format)).value_or(TextRole::Normal);
}

[[nodiscard]] Tone toneOf(Value const& value, FormatContext const& format) {
    std::string const text = textOf(value, format);
    if (text == "info") {
        return Tone::Info;
    }
    if (text == "ok" || text == "success") {
        return Tone::Ok;
    }
    if (text == "warn" || text == "warning") {
        return Tone::Warn;
    }
    if (text == "err" || text == "error") {
        return Tone::Err;
    }
    return Tone::Neutral;
}

[[nodiscard]] std::optional<Tone> toneNamed(Value const& value) {
    if (value.isNull()) {
        return Tone::Neutral;
    }
    static constexpr std::array<std::pair<std::string_view, Tone>, 5> kTones = {
        {
            {"neutral", Tone::Neutral},
            {"info", Tone::Info},
            {"ok", Tone::Ok},
            {"warn", Tone::Warn},
            {"err", Tone::Err},
        },
    };
    if (std::string const* const text = value.asString()) {
        // A loop, not std::ranges::find: an array's iterator is a pointer on some standard libraries and not on
        // others, so `auto` there reads differently to each.
        for (auto const& [name, tone] : kTones) {
            if (name == *text) {
                return tone;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<TextSpan> spansOf(Value const& value, FormatContext const& format, bool& dropped,
                                            bool* badTone) {
    std::vector<TextSpan> spans;
    if (value.isNull()) {
        return spans;
    }
    ValueList const* const runs = value.asList();
    if (runs == nullptr) {
        dropped = true;
        return spans;
    }
    auto const flag = [](Value const* field) {
        bool const* const set = field != nullptr ? field->asBool() : nullptr;
        return set != nullptr && *set;
    };
    for (Value const& run : *runs) {
        if (run.asRecord() == nullptr) {
            if (run.isNull() || run.asList() != nullptr) {
                dropped = true;
            } else {
                spans.push_back(TextSpan{.text = textOf(run, format)});
            }
            continue;
        }
        Value const* const text = run.field("text");
        if (text == nullptr) {
            dropped = true;
            continue;
        }
        Tone tone = Tone::Neutral;
        if (Value const* const named = run.field("tone")) {
            if (std::optional<Tone> const known = toneNamed(*named)) {
                tone = *known;
            } else if (badTone != nullptr) {
                *badTone = true;
            }
        }
        spans.push_back(TextSpan{
            .text = textOf(*text, format),
            .strong = flag(run.field("strong")),
            .mono = flag(run.field("mono")),
            .tone = tone,
            .action = flag(run.field("action")),
        });
    }
    return spans;
}

[[nodiscard]] bool declaresNames(ScopeDecl const& scope) noexcept {
    return !(scope.models.empty() && scope.state.empty() && scope.lets.empty() && scope.queries.empty() &&
             scope.mutations.empty() && scope.watches.empty() && scope.checks.empty());
}

[[nodiscard]] std::optional<std::vector<int>> splitterSizesOf(Value const& value) {
    ValueList const* const items = value.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    std::vector<int> sizes;
    sizes.reserve(items->size());
    for (Value const& item : *items) {
        if (item.asInt() == nullptr && item.asDecimal() == nullptr) {
            return std::nullopt;
        }
        sizes.push_back(static_cast<int>(intOf(item, 0)));
    }
    return sizes;
}

[[nodiscard]] Sizing sizingOf(Value const& value) {
    if (Value const* const fixed = value.field("fixed")) {
        return Sizing::fixed(static_cast<int>(intOf(*fixed, 0)));
    }
    if (Value const* const percent = value.field("percent")) {
        return Sizing::percent(static_cast<int>(intOf(*percent, 1)));
    }
    if (Value const* const stretch = value.field("stretch")) {
        return Sizing::stretch(static_cast<int>(intOf(*stretch, 1)));
    }
    return Sizing::content();
}

[[nodiscard]] bool isSizing(Value const& value) {
    if (std::string const* const text = value.asString()) {
        return *text == "content";
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr || record->size() != 1 || record->begin()->second.asInt() == nullptr) {
        return false;
    }
    if (record->contains("percent")) {
        std::int64_t const share = *record->begin()->second.asInt();
        return share >= 1 && share <= 100;
    }
    return record->contains("fixed") || record->contains("stretch");
}

[[nodiscard]] Align alignOf(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return Align::Stretch;
}

[[nodiscard]] std::optional<Align> cellPlaceNamed(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return std::nullopt;
}

[[nodiscard]] Justify justifyOf(std::string_view name) {
    if (name == "center") {
        return Justify::Center;
    }
    if (name == "end") {
        return Justify::End;
    }
    if (name == "between") {
        return Justify::Between;
    }
    return Justify::Start;
}

[[nodiscard]] Axis axisOf(Value const& value) {
    std::string const* const text = value.asString();
    return text != nullptr && *text == "horizontal" ? Axis::Horizontal : Axis::Vertical;
}

[[nodiscard]] StepState stepStateOf(std::string_view name) {
    if (name == "active") {
        return StepState::Active;
    }
    if (name == "done") {
        return StepState::Done;
    }
    return name == "failed" ? StepState::Failed : StepState::Pending;
}

[[nodiscard]] std::vector<SelectOption> selectOptions(Value const& options, FormatContext const& format) {
    std::vector<SelectOption> out;
    if (ValueList const* const items = options.asList()) {
        for (Value const& option : *items) {
            Value const key = optionKey(option);
            Value const* const label = option.field("label");
            out.push_back(SelectOption{.key = keyOf(key), .label = textOf(label != nullptr ? *label : key, format)});
        }
    }
    return out;
}

[[nodiscard]] LayoutHints layoutHintsOf(Node const& node) {
    if (!node) {
        return {};
    }
    return std::visit(
        [](auto const& spec) -> LayoutHints {
            if constexpr (requires { spec.common.layout; }) {
                return spec.common.layout;
            } else {
                return {};
            }
        },
        node->kind);
}

[[nodiscard]] Node withWidth(Node const& node, Sizing width) {
    if (!node) {
        return node;
    }
    auto copy = std::make_shared<NodeData>(*node);
    std::visit(
        [&width](auto& spec) {
            if constexpr (requires { spec.common.layout; }) {
                spec.common.layout.width = width;
            }
        },
        copy->kind);
    return copy;
}

[[nodiscard]] Sizing holderSizing(std::span<Sizing const> sizings) {
    int weight = 0;
    for (Sizing const& sizing : sizings) {
        if (sizing.kind == Sizing::Kind::Stretch) {
            weight = std::max(weight, sizing.amount);
        }
    }
    return weight > 0 ? Sizing::stretch(weight) : Sizing::content();
}

namespace {

/// @brief The column that holds a node list. It stretches along an axis when one of its children does, so the
///        content means what its nodes would mean written inline in the holder's parent.
/// @param children The content.
/// @param gap      The spacing step between children.
/// @return The column; bounds and `alignSelf` stay unset, because they belong to a child.
[[nodiscard]] Column listColumn(std::vector<Node> children, int gap = 0) {
    std::vector<Sizing> widths;
    std::vector<Sizing> heights;
    for (Node const& child : children) {
        LayoutHints const hints = layoutHintsOf(child);
        widths.push_back(hints.width);
        heights.push_back(hints.height);
    }
    // A lone child's share of its row is the holder's, and the child fills the holder, so the share is taken once.
    if (children.size() == 1 && widths.front().kind == Sizing::Kind::Percent) {
        Sizing const share = widths.front();
        children.front() = withWidth(children.front(), Sizing::stretch(1));
        Column column{.children = std::move(children), .gap = gap};
        column.common.layout.width = share;
        column.common.layout.height = holderSizing(heights);
        return column;
    }
    Column column{.children = std::move(children), .gap = gap};
    column.common.layout.width = holderSizing(widths);
    column.common.layout.height = holderSizing(heights);
    return column;
}

}  // namespace

[[nodiscard]] Node lazy(std::function<Node()> build, LayoutHints const& hints) {
    Common common;
    common.layout = hints;
    return ui::forEach<int>([] { return std::vector<int>{0}; }, [](int const&) { return Key{std::int64_t{0}}; },
                            [build = std::move(build)](reactive::Signal<int> const&) { return build(); },
                            Axis::Vertical, 0, common);
}

[[nodiscard]] Node holding(std::vector<Node> children, std::shared_ptr<RtScope> scope, std::vector<KeyBinding> keys,
                           int gap) {
    Column column = listColumn(std::move(children), gap);
    column.common.visible = Prop<bool>{[held = std::move(scope)] { return held != nullptr; }};
    column.common.keys = std::move(keys);
    return ui::column(std::move(column));
}

// NOLINTBEGIN(misc-no-recursion): an expression nests its arguments and fields, and a document nests its nodes; the
// walks follow them.
[[nodiscard]] bool readsAny(ExprPtr const& expr, std::set<std::string, std::less<>> const& names) {
    if (!expr) {
        return false;
    }
    if (!expr->name.empty() && names.contains(expr->name)) {
        return true;
    }
    return std::ranges::any_of(expr->args, [&](ExprPtr const& arg) { return readsAny(arg, names); }) ||
           std::ranges::any_of(expr->fields, [&](auto const& field) { return readsAny(field.second, names); });
}

void collectIds(DocNode const& node, std::set<std::string, std::less<>>& out) {
    if (!node.id.empty()) {
        out.insert(node.id);
    }
    for (auto const* group : {&node.children, &node.parts}) {
        for (auto const& [key, list] : *group) {
            for (DocNodePtr const& child : list) {
                collectIds(*child, out);
            }
        }
    }
}
// NOLINTEND(misc-no-recursion)

// NOLINTNEXTLINE(misc-no-recursion): lowering a node lowers its children, which come back through here.
Node ScreenCore::lowerAll(std::span<DocNodePtr const> nodes, LowerCtx const& ctx, int gap) {
    LowerCtx listCtx = ctx;
    listCtx.listGap = gap;
    if (nodes.size() == 1) {
        return lower(*nodes.front(), listCtx);
    }
    std::vector<Node> children;
    children.reserve(nodes.size());
    for (DocNodePtr const& child : nodes) {
        children.push_back(lower(*child, listCtx));
    }
    // Several nodes share a holder, and a share of a row is not a share of a holder: each is reported and takes its
    // content size.
    for (auto&& [child, written] : std::views::zip(children, nodes)) {
        if (layoutHintsOf(child).width.kind == Sizing::Kind::Percent) {
            report(written->path,
                   "a percent among several nodes is a share of their holder; " + std::string{kPercentPlaces});
            child = withWidth(child, Sizing::content());
        }
    }
    Column holder = listColumn(std::move(children), gap);
    // The column is the mount's, not the document's: a child that bleeds reaches past the padding of the container
    // the document names, so the column passes that padding on instead of hiding it.
    holder.holder = true;
    return ui::column(std::move(holder));
}

}  // namespace morph::ui::detail
