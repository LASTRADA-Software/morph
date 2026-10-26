// SPDX-License-Identifier: Apache-2.0

#include <QAccessible>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

namespace {

/// A relay handler that hands the payload's text to @p handler.
std::function<void(QVariant const&)> textHandler(std::function<void(std::string)> handler) {
    return [handler = std::move(handler)](QVariant const& payload) {
        if (handler) {
            handler(toUtf8(payload.toString()));
        }
    };
}

/// Declares the option a drop-down shows as the value of its display child, the style's text field: a ComboBox node
/// has no value of its own, and Qt Quick Controls declares that child only while the platform's accessibility is on,
/// which on some platforms it never is. Set from C++ because QML can write an attached property only on the item
/// that declares it. A style sets its content item once, when the combo box is made.
void exposeShownOption(QObject const* comboBox) {
    auto* const shown = comboBox != nullptr ? comboBox->property("contentItem").value<QQuickItem*>() : nullptr;
    if (shown == nullptr) {
        return;
    }
    QQmlProperty const role{shown, QStringLiteral("Accessible.role"), qmlContext(shown)};
    if (role.isValid()) {
        static_cast<void>(role.write(static_cast<int>(QAccessible::EditableText)));
    }
}

}  // namespace

// ── Text ──────────────────────────────────────────────────────────────────

TextImpl::TextImpl(Context& context, ui::ContainerWidget* parent)
    : FieldItemWidget{context, QStringLiteral("MorphText"), parent} {}

void TextImpl::setText(std::string_view text) {
    write("richText", QString{});
    write("text", toQString(text));
}

void TextImpl::setDescription(std::string_view description) {
    static_cast<void>(description);
    throw std::logic_error{"morph::qt_quick: a text is not described"};
}

void TextImpl::setTone(ui::Tone tone) {
    static_cast<void>(tone);
    throw std::logic_error{"morph::qt_quick: a text is not toned"};
}

void TextImpl::setSpans(std::vector<ui::TextSpan> const& spans) {
    write("text", toQString(ui::spansText(spans)));
    write("richText", richTextOf(spans, monoFamiliesOf(context())));
}

QString richTextOf(std::vector<ui::TextSpan> const& spans, QStringList const& monoFamilies) {
    // A family name sits in a quoted CSS string inside an HTML attribute: escaped for the string, then the attribute.
    QStringList quoted;
    for (QString family : monoFamilies) {
        family.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('\''), QStringLiteral("\\'"));
        quoted.append(QLatin1Char('\'') + family + QLatin1Char('\''));
    }
    QString const monoOpen = QStringLiteral("<span style=\"font-family:%1\">")
                                 .arg((quoted.isEmpty() ? QStringLiteral("monospace") : quoted.join(QLatin1Char(',')))
                                          .toHtmlEscaped());
    QString html = QStringLiteral("<span style=\"white-space:pre-wrap\">");
    for (ui::TextSpan const& span : spans) {
        QString run = toQString(span.text).toHtmlEscaped();
        if (span.strong) {
            run = QStringLiteral("<b>") + run + QStringLiteral("</b>");
        }
        if (span.mono) {
            run = monoOpen + run + QStringLiteral("</span>");
        }
        html += run;
    }
    return html + QStringLiteral("</span>");
}

QStringList monoFamiliesOf(Context const& context) {
    QObject const* const theme =
        context.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    return theme != nullptr ? theme->property("monoFamilies").toStringList() : QStringList{};
}

void TextImpl::setRole(ui::TextRole role) { write("role", static_cast<int>(role)); }

std::string TextImpl::shownText() const { return toUtf8(read("text").toString()); }

// ── TextInput ─────────────────────────────────────────────────────────────

namespace {

QVariantMap textInputProperties(ui::TextInputMode mode) {
    QVariantMap properties;
    if (mode == ui::TextInputMode::Password) {
        properties.insert(QStringLiteral("password"), true);
    }
    return properties;
}

}  // namespace

TextInputImpl::TextInputImpl(Context& context, ui::ContainerWidget* parent, ui::TextInputMode mode)
    : FieldItemWidget{
          context,
          mode == ui::TextInputMode::Multiline ? QStringLiteral("MorphTextArea") : QStringLiteral("MorphTextInput"),
          parent, textInputProperties(mode)},
      _multiline{mode == ui::TextInputMode::Multiline} {}

