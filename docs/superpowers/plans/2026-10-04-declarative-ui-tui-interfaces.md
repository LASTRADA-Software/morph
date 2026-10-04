# Declarative UI Program — Cross-Part Interface Contract

The public names each part **produces** and later parts **consume**. Part plans implement exactly these
declarations (Doxygen, `noexcept`, `[[nodiscard]]` and private members are each plan's to add); a part that needs a
name not listed here defines it in its own namespace and lists it in its plan's Interfaces block. If a plan must
deviate, its squashed commit states the deviation (master plan, "The docs commit").

Naming: types `CamelCase`, functions `camelBack`, private members `_camelBack`, constants `kName`. Strings are UTF-8.

---

## Part 0 — core-cpp 0.7 (`core::tui`, in the core-cpp repository)

```cpp
namespace core::tui {
enum class MouseTracking : std::uint8_t { Off, Buttons, Drag, AnyMotion };
// TerminalInput::setMouseTracking(MouseTracking); Terminal::setMouseTracking(MouseTracking);
// Screen::releasePointer(); [[nodiscard]] Component* Screen::componentAt(int row, int col) const;
}
```

morph: `MORPH_CORE_CPP_VERSION 0.7`, `GIT_TAG v0.7.0`.

## Part 1 — `morph::reactive` (`include/morph/reactive/`)

As Part 1's plan defines them: `Runtime`, `RuntimeOptions{maxEffectRunsPerFlush, afterFlush}` (`runtime.hpp`);
`Signal<T>`, `Computed<T>`, `Effect` (`signal.hpp`); `Scope` (`scope.hpp`); `ExhaustiveUpdate`,
`Store<ViewState, Msg>`, `request()` (`store.hpp`); `errorMessage`, `Refetchable`, `Query<A, R>`,
`MutationOptions`, `Mutation<A, R>`, `Subscription<R>` (`control.hpp`); `detail::site::*` misuse names. Plus:

```cpp
// include/morph/reactive/scheduler.hpp
namespace morph::reactive {
class TimerHandle {                         // move-only; cancels on destruction
public:
    TimerHandle() = default;
    explicit TimerHandle(std::function<void()> cancel);
    ~TimerHandle();
    TimerHandle(TimerHandle&&) noexcept; TimerHandle& operator=(TimerHandle&&) noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool active() const noexcept;
};
class Scheduler {                           // callbacks run on the runtime's owner executor
public:
    virtual ~Scheduler() = default;
    [[nodiscard]] virtual TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
    [[nodiscard]] virtual TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) = 0;
};
}
// include/morph/reactive/testing/manual_scheduler.hpp
namespace morph::reactive::testing {
class ManualScheduler final : public Scheduler {   // deterministic: time advances only when told
public:
    void advance(std::chrono::milliseconds by);    // fires due timers in deadline order, on the calling thread
    [[nodiscard]] std::size_t pendingTimers() const;
};
}
// control.hpp addition — last constructor parameter of both Query constructors
namespace morph::reactive {
struct QueryOptions {
    Scheduler* scheduler = nullptr;             // required when refreshEvery > 0
    std::chrono::milliseconds refreshEvery{0};  // 0 = no timed refresh
};
}
```

## Part 2 — `morph::ui` (`include/morph/ui/`)

### `view.hpp`

```cpp
namespace morph::ui {
using Action = std::function<void()>;
using Key = std::variant<std::int64_t, std::string>;
template <class T> class Prop;   // implicit from a non-callable convertible to T (constant) or a callable
                                 // returning one (binding); isBound(), constant(), binding(), evaluate()
enum class TextRole : std::uint8_t { Normal, Muted, Heading, Error, Success };
enum class TextInputMode : std::uint8_t { SingleLine, Multiline, Password };
enum class SelectStyle : std::uint8_t { Dropdown, Radio };
enum class Axis : std::uint8_t { Vertical, Horizontal };
enum class SelectionMode : std::uint8_t { None, Single, Multiple };
enum class DateMode : std::uint8_t { Date, DateTime };
enum class FilePickerMode : std::uint8_t { Open, Save };
struct Sizing { enum class Kind : std::uint8_t { Content, Fixed, Stretch }; Kind kind; int amount;
                static constexpr Sizing content(); static constexpr Sizing fixed(int units);
                static constexpr Sizing stretch(int weight = 1); bool operator==(Sizing const&) const = default; };
struct LayoutHints { Sizing width{}; Sizing height{}; bool operator==(LayoutHints const&) const = default; };
struct Common {
    Prop<bool> visible = true;
    Prop<bool> enabled = true;
    LayoutHints layout{};
    Prop<std::optional<Key>> dragKey;               // engaged: draggable, carrying the key
    std::function<bool(Key const&)> accepts;        // empty: accepts every key (when onDrop is set)
    std::function<void(Key)> onDrop;                // set: a drop target
};
struct NodeData; using Node = std::shared_ptr<NodeData const>;
struct Text { Prop<std::string> text; Prop<TextRole> role = TextRole::Normal; Common common{}; };
struct Button { Prop<std::string> label; Action onClick; Common common{}; };
struct TextInput { Prop<std::string> value; std::function<void(std::string)> onChange;
                   std::function<void(std::string)> onSubmit; Prop<std::string> placeholder;
                   TextInputMode mode = TextInputMode::SingleLine; Common common{}; };
struct Checkbox { Prop<std::string> label; Prop<bool> checked; std::function<void(bool)> onToggle; Common common{}; };
struct SelectOption { Key key; std::string label; bool operator==(SelectOption const&) const = default; };
struct Select { Prop<std::vector<SelectOption>> options; Prop<std::optional<Key>> selected;
                std::function<void(Key)> onSelect; SelectStyle style = SelectStyle::Dropdown; Common common{}; };
struct MenuItem { Prop<std::string> label; Action onSelect; };
struct Menu { std::vector<MenuItem> items; Common common{}; };
struct Column { std::vector<Node> children; int gap = 0; Common common{}; };
struct Row { std::vector<Node> children; int gap = 0; Common common{}; };
struct GridCell { Node node; int span = 1; };
struct Grid { int columns = 1; std::vector<GridCell> cells; int gap = 0; Common common{}; };
struct Spacer { Common common{}; };
struct Panel { Prop<std::string> title; int padding = 0; Node child; bool collapsible = false;
               Prop<bool> collapsed = false; std::function<void(bool)> onToggle; Common common{}; };
struct Scroll { Node child; Axis axis = Axis::Vertical; Common common{}; };
struct SwitchCase { Key key; Node node; };
struct Switch { Prop<Key> selector; std::vector<SwitchCase> cases; Node fallback; Common common{}; };
struct Tab { std::string label; Node node; };
struct Tabs { std::vector<Tab> tabs; Prop<std::size_t> selected; std::function<void(std::size_t)> onSelect; Common common{}; };
struct Dialog { Prop<bool> open; Prop<std::string> title; Node child; Action onDismiss; Common common{}; };
struct Busy { Prop<bool> active; Prop<std::string> label; Common common{}; };
namespace detail { class RowSlot; class ForEachSession; class ForEachModel; }   // as Part 2's plan defines
struct ForEach { std::shared_ptr<detail::ForEachModel const> model; Axis axis = Axis::Vertical; int gap = 0; Common common{}; };
struct TableColumn { std::string label; Sizing width = Sizing::content(); };
struct Table { std::vector<TableColumn> columns; std::shared_ptr<detail::ForEachModel const> rows;
               SelectionMode selectionMode = SelectionMode::None; Prop<std::vector<Key>> selection;
               std::function<void(std::vector<Key>)> onSelectionChange; std::function<void(Key)> onActivate;
               Common common{}; };   // each row view is a Row whose children are the cells
struct DateTimeInput { Prop<std::optional<morph::time::Timestamp>> value;
                       std::function<void(std::optional<morph::time::Timestamp>)> onChange;
                       DateMode mode = DateMode::DateTime; int offsetMinutes = 0; Common common{}; };
struct Slider { Prop<std::int64_t> value; std::int64_t minimum = 0; std::int64_t maximum = 100;
                std::int64_t step = 1; std::function<void(std::int64_t)> onChange; Common common{}; };
struct FilePicker { Prop<std::string> path; FilePickerMode mode = FilePickerMode::Open;
                    std::function<void(std::string)> onPicked; Common common{}; };
struct NodeData { std::variant<Text, Button, TextInput, Checkbox, Select, Menu, Column, Row, Grid, Spacer, Panel,
                               Scroll, Switch, Tabs, Dialog, Busy, ForEach, Table, DateTimeInput, Slider,
                               FilePicker> kind; };
// builders, one per kind:
Node text(Text); Node button(Button); Node textInput(TextInput); Node checkbox(Checkbox); Node select(Select);
Node menu(Menu); Node column(Column); Node row(Row); Node grid(Grid); Node spacer(Spacer = {}); Node panel(Panel);
Node scroll(Scroll); Node switchOf(Switch); Node tabs(Tabs); Node dialog(Dialog); Node busy(Busy);
Node dateTimeInput(DateTimeInput); Node slider(Slider); Node filePicker(FilePicker);
template <class E> requires std::is_enum_v<E>
Node switchOn(std::function<E()> selector, std::vector<std::pair<E, Node>> cases, Node fallback = {});
template <class RowT> Node forEach(std::function<std::vector<RowT>()> rows, std::function<Key(RowT const&)> keyOf,
    std::function<Node(reactive::Signal<RowT> const&)> rowView, Axis axis = Axis::Vertical, int gap = 0);
template <class RowT> Node forEach(reactive::Signal<std::vector<RowT>> const& rows,
    std::function<Key(RowT const&)> keyOf, std::function<Node(reactive::Signal<RowT> const&)> rowView,
    Axis axis = Axis::Vertical, int gap = 0);
struct TableOptions { SelectionMode selectionMode = SelectionMode::None; Prop<std::vector<Key>> selection;
                      std::function<void(std::vector<Key>)> onSelectionChange; std::function<void(Key)> onActivate;
                      Common common{}; };
template <class RowT> Node table(std::vector<TableColumn> columns, std::function<std::vector<RowT>()> rows,
    std::function<Key(RowT const&)> keyOf, std::function<std::vector<Node>(reactive::Signal<RowT> const&)> cells,
    TableOptions options = {});
}
```

### `backend.hpp`

```cpp
namespace morph::ui {
class Widget { public: virtual ~Widget() = default;
    virtual void setVisible(bool) = 0; virtual void setEnabled(bool) = 0; virtual void setLayout(LayoutHints const&) = 0;
    virtual void setDragKey(std::optional<Key> const&) = 0;
    virtual void setDropHandler(std::function<bool(Key const&)> accepts, std::function<void(Key)> onDrop) = 0; };
class ContainerWidget : public Widget { public: virtual void moveChild(Widget& child, std::size_t index) = 0; };
class TextWidget : public Widget { public: virtual void setText(std::string_view) = 0; virtual void setRole(TextRole) = 0; };
class ButtonWidget : public Widget { public: virtual void setLabel(std::string_view) = 0; virtual void setOnClick(Action) = 0; };
class TextInputWidget : public Widget { public: virtual void setText(std::string_view) = 0;   // never fires onChange
    virtual void setPlaceholder(std::string_view) = 0; virtual void setOnChange(std::function<void(std::string)>) = 0;
    virtual void setOnSubmit(std::function<void(std::string)>) = 0; };
class CheckboxWidget : public Widget { public: virtual void setLabel(std::string_view) = 0; virtual void setChecked(bool) = 0;
    virtual void setOnToggle(std::function<void(bool)>) = 0; };
class SelectWidget : public Widget { public: virtual void setOptions(std::vector<SelectOption> const&) = 0;
    virtual void setSelected(std::optional<Key> const&) = 0; virtual void setOnSelect(std::function<void(Key)>) = 0; };
class MenuWidget : public Widget { public: virtual void setItems(std::vector<std::string> const&) = 0;
    virtual void setOnActivate(std::function<void(std::size_t)>) = 0; };
class StackWidget : public ContainerWidget { public: virtual void setGap(int) = 0; };
class GridWidget : public ContainerWidget { public: virtual void setColumns(int) = 0; virtual void setGap(int) = 0;
    virtual void setSpan(Widget& child, int span) = 0; };
class SpacerWidget : public Widget {};
class PanelWidget : public ContainerWidget { public: virtual void setTitle(std::string_view) = 0; virtual void setPadding(int) = 0;
    virtual void setCollapsible(bool) = 0; virtual void setCollapsed(bool) = 0; virtual void setOnToggle(std::function<void(bool)>) = 0; };
class ScrollWidget : public ContainerWidget {};
class SlotWidget : public ContainerWidget {};
class TabsWidget : public ContainerWidget { public: virtual void setTabs(std::vector<std::string> const&) = 0;
    virtual void setSelected(std::size_t) = 0; virtual void setOnSelect(std::function<void(std::size_t)>) = 0; };
class DialogWidget : public ContainerWidget { public: virtual void setOpen(bool) = 0; virtual void setTitle(std::string_view) = 0;
    virtual void setOnDismiss(Action) = 0; };
class BusyWidget : public Widget { public: virtual void setActive(bool) = 0; virtual void setLabel(std::string_view) = 0; };
class TableWidget : public ContainerWidget { public: virtual void setColumns(std::vector<TableColumn> const&) = 0;
    virtual void setSelectionMode(SelectionMode) = 0; virtual void setRowKey(Widget& row, Key const& key) = 0;
    virtual void setSelection(std::vector<Key> const&) = 0;
    virtual void setOnSelectionChange(std::function<void(std::vector<Key>)>) = 0;
    virtual void setOnActivate(std::function<void(Key)>) = 0; };
class DateTimeInputWidget : public Widget { public: virtual void setValue(std::optional<morph::time::Timestamp> const&) = 0;
    virtual void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)>) = 0; };
class SliderWidget : public Widget { public: virtual void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) = 0;
    virtual void setValue(std::int64_t) = 0; virtual void setOnChange(std::function<void(std::int64_t)>) = 0; };
class FilePickerWidget : public Widget { public: virtual void setPath(std::string_view) = 0;
    virtual void setOnPicked(std::function<void(std::string)>) = 0; };
class IViewBackend { public: virtual ~IViewBackend() = default;   // each factory appends to parent (null: the root)
    virtual std::unique_ptr<TextWidget> createText(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<ButtonWidget> createButton(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<TextInputWidget> createTextInput(ContainerWidget* parent, TextInputMode mode) = 0;
    virtual std::unique_ptr<CheckboxWidget> createCheckbox(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<SelectWidget> createSelect(ContainerWidget* parent, SelectStyle style) = 0;
    virtual std::unique_ptr<MenuWidget> createMenu(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<StackWidget> createStack(ContainerWidget* parent, Axis axis) = 0;
    virtual std::unique_ptr<GridWidget> createGrid(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<SpacerWidget> createSpacer(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<PanelWidget> createPanel(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<ScrollWidget> createScroll(ContainerWidget* parent, Axis axis) = 0;
    virtual std::unique_ptr<SlotWidget> createSlot(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<TabsWidget> createTabs(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<DialogWidget> createDialog(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<BusyWidget> createBusy(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<TableWidget> createTable(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<DateTimeInputWidget> createDateTimeInput(ContainerWidget* parent, DateMode mode, int offsetMinutes) = 0;
    virtual std::unique_ptr<SliderWidget> createSlider(ContainerWidget* parent) = 0;
    virtual std::unique_ptr<FilePickerWidget> createFilePicker(ContainerWidget* parent, FilePickerMode mode) = 0; };
}
```

### `mount.hpp`, `frontend.hpp`

```cpp
namespace morph::ui {
namespace detail::site { inline constexpr char const* kDuplicateKey = "morph::ui: duplicate ForEach key"; }
class Mounted {                     // non-copyable, non-movable
public:
    Mounted(reactive::Runtime& runtime, IViewBackend& backend, Node root, ContainerWidget* parent = nullptr);
    [[nodiscard]] Widget& root() const noexcept;
};
using Scheduler = reactive::Scheduler;
using TimerHandle = reactive::TimerHandle;
class AppContext { public: virtual ~AppContext() = default;
    virtual reactive::Runtime& runtime() = 0; virtual exec::IExecutor& executor() = 0;
    virtual Scheduler& scheduler() = 0; virtual exec::IoLoop* ioLoop() = 0;   // null when the frontend has none
    virtual void quit(int exitCode = 0) = 0; [[nodiscard]] virtual std::string_view frontendName() const = 0; };
class Application { public: virtual ~Application() = default; [[nodiscard]] virtual Node view() = 0; };
using ApplicationFactory = std::function<std::unique_ptr<Application>(AppContext&)>;
class Frontend { public: virtual ~Frontend() = default; [[nodiscard]] virtual std::string_view name() const = 0;
    virtual int run(ApplicationFactory const& factory) = 0; };
struct FrontendOption { std::string name; std::function<bool()> usable; std::function<std::unique_ptr<Frontend>()> make; };
class FrontendSelectionError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
using EnvironmentReader = std::function<std::optional<std::string>(std::string_view name)>;
[[nodiscard]] EnvironmentReader processEnvironment();
// --ui=<name>, else MORPH_UI, else the first usable option in the given order; throws FrontendSelectionError
[[nodiscard]] std::unique_ptr<Frontend> selectFrontend(std::span<FrontendOption const> built, int argc,
    char const* const* argv, EnvironmentReader const& env = processEnvironment());
}
```

### `testing/recording_backend.hpp`, `testing/backend_conformance.hpp`

```cpp
namespace morph::ui::testing {
class RecordingBackend final : public IViewBackend {
public:
    [[nodiscard]] std::string dump() const;                     // golden tree: "Kind#id prop=value ..." indented
    [[nodiscard]] std::vector<std::string> const& log() const;  // "create Kind#id in Parent#id", "set Kind#id prop=v",
    void clearLog();                                            // "move Kind#id to n", "destroy Kind#id"
    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string_view prop, std::string_view value) const;
    [[nodiscard]] std::vector<int> all(std::string_view kind) const;
    [[nodiscard]] std::string prop(int id, std::string_view name) const;
    [[nodiscard]] bool exists(int id) const;
    void click(int id); void edit(int id, std::string text); void submit(int id, std::string text);
    void toggle(int id); void choose(int id, Key key); void chooseIndex(int id, std::size_t index);
    void collapse(int id, bool collapsed); void dismiss(int id);
    void selectRows(int id, std::vector<Key> keys); void activateRow(int id, Key key);
    void setDateTime(int id, std::optional<morph::time::Timestamp> value); void slide(int id, std::int64_t value);
    void pick(int id, std::string path);
    bool drag(int sourceId, int targetId);                       // true when the target accepted and onDrop ran
};
class ConformanceProbe {                                         // one per backend under test
public:
    virtual ~ConformanceProbe() = default;
    virtual IViewBackend& backend() = 0;
    virtual reactive::Runtime& runtime() = 0;
    virtual void settle() = 0;                                   // run posted flushes and native events until idle
    [[nodiscard]] virtual std::string textOf(Widget const& w) = 0;
    [[nodiscard]] virtual bool visibleOf(Widget const& w) = 0;
    [[nodiscard]] virtual bool enabledOf(Widget const& w) = 0;
    [[nodiscard]] virtual std::size_t childCount(ContainerWidget const& c) = 0;
    [[nodiscard]] virtual Widget const* childAt(ContainerWidget const& c, std::size_t index) = 0;
    virtual void click(Widget& w) = 0;
    virtual void type(Widget& w, std::string_view text) = 0;
    virtual void drag(Widget& source, Widget& target) = 0;
};
struct ConformanceCase { std::string_view name; std::function<std::optional<std::string>(ConformanceProbe&)> run; };
[[nodiscard]] std::span<ConformanceCase const> conformanceCases();   // nullopt = pass, else the failure message
}
```

## Part 3 — `IoLoopDriver::Caller` and `morph::tui`

```cpp
namespace morph::exec {
enum class IoLoopDriver : std::uint8_t { OwnThread, Caller };
// IoLoop: explicit IoLoop(IoLoopDriver driver = IoLoopDriver::OwnThread); [[nodiscard]] IoLoopDriver driver() const noexcept;
}
namespace morph::tui {
class LoopExecutor final : public exec::IExecutor { public: explicit LoopExecutor(::core::net::EventLoop& loop); };
struct FrontendConfig {
    ::core::tui::Terminal* terminal = nullptr;                    // null: the process terminal
    ::core::tui::runtime::InputSource* input = nullptr;          // null: the terminal's input
    ::core::tui::MouseTracking mouse = ::core::tui::MouseTracking::Drag;
};
inline constexpr int kInterruptExitCode = 130;                 // Frontend::run's result when Ctrl+C ended it
class Frontend final : public ui::Frontend { public: explicit Frontend(FrontendConfig config = {}); };
[[nodiscard]] ui::FrontendOption frontendOption(FrontendConfig config = {});   // name "tui"; usable: stdin is a tty
class Backend final : public ui::IViewBackend { public: explicit Backend(::core::tui::Screen& screen); };
}
```

CMake: option `MORPH_BUILD_TUI`, target `morph_tui` / `morph::tui`, install component `tui`.

## Part 4 — `morph::qt_quick`

```cpp
namespace morph::qt_quick {
class Frontend final : public ui::Frontend { public: Frontend(int& argc, char** argv); };
[[nodiscard]] ui::FrontendOption frontendOption(int& argc, char** argv,
                                              ui::EnvironmentReader env = ui::processEnvironment());   // name "qt"
class Backend final : public ui::IViewBackend { public: Backend(QQmlEngine& engine, QQuickItem& root); };
}
```

CMake: option `MORPH_BUILD_QT_QUICK` (requires `MORPH_BUILD_QT`), target `morph_qt_quick` / `morph::qt_quick`,
install component `qt_quick`, private QML module URI `MorphUi`.

## Part 5 — the forms engine (`include/morph/forms/engine/`; `parseDecimal` in `include/morph/util/rational.hpp`)

```cpp
namespace morph::math {
[[nodiscard]] std::optional<Rational> parseDecimal(std::string_view text, render::NumericLocale const& locale,
                                                   std::optional<std::uint32_t> maxDecimalPlaces = std::nullopt);
}
namespace morph::forms {
enum class FieldKind : std::uint8_t { Text, Multiline, Integer, Number, Decimal, Quantity, Boolean, Enum, Choice,
                                      DateTime, Date, Slider, Array, Object, ObjectArray };
enum class SubmitMode : std::uint8_t { Automatic, Explicit };
enum class Tri : std::uint8_t { False, True, Unknown };
struct SchemaError { std::string path; std::string message; };
struct FieldSpec;            // as spec 2 §2 lists; children for Object/ObjectArray
struct FieldGroupSpec;       // title, kind (GroupKind), field names
class RuleExpr;              // parsed x-rules
class FormModel { public:
    [[nodiscard]] static std::expected<FormModel, SchemaError> fromSchema(std::string_view actionType, std::string_view schemaJson);
    [[nodiscard]] std::string_view actionType() const noexcept; [[nodiscard]] std::string_view title() const noexcept;
    [[nodiscard]] std::span<FieldSpec const> fields() const noexcept; [[nodiscard]] std::span<FieldGroupSpec const> groups() const noexcept;
    [[nodiscard]] std::span<RuleExpr const> rules() const noexcept; [[nodiscard]] SubmitMode submitMode() const noexcept; };
// FormModel also has: template <class A> [[nodiscard]] static FormModel forAction();
//   = fromSchema(ActionTraits<A>::typeId(), schemaJson<A>()), throwing std::logic_error naming the schema path when
//   the engine cannot read the action's own schema (a programming error, not input).
using Submitter = std::function<async::Completion<std::string>(std::string_view actionType, std::string bodyJson)>;
using ChoiceFetcher = std::function<async::Completion<std::string>(std::string_view optionsAction, std::string bodyJson)>;
struct FormSessionOptions { render::NumericLocale locale{}; std::string bcp47 = "en"; render::TranslationProvider translations;
                            int displayOffsetMinutes = 0; };
class FieldState;            // text, unit, engaged (Signals); encoded, error, visible, readonly, required (Computeds); options (Choice)
class FormSession { public:
    FormSession(reactive::Runtime& runtime, FormModel model, Submitter submit, ChoiceFetcher choices, FormSessionOptions options = {});
    [[nodiscard]] FormModel const& model() const noexcept; [[nodiscard]] FieldState& field(std::string_view path);
    [[nodiscard]] bool ready() const; [[nodiscard]] std::optional<std::string> body() const;
    void submit(); void prefill(std::string_view bodyJson); void reset();
    [[nodiscard]] bool pending() const; [[nodiscard]] std::optional<std::string> const& lastReply() const;
    [[nodiscard]] std::exception_ptr lastError() const; };
template <class A, class M, class S = bridge::NoSharing> class Form { public:
    Form(reactive::Runtime& runtime, bridge::BridgeHandler<M, S>& handler, ChoiceFetcher choices, FormSessionOptions options = {});
    [[nodiscard]] FormSession& session() noexcept;
    template <auto Member> void set(/* the member's type */ auto value);
    [[nodiscard]] std::optional<A> value() const; [[nodiscard]] bool ready() const;
    [[nodiscard]] std::optional<typename model::ActionTraits<A>::Result> const& lastResult() const; };
[[nodiscard]] Submitter bridgeSubmitter(bridge::Bridge& bridge, exec::IExecutor& callbacks);
[[nodiscard]] ChoiceFetcher bridgeChoiceFetcher(bridge::Bridge& bridge, exec::IExecutor& callbacks);
// handler_submitter.hpp: route through the application's own handlers (in-memory or shared instances)
template <class... Handlers> [[nodiscard]] Submitter handlerSubmitter(exec::IExecutor& callbacks, Handlers&... handlers);
template <class... Handlers> [[nodiscard]] ChoiceFetcher handlerChoiceFetcher(exec::IExecutor& callbacks, Handlers&... handlers);
class FieldView;             // a field's spec and state, handed to an override
class Overrides { public:
    using Render = std::function<ui::Node(FieldView&)>;
    Overrides& byField(std::string path, Render); Overrides& byWidget(std::string hint, Render);
    Overrides& byUnit(std::string unit, Render); Overrides& byKind(FieldKind kind, Render); };
struct FormViewOptions { Overrides overrides; int gridColumns = 2; std::string submitLabel = "Submit"; };
[[nodiscard]] ui::Node formView(FormSession& session, FormViewOptions options = {});
class CollectionModel; class CollectionSession; struct CollectionViewOptions;
[[nodiscard]] ui::Node collectionView(CollectionSession& session, CollectionViewOptions options = {});
class WizardModel; class WizardSession;
[[nodiscard]] ui::Node wizardView(WizardSession& session);
class AppShellModel; class AppShellSession;
[[nodiscard]] ui::Node appShellView(AppShellSession& session);
}
namespace morph::app { template <FixedString Id, class View> struct ViewScreen; }   // kind "view" in app.hpp
```

Part 5's plan gives the full declarations of the forward-declared classes; parts 8–10 use only what it lists.

## Part 6 — examples/common (Qt-free; target `morph_ladder_app_common`, headers `examples/common/app/`)

```cpp
namespace morph::examples {
struct AppEnvironment { std::optional<std::string> server; std::string db; std::string user; bool seed = false;
                        std::optional<std::string> pollId;   // polls' ?poll= (WASM) / --poll (native)
                        [[nodiscard]] static AppEnvironment fromArgs(int argc, char const* const* argv); };
class Connection { public:                                    // owns the pool (local) and the Bridge
    [[nodiscard]] bridge::Bridge& bridge() noexcept; [[nodiscard]] exec::IExecutor& callbacks() noexcept; };
struct LocalSetup { std::function<void(std::string const& database)> setupDatabase; std::size_t workers = 4; };
class TransportError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
[[nodiscard]] std::unique_ptr<Connection> connect(ui::AppContext& ctx, AppEnvironment const& env, LocalSetup local);
[[nodiscard]] std::string newUuid();                          // RFC 4122 v4, lower-case
struct Wiring {                                               // examples/common/app/wiring.hpp; all four borrowed
    reactive::Runtime& runtime; reactive::Scheduler& scheduler; bridge::Bridge& bridge; exec::IExecutor& callbacks; };
// examples/common/app/completion_map.hpp: derive a completion on the owner, gated by token; a throw from onValue fails
// the derived completion; onError observes a failure before it is passed on.
template <class To, class From, class OnValue, class OnError>
[[nodiscard]] async::Completion<To> mapCompletion(exec::IExecutor& owner, async::CallbackToken token,
                                                  async::Completion<From> from, OnValue onValue, OnError onError);
template <class Event, class Cursor> class Poller;            // "events since cursor" over a Query with refreshEvery
}
namespace morph::examples::testing {
template <class Pred> bool pumpUntil(exec::MainThreadExecutor& owner, Pred done,
                                     std::chrono::milliseconds budget = std::chrono::milliseconds{2000});
enum class SmokeFrontend : std::uint8_t { Tui, QtQuick };
void runFrontendSmoke(ui::ApplicationFactory const& factory, SmokeFrontend frontend);   // mounts, quits, asserts exit 0
}
```

`examples/common/app/` also holds the remote transport implementations: `QtWebSocketBackend` when the frontend is
`"qt"` (compiled only when `MORPH_BUILD_QT`), `morph::net::SocketBackend` on `ctx.ioLoop()` when it is `"tui"`
(compiled only when `MORPH_BUILD_NET` on POSIX); `connect` throws `TransportError` when `--server` is given and
neither applies.

## Conventions for every example application (Parts 6–10)

- Client code lives in `<app>::client` (bank: `bank::client`, because `bank::app` is the domain library's `App`).
- Each app exposes `[[nodiscard]] std::unique_ptr<ui::Application> <app>::client::makeApplication(ui::AppContext& ctx,
  examples::AppEnvironment const& env)`; app-specific knobs come from `env` or are defaulted, so every `ui/main.cpp`
  has the shape of spec 4 §3.
- View tests use `RecordingBackend`'s spellings exactly as Part 2 defines them (e.g. stacks log as `Column`/`Row`).