void TextInputImpl::setText(std::string_view text) {
    QString const next = toQString(text);
    QObject* const control = part("control");
    if (control == nullptr || control->property("text").toString() == next) {
        return;
    }
    if (_multiline) {
        call("assign", next);
    } else {
        control->setProperty("text", next);
    }
}

void TextInputImpl::setPlaceholder(std::string_view placeholder) { write("placeholder", toQString(placeholder)); }

// MorphTextArea declares neither property; the mount never asks a multiline input for them.
void TextInputImpl::setUnit(std::string_view unit) {
    if (_multiline) {
        throw std::logic_error{"morph::qt_quick: a multiline text input shows no unit"};
    }
    write("unit", toQString(unit));
}

void TextInputImpl::setDecimals(int decimals) {
    if (_multiline) {
        throw std::logic_error{"morph::qt_quick: a multiline text input takes no places"};
    }
    write("decimals", decimals);
}

void TextInputImpl::setIcon(std::string_view icon) {
    if (_multiline) {
        throw std::logic_error{"morph::qt_quick: a multiline text input shows no icon"};
    }
    context().checkIcon(icon);
    write("icon", toQString(icon));
}

void TextInputImpl::setOnChange(std::function<void(std::string)> onChange) {
    relay().on(QStringLiteral("change"), textHandler(std::move(onChange)));
}

// The field reads `submits` to decide whether Enter stops at it; a multiline input, which never submits, declares no
// such property, so it is not written there.
void TextInputImpl::setOnSubmit(std::function<void(std::string)> onSubmit) {
    if (!_multiline) {
        write("submits", static_cast<bool>(onSubmit));
    }
    relay().on(QStringLiteral("submit"), textHandler(std::move(onSubmit)));
}

void TextInputImpl::setOnCommit(std::function<void(std::string)> onCommit) {
    relay().on(QStringLiteral("commit"), textHandler(std::move(onCommit)));
}

std::string TextInputImpl::shownText() const {
    QObject const* const control = part("control");
    if (control == nullptr) {
        return {};
    }
    return toUtf8(control->property("text").toString());
}

std::size_t TextInputImpl::cursor() const {
    QObject const* const control = part("control");
    if (control == nullptr) {
        return 0;
    }
    QString const text = control->property("text").toString();
    qsizetype const position = std::clamp<qsizetype>(control->property("cursorPosition").toInt(), 0, text.size());
    return static_cast<std::size_t>(text.left(position).toUtf8().size());
}

void TextInputImpl::moveCursor(std::size_t position) const {
    QObject* const control = part("control");
    if (control == nullptr) {
        return;
    }
    QByteArray const bytes = control->property("text").toString().toUtf8();
    auto const clamped =
        static_cast<qsizetype>(std::min<std::size_t>(position, static_cast<std::size_t>(bytes.size())));
    control->setProperty("cursorPosition", static_cast<int>(QString::fromUtf8(bytes.left(clamped)).size()));
}

// ── Checkbox ──────────────────────────────────────────────────────────────

CheckboxImpl::CheckboxImpl(Context& context, ui::ContainerWidget* parent)
    : FieldItemWidget{context, QStringLiteral("MorphCheckbox"), parent} {}

void CheckboxImpl::setLabel(std::string_view label) { write("label", toQString(label)); }

void CheckboxImpl::setChecked(bool checked) {
    QObject* const control = part("control");
    if (control != nullptr && control->property("checked").toBool() != checked) {
        control->setProperty("checked", checked);
    }
}

void CheckboxImpl::setOnToggle(std::function<void(bool)> onToggle) {
    relay().on(QStringLiteral("toggle"), [onToggle = std::move(onToggle)](QVariant const& payload) {
        if (onToggle) {
            onToggle(payload.toBool());
        }
    });
}

std::string CheckboxImpl::shownText() const { return toUtf8(read("label").toString()); }

bool CheckboxImpl::checked() const {
    QObject const* const control = part("control");
    return control != nullptr && control->property("checked").toBool();
}

// ── Select ────────────────────────────────────────────────────────────────

SelectImpl::SelectImpl(Context& context, ui::ContainerWidget* parent, ui::SelectStyle style)
    : FieldItemWidget{context, QStringLiteral("MorphSelect"), parent,
                      QVariantMap{{QStringLiteral("radio"), style == ui::SelectStyle::Radio}}} {
    if (style == ui::SelectStyle::Dropdown) {
        exposeShownOption(part("control"));
    }
}

void SelectImpl::setOptions(std::vector<ui::SelectOption> const& options) {
    if (options == _options) {
        return;
    }
    _options = options;
    QStringList labels;
    labels.reserve(static_cast<qsizetype>(options.size()));
    for (ui::SelectOption const& option : options) {
        labels.append(toQString(option.label));
    }
    write("options", labels);
    // A new list resets what the control marks; the requested key is marked again wherever it now is.
    call("mark", -1);
    markRequested();
}

void SelectImpl::setSelected(std::optional<ui::Key> const& key) {
    _requested = key;
    markRequested();
}

void SelectImpl::markRequested() const {
    int index = -1;
    if (_requested) {
        auto const found = std::ranges::find(_options, *_requested, &ui::SelectOption::key);
        if (found != _options.end()) {
            index = static_cast<int>(std::distance(_options.begin(), found));
        }
    }
    if (marked() != index) {
        call("mark", index);
    }
}

void SelectImpl::setOnSelect(std::function<void(ui::Key)> onSelect) {
    relay().on(QStringLiteral("select"), [this, onSelect = std::move(onSelect)](QVariant const& payload) {
        int const index = payload.toInt();
        if (index < 0 || static_cast<std::size_t>(index) >= _options.size()) {
            return;
        }
        ui::Key key = _options.at(static_cast<std::size_t>(index)).key;
        _requested = key;
        if (onSelect) {
            onSelect(std::move(key));
        }
    });
}

void SelectImpl::setOnOpen(ui::Action onOpen) {
    relay().on(QStringLiteral("open"), [onOpen = std::move(onOpen)](QVariant const&) {
        if (onOpen) {
            onOpen();
        }
    });
}

int SelectImpl::marked() const { return read("marked").toInt(); }

std::string SelectImpl::shownText() const {
    int const index = marked();
    if (index < 0 || static_cast<std::size_t>(index) >= _options.size()) {
        return {};
    }
    return _options.at(static_cast<std::size_t>(index)).label;
}

// ── DateTimeInput ─────────────────────────────────────────────────────────

QString formatDateTime(morph::time::DateTime const& instant, ui::DateMode mode, int offsetMinutes) {
    auto const local = instant.value + std::chrono::minutes{offsetMinutes};
    auto const day = std::chrono::floor<std::chrono::days>(local);
    std::chrono::year_month_day const date{day};
    QString text = QStringLiteral("%1-%2-%3")
                       .arg(static_cast<int>(date.year()), 4, 10, QLatin1Char('0'))
                       .arg(static_cast<unsigned>(date.month()), 2, 10, QLatin1Char('0'))
                       .arg(static_cast<unsigned>(date.day()), 2, 10, QLatin1Char('0'));
    if (mode == ui::DateMode::DateTime) {
        std::chrono::hh_mm_ss const clock{std::chrono::floor<std::chrono::minutes>(local - day)};
        text += QStringLiteral(" %1:%2")
                    .arg(clock.hours().count(), 2, 10, QLatin1Char('0'))
                    .arg(clock.minutes().count(), 2, 10, QLatin1Char('0'));
    }
    return text;
}

std::optional<morph::time::DateTime> parseDateTime(QString const& text, ui::DateMode mode, int offsetMinutes) {
    static QRegularExpression const dateOnly{QStringLiteral(R"(^\s*(\d{4})-(\d{2})-(\d{2})\s*$)")};
    static QRegularExpression const withTime{QStringLiteral(R"(^\s*(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})\s*$)")};
    QRegularExpressionMatch const match = (mode == ui::DateMode::DateTime ? withTime : dateOnly).match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    std::chrono::year_month_day const date{std::chrono::year{match.captured(1).toInt()},
                                           std::chrono::month{match.captured(2).toUInt()},
                                           std::chrono::day{match.captured(3).toUInt()}};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::chrono::minutes timeOfDay{0};
    if (mode == ui::DateMode::DateTime) {
        int const hours = match.captured(4).toInt();
        int const minutes = match.captured(5).toInt();
        if (hours > 23 || minutes > 59) {
            return std::nullopt;
        }
        timeOfDay = std::chrono::hours{hours} + std::chrono::minutes{minutes};
    }
    auto const local = std::chrono::sys_days{date} + timeOfDay;
    return morph::time::DateTime{
        std::chrono::time_point_cast<std::chrono::milliseconds>(local - std::chrono::minutes{offsetMinutes})};
}

DateTimeImpl::DateTimeImpl(Context& context, ui::ContainerWidget* parent, ui::DateMode mode, int offsetMinutes)
    : FieldItemWidget{context, QStringLiteral("MorphDateTimeInput"), parent,
                      QVariantMap{{QStringLiteral("withTime"), mode == ui::DateMode::DateTime},
                                  {QStringLiteral("offsetMinutes"), offsetMinutes}}},
      _mode{mode},
      _offsetMinutes{offsetMinutes} {
    relay().on(QStringLiteral("edit"), [this](QVariant const& payload) {
        QString const text = payload.toString().trimmed();
        if (text.isEmpty()) {
            _shown.clear();
            if (_onChange) {
                _onChange(std::nullopt);
            }
            return;
        }
        std::optional<morph::time::DateTime> const parsed = parseDateTime(text, _mode, _offsetMinutes);
        if (!parsed) {
            // Text that names no instant is not a request: the field shows its value again. Forced, because the
            // user's text may already read the same as the held value, and the held value stays for the next edit.
            show(_shown, true);
            return;
        }
        _shown = text;
        if (_onChange) {
            _onChange(morph::time::Timestamp{*parsed});
        }
    });
}

void DateTimeImpl::show(QString const& text, bool force) const {
    QObject* const control = part("control");
    if (control != nullptr && (force || control->property("text").toString() != text)) {
        control->setProperty("text", text);
    }
}

void DateTimeImpl::setValue(std::optional<morph::time::Timestamp> const& value) {
    QString text;
    if (value && value->value) {
        text = formatDateTime(*value->value, _mode, _offsetMinutes);
    }
    _shown = text;
    show(text);
}

void DateTimeImpl::setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) {
    _onChange = std::move(onChange);
}

std::string DateTimeImpl::shownText() const {
    QObject const* const control = part("control");
    if (control == nullptr) {
        return {};
    }
    return toUtf8(control->property("text").toString());
}

// ── Slider ────────────────────────────────────────────────────────────────

SliderImpl::SliderImpl(Context& context, ui::ContainerWidget* parent)
    : FieldItemWidget{context, QStringLiteral("MorphSlider"), parent} {}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the signature of ui::SliderWidget::setRange, overridden.
void SliderImpl::setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) {
    QObject* const control = part("control");
    if (control == nullptr) {
        return;
    }
    control->setProperty("from", static_cast<double>(minimum));
    control->setProperty("to", static_cast<double>(maximum));
    control->setProperty("stepSize", static_cast<double>(step));
}

void SliderImpl::setValue(std::int64_t value) {
    if (this->value() != value) {
        call("show", static_cast<double>(value));
    }
}

void SliderImpl::setOnChange(std::function<void(std::int64_t)> onChange) {
    relay().on(QStringLiteral("change"), [onChange = std::move(onChange)](QVariant const& payload) {
        if (onChange) {
            onChange(payload.toLongLong());
        }
    });
}

void SliderImpl::setOnCommit(std::function<void(std::int64_t)> onCommit) {
    relay().on(QStringLiteral("commit"), [onCommit = std::move(onCommit)](QVariant const& payload) {
        if (onCommit) {
            onCommit(payload.toLongLong());
        }
    });
}

std::int64_t SliderImpl::value() const {
    QObject const* const control = part("control");
    return control != nullptr ? static_cast<std::int64_t>(std::round(control->property("value").toDouble())) : 0;
}

// ── FilePicker ────────────────────────────────────────────────────────────

std::string localPath(QString const& url) {
    QUrl const parsed{url};
    if (parsed.isLocalFile()) {
        return toUtf8(parsed.toLocalFile());
    }
    return toUtf8(url);
}

FilePickerImpl::FilePickerImpl(Context& context, ui::ContainerWidget* parent, ui::FilePickerMode mode)
    : FieldItemWidget{context, QStringLiteral("MorphFilePicker"), parent,
                      QVariantMap{{QStringLiteral("save"), mode == ui::FilePickerMode::Save}}} {}

void FilePickerImpl::setPath(std::string_view path) { write("path", toQString(path)); }

void FilePickerImpl::setOnPicked(std::function<void(std::string)> onPicked) {
    relay().on(QStringLiteral("picked"), [this, onPicked = std::move(onPicked)](QVariant const& payload) {
        std::string path = localPath(payload.toString());
        write("path", toQString(path));
        if (onPicked) {
            onPicked(std::move(path));
        }
    });
}

std::string FilePickerImpl::shownText() const { return toUtf8(read("path").toString()); }

}  // namespace morph::qt_quick::detail
